#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <range/v3/view/iota.hpp>
#include <range/v3/view/zip_with.hpp>

#include <fmt/format.h>

#include <ztd/out_ptr.hpp>

#if defined(__unix__) || defined(__unix) ||                                    \
    (defined(__APPLE__) && defined(__MACH__))
#define UNIX
#endif

#if defined(UNIX)
#include <DeckLinkAPIDispatch.cpp>
#elif defined(WIN32)
#include <DeckLinkAPI_i.c>
#include <DeckLinkAPI_i.h>
#endif

#include "ipc_shared_object.hpp"
#include "server/server.hpp"
#include "triple_buffer.hpp"

using fmt::operator""_a;

using namespace std::literals;

struct DeckLinkRelease {
  void operator()(IUnknown *p) {
    if (p != nullptr) {
      p->Release();
    }
  }
};

template <typename T> using decklink_ptr = std::unique_ptr<T, DeckLinkRelease>;

template <typename T> auto Makedecklink_ptr(T *p) { return decklink_ptr<T>{p}; }

struct DLString {
#if defined(__linux__)
  char const *data;
#elif defined(__APPLE__) && defined(__MACH__)
  CFStringRef data;
#elif defined(WIN32)
  BSTR data;
#endif

  DLString() = default;
  DLString(DLString const &) = delete;

  void print() const {
#if defined(__linux__)
    std::cerr << data;
#elif defined(__APPLE__) && defined(__MACH__)
    std::cerr << CFStringGetCStringPtr(data, kCFStringEncodingASCII);
#elif defined(WIN32)
    std::wcerr << data;
#endif
  }

  ~DLString() {
#if defined(__linux__)
    free(const_cast<char *>(data));
#elif defined(__APPLE__) && defined(__MACH__)
    CFRelease(data);
#elif defined(WIN32)
    SysFreeString(data);
#endif
  }
};

template <> struct fmt::formatter<DLString> : fmt::formatter<char const *> {
  auto format(DLString const &str, auto &ctx) const -> decltype(ctx.out()) {
#if defined(__linux__)
    return static_cast<fmt::formatter<char const *> const &>(*this).format(
        str.data, ctx);
#elif defined(__APPLE__) && defined(__MACH__)
    return static_cast<fmt::formatter<char const *> const &>(*this).format(
        CFStringGetCStringPtr(str.data, kCFStringEncodingASCII), ctx);
#elif defined(WIN32)
    return static_cast<fmt::formatter<wchar_t const *> const &>(*this).format(
        str.data, ctx);
#endif
  }
};

using ztd::out_ptr::out_ptr;

template <typename T> auto find_if(auto &&it, auto const &f) {
  auto x = decklink_ptr<T>{};
  while (it->Next(out_ptr(x)) == S_OK) {
    if (f(x)) {
      return x;
    }
  }
  return decklink_ptr<T>{};
}

class output_frame : public IDeckLinkVideoFrame {
private:
  triple_buffer::buffer const &buffer;

public:
  output_frame(triple_buffer::buffer const &buffer) : buffer{buffer} {}

  auto QueryInterface(REFIID id, void **outputInterface) -> HRESULT override {
    *outputInterface = nullptr;
    return E_NOINTERFACE;
  }

  auto AddRef() -> ULONG override { return 0; }
  auto Release() -> ULONG override { return 0; }

  auto GetWidth() -> long override { return 1920; }
  auto GetHeight() -> long override { return 1080; }
  auto GetRowBytes() -> long override { return 1920 * 4; }
  auto GetPixelFormat() -> BMDPixelFormat override { return bmdFormat8BitBGRA; }
  auto GetFlags() -> BMDFrameFlags override { return bmdFormat8BitBGRA; }
  auto GetBytes(void **_buffer) -> HRESULT override {
    *_buffer = const_cast<uint8_t *>(buffer.data());
    return S_OK;
  }
  auto GetTimecode(BMDTimecodeFormat format, IDeckLinkTimecode **timecode)
      -> HRESULT override {
    *timecode = nullptr;
    return S_FALSE;
  }
  auto GetAncillaryData(IDeckLinkVideoFrameAncillary **ancillary)
      -> HRESULT override {
    *ancillary = nullptr;
    return S_FALSE;
  }
};

