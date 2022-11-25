#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

template <typename T> auto make_decklink_ptr(T *p) {
  return decklink_ptr<T>{p};
}

struct DLString {
#if defined(__linux__)
  char const *data;
#elif defined(__APPLE__) && defined(__MACH__)
  CFStringRef data;
#elif defined(WIN32)
  BSTR data;
#endif

  void print() const {
#if defined(__linux__)
    std::cout << data;
#elif defined(__APPLE__) && defined(__MACH__)
    std::cout << CFStringGetCStringPtr(data, kCFStringEncodingASCII);
#elif defined(WIN32)
    std::wcout << data;
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
  triple_buffer::buffer &buffer;

public:
  output_frame(triple_buffer::buffer &buffer) : buffer{buffer} {}

  auto QueryInterface(REFIID id, void **outputInterface) -> HRESULT override {
    *outputInterface = nullptr;
    return E_NOINTERFACE;
  }

  auto AddRef() -> ULONG override { return 0; }
  auto Release() -> ULONG override { return 0; }

  auto GetWidth() -> long override { return triple_buffer::width; }
  auto GetHeight() -> long override { return triple_buffer::height; }
  auto GetRowBytes() -> long override { return triple_buffer::pitch; }
  auto GetPixelFormat() -> BMDPixelFormat override { return bmdFormat8BitBGRA; }
  auto GetFlags() -> BMDFrameFlags override { return bmdFrameFlagDefault; }
  auto GetBytes(void **_buffer) -> HRESULT override {
    *_buffer = buffer.begin();
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

class Callback : public IDeckLinkInputCallback {
private:
  std::optional<ipc_unmanaged_object<triple_buffer>> &output_buffer;
  IDeckLinkVideoConversion &decklink_convertor;

public:
  Callback(std::optional<ipc_unmanaged_object<triple_buffer>> &output_buffer,
           IDeckLinkVideoConversion &decklink_convertor)
      : output_buffer{output_buffer}, decklink_convertor{decklink_convertor} {}

private:
  auto VideoInputFrameArrived(IDeckLinkVideoInputFrame *videoFrame,
                              IDeckLinkAudioInputPacket *audioPacket)
      -> HRESULT override {
    if (output_buffer) {
      auto output_frame_ = output_frame{(*output_buffer)->write()};
      decklink_convertor.ConvertFrame(videoFrame, &output_frame_);
      (*output_buffer)->done_writing();
    }
    return S_OK;
  }

  auto
  VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                          IDeckLinkDisplayMode *newDisplayMode,
                          BMDDetectedVideoInputFormatFlags detectedSignalFlags)
      -> HRESULT override {
    if (newDisplayMode->GetDisplayMode() != bmdModeHD1080p25) {
      std::cerr << "Invalid mode\n";
      std::terminate();
    }
    return S_OK;
  }

  auto QueryInterface(REFIID iid, LPVOID *ppv) -> HRESULT override {
    return E_NOINTERFACE;
  }
  auto AddRef() -> ULONG override { return 0; }
  auto Release() -> ULONG override { return 0; }
};

class active_decklink {
private:
  decklink_ptr<IDeckLinkInput> decklink_input;

public:
  active_decklink(IDeckLink &decklink, Callback &callback) {
    if (decklink.QueryInterface(IID_IDeckLinkInput, out_ptr(decklink_input)) !=
        S_OK) {
      std::cerr << "Could not get a DeckLink input\n";
      std::terminate();
    }

    if (decklink_input->EnableVideoInput(bmdModeHD1080p25, bmdFormat8BitYUV,
                                         bmdVideoInputEnableFormatDetection) !=
        S_OK) {
      std::cerr << "Could not enable video input\n";
      std::terminate();
    }

    if (decklink_input->SetCallback(&callback) != S_OK) {
      std::cerr << "Could not set callback\n";
      std::terminate();
    }

    if (decklink_input->StartStreams() != S_OK) {
      std::cerr << "Could not start streams\n";
      std::terminate();
    }
  }

  ~active_decklink() {
    decklink_input->StopStreams();
    decklink_input->DisableVideoInput();
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
  std::string_view name;
  std::vector<decklink_ptr<IDeckLink>> &decklinks;
  std::optional<std::size_t> &decklink_index;

  ReloadDecklink reload_decklink;

public:
  std::function<void()> reload_clients = [] {};

  http_delegate(std::string_view name,
                std::vector<decklink_ptr<IDeckLink>> &decklinks,
                std::optional<std::size_t> &decklink_index,
                ReloadDecklink reload_decklink)
      : name{name}, decklinks{decklinks}, decklink_index{decklink_index},
        reload_decklink{std::move(reload_decklink)} {}

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
    <h2>{name}</h2>
    Decklink
    <select onchange="fetch('/decklink', {{method: 'POST', body: event.target.value}})">
      <option value="-1"> - </option>
      {decklinks}
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
          "name"_a = name,
          "decklinks"_a = fmt::join(
              ranges::views::zip_with(decklink_option::make(decklink_index),
                                      ranges::views::iota(0ul), decklinks),
              ""));
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
    } else {
      return send(http::not_found(req));
    }
  }
};

int main(int argc, char **argv) {
  auto const name = argc >= 2 ? std::string_view{argv[1]} : "Decklink Input"sv;

#if defined(WIN32)
  if (CoInitialize(nullptr) != S_OK) {
    std::cerr << "Could not initialise COM (Windows)\n";
    std::terminate();
  }
#endif

#if defined(UNIX)
  auto decklink_convertor = make_decklink_ptr(CreateVideoConversionInstance());
#elif defined(WIN32)
  auto decklink_convertor = decklink_ptr<IDeckLinkConversion>{};
  if (CoCreateInstance(CLSID_CDeckLinkVideoConversion, nullptr, CLSCTX_ALL,
                       IID_IDeckLinkVideoConversion,
                       out_ptr(decklink_convertor)) != S_OK) {
    std::cerr << "Could not get a DeckLink Convertor (Windows)\n";
    std::terminate();
  }
#endif

  auto decklinks = [&] {
#if defined(UNIX)
    auto decklink_iterator =
        make_decklink_ptr(CreateDeckLinkIteratorInstance());
#elif defined(WIN32)
    auto decklink_iterator = decklink_ptr<IDeckLinkIterator>{};
    if (CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL,
                         IID_IDeckLinkIterator,
                         out_ptr(decklink_iterator)) != S_OK) {
      std::cerr << "Could not get a DeckLink Iterator (Windows)\n";
      std::terminate();
    }
#endif

    if (decklink_iterator == nullptr) {
      std::cerr << "Could not get a DeckLink Iterator\n";
      std::terminate();
    }

    auto decklinks = std::vector<decklink_ptr<IDeckLink>>{};
    {
      auto decklink = decklink_ptr<IDeckLink>{};
      while (decklink_iterator->Next(out_ptr(decklink)) == S_OK) {
        decklinks.push_back(std::move(decklink));
      }
    }
    return decklinks;
  }();

  auto decklink_index = std::optional<std::size_t>{};
  auto decklink = std::optional<active_decklink>{};

  auto output_buffer = std::optional<ipc_unmanaged_object<triple_buffer>>{};

  auto callback = Callback{output_buffer, *decklink_convertor};

  auto reload_decklink = [&] {
    if (decklink_index) {
      decklink.emplace(*decklinks[*decklink_index], callback);
    } else {
      decklink = std::nullopt;
    }
  };

  auto http_delegate_ =
      std::make_shared<http_delegate<decltype(reload_decklink)>>(
          name, decklinks, decklink_index, reload_decklink);
  auto websocket_delegate_ = std::make_shared<websocket::tracking_delegate>();
  auto server_ = server{http_delegate_, websocket_delegate_, "0.0.0.0", 0, 4};

  http_delegate_->reload_clients = [&] { websocket_delegate_->send(""s); };

  // TODO terminate on disconnect
  auto router_websocket_delegate_ = websocket::make_read_client_delegate(
      [&](std::any &, beast::flat_buffer &buffer) {
        auto name = std::string{static_cast<char const *>(buffer.data().data()),
                                buffer.data().size()};
        output_buffer.emplace(name.c_str());
      });
  auto router_websocket = server_.connect_to_websocket(
      router_websocket_delegate_, "127.0.0.1", 8080,
      fmt::format("input_{port}", "port"_a = server_.port()));

  while (true) {
    std::this_thread::sleep_for(1h);
  }
}