class active_decklink {
private:
  decklink_ptr<IDeckLinkOutput> decklink_output;
  decklink_ptr<IDeckLinkKeyer> decklink_keyer;

public:
  active_decklink(IDeckLink &decklink, bool external_keyer) {
    decklink_output = decklink_ptr<IDeckLinkOutput>{};
    if (decklink.QueryInterface(IID_IDeckLinkOutput,
                                out_ptr(decklink_output)) != S_OK) {
      std::cerr << "Could not get a DeckLink output\n";
      std::terminate();
    }

    decklink_keyer = decklink_ptr<IDeckLinkKeyer>{};
    if (decklink.QueryInterface(IID_IDeckLinkKeyer, out_ptr(decklink_keyer)) !=
        S_OK) {
      std::cerr << "Could not get a DeckLink keyer\n";
      std::terminate();
    }

    if (decklink_output->EnableVideoOutput(bmdModeHD1080p25,
                                           bmdVideoOutputFlagDefault) != S_OK) {
      std::cerr << "Could not enable video output\n";
      std::terminate();
    }

    if (decklink_keyer->Enable(external_keyer) != S_OK ||
        decklink_keyer->SetLevel(255) != S_OK) {
      std::cerr << "Could not enable keyer\n";
      std::terminate();
    }
  }

  ~active_decklink() {
    decklink_keyer->Disable();
    decklink_output->DisableVideoOutput();
  }

  void display_frame(triple_buffer::buffer const &buffer) {
    auto frame = output_frame{buffer};
    decklink_output->DisplayVideoFrameSync(&frame);
  }
};

struct decklink_option {
  bool selected;
  std::size_t index;
  IDeckLink &decklink;

  static constexpr auto make(std::optional<std::size_t> selected_index) {
    return
        [selected_index](std::size_t index, decklink_ptr<IDeckLink> &decklink) {
          return decklink_option{selected_index == index, index, *decklink};
        };
  }
};

template <> struct fmt::formatter<decklink_option> {
  constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
    if (ctx.begin() != ctx.end() && *ctx.begin() != '}')
      throw format_error("invalid format");
    return ctx.begin();
  }

  auto format(decklink_option const &decklink, auto &ctx) const
      -> decltype(ctx.out()) {
    auto name = DLString{};
    decklink.decklink.GetDisplayName(&name.data);
    return fmt::format_to(
        ctx.out(),
        R"html(<option value="{index}" {selected}>{name}</option>)html",
        "index"_a = decklink.index,
        "selected"_a = decklink.selected ? "selected"sv : ""sv,
        "name"_a = name);
  }
};

template <typename ReloadDecklink> class http_delegate {
public:
  using body_type = beast::http::string_body;

private:
  std::vector<decklink_ptr<IDeckLink>> &decklinks;
  std::optional<std::size_t> &decklink_index;
  bool &external_keyer;

  ReloadDecklink reload_decklink;

public:
  std::function<void()> reload_clients = [] {};

  http_delegate(std::vector<decklink_ptr<IDeckLink>> &decklinks,
                std::optional<std::size_t> &decklink_index,
                bool &external_keyer, ReloadDecklink reload_decklink)
      : decklinks{decklinks}, decklink_index{decklink_index},
        external_keyer{external_keyer}, reload_decklink{
                                            std::move(reload_decklink)} {}

  template <typename Body, typename Allocator>
  void handle_request(
      beast::http::request<Body, beast::http::basic_fields<Allocator>> &&req,
      auto &&send) {
    if (req.target() == "/") {
      auto body = fmt::format(
          R"html(
<html>
  <head>
  </head>
  <body>
    Decklink
    <select onchange="fetch('/decklink', {{method: 'POST', body: event.target.value}})">
      <option value="-1"> - </option>
      {decklinks}
    </select>
    <br/>
    Keyer
    <select onchange="fetch('/keyer', {{method: 'POST', body: event.target.value}})">
      <option value="internal" {internal_selected}>Internal</option>
      <option value="external" {external_selected}>External</option>
    </select>
    <script>
      let ws;
      
      function open_ws() {{
        ws = new WebSocket(`ws://${{window.location.host}}`);
        ws.onopen = function(ev) {{}};
        ws.onclose = function(ev) {{
          console.log(`Close: ${{ev}}`);
        }};
        ws.onmessage = function(ev) {{
          window.location.reload();
        }};
        ws.onerror = function(ev) {{
          console.log(`Error: ${{ev}}`);
          open_ws();
        }};
      }}

      open_ws();
    </script>
  </body>
</html>
)html"sv,
          "decklinks"_a = fmt::join(
              ranges::views::zip_with(decklink_option::make(decklink_index),
                                      ranges::views::iota(0ul), decklinks),
              ""),
          "internal_selected"_a = external_keyer ? ""sv : "selected"sv,
          "external_selected"_a = external_keyer ? "selected"sv : ""sv);
      auto mime_type = "text/html"sv;

      return http::string_response(req, std::move(body), mime_type, send);
    } else if (req.target() == "/decklink" &&
               req.method() == beast::http::verb::post) {
      auto index = std::stol(req.body());
      if (index >= 0 && static_cast<std::size_t>(index) < decklinks.size()) {
        decklink_index.emplace(static_cast<std::size_t>(index));
      } else {
        decklink_index = std::nullopt;
      }
      reload_decklink();
      reload_clients();
      return send(http::empty_response(req));
    } else if (req.target() == "/keyer" &&
               req.method() == beast::http::verb::post) {
      external_keyer = req.body() == "external"sv;
      reload_decklink();
      reload_clients();
      return send(http::empty_response(req));
    } else {
      return send(http::not_found(req));
    }
  }
};

class websocket_delegate {
public:
  void on_read(beast::flat_buffer &buffer) {
    // TODO
  }
};

int main([[maybe_unused]] int argc, char **argv) {
  auto decklinks = [&] {
#if defined(UNIX)
    auto decklinkIterator = Makedecklink_ptr(CreateDeckLinkIteratorInstance());
#elif defined(WIN32)
    if (CoInitialize(nullptr) != S_OK) {
      std::cerr << "Could not initialise COM (Windows)\n";
      std::terminate();
    }
    auto decklinkIterator = decklink_ptr<IDeckLinkIterator>{};
    if (CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL,
                         IID_IDeckLinkIterator,
                         out_ptr(decklinkIterator)) != S_OK) {
      std::cerr << "Could not get a DeckLink Iterator (Windows)\n";
      std::terminate();
    }
#endif

    if (decklinkIterator == nullptr) {
      std::cerr << "Could not get a DeckLink Iterator\n";
      std::terminate();
    }

    auto decklinks = std::vector<decklink_ptr<IDeckLink>>{};
    {
      auto decklink = decklink_ptr<IDeckLink>{};
      while (decklinkIterator->Next(out_ptr(decklink)) == S_OK) {
        decklinks.push_back(std::move(decklink));
      }
    }
    return decklinks;
  }();

  auto decklink_index = std::optional<std::size_t>{};
  auto external_keyer = false;
  auto decklink = std::optional<active_decklink>{};

  auto reload_decklink = [&] {
    if (decklink_index) {
      decklink.emplace(*decklinks[*decklink_index], external_keyer);
    } else {
      decklink = std::nullopt;
    }
  };

  /*
  auto displayMode = [&] {
    auto displayModeIterator = decklink_ptr<IDeckLinkDisplayModeIterator>{};
    if (decklinkOutput->GetDisplayModeIterator(out_ptr(displayModeIterator))
  != S_OK) { std::cerr << "Could not get a display mode iterator\n";
      std::terminate();
    }

    auto modes = std::vector<decklink_ptr<IDeckLinkDisplayMode>>{};
    {
      auto mode = decklink_ptr<IDeckLinkDisplayMode>{};
      while (displayModeIterator->Next(out_ptr(mode)) == S_OK) {
        modes.push_back(std::move(mode));
      }
    }

    auto const index = [&] {
      if (modes.empty()) {
        std::cerr << "Could not find any display modes\n";
        std::terminate();
      } else {
        std::cerr << "Modes:\n";
        {
          auto i = 0;
          for (auto const &mode : modes) {
            auto name = DLString{};
            mode->GetName(&name.data);
            std::cerr << i++ << ' ';
            name.print();
            std::cerr << '\n';
          }
        }
        /
        std::cerr << "Please select: ";
        auto i = -1;
        while (i < 0 || i >= modes.size()) {
          std::cin >> i;
        }
        return i;
        /
        return 4;
      }
    }();

    return std::move(modes[index]);
  }();
  */

  auto output_buffer = ipc_unmanaged_object<triple_buffer>{argv[1]};

  auto http_delegate_ =
      std::make_shared<http_delegate<decltype(reload_decklink)>>(
          decklinks, decklink_index, external_keyer, reload_decklink);
  auto websocket_delegate_ = std::make_shared<websocket_delegate>();
  auto server_ = server{http_delegate_, websocket_delegate_, "0.0.0.0", 0, 4};

  http_delegate_->reload_clients = [&] { server_.send(""s); };

  std::cout << server_.port() << '\n' << std::flush;

  while (true) {
    while (!output_buffer->novel_to_read()) {
    }

    output_buffer->about_to_read();

    if (decklink) {
      decklink->display_frame(output_buffer->read());
    }
  }
}
