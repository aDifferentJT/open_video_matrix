
#include "ipc_shared_object.hpp"
#include "server/server.hpp"
#include "triple_buffer.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <Magick++.h>
#pragma clang diagnostic pop

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/tokenize.hpp>
#include <range/v3/view/transform.hpp>

namespace ipc = boost::interprocess;

using fmt::operator""_a;

using namespace std::literals;

// Should be std
// C++20 make_unique_for_overwrite
template <class T>
requires(!std::is_array_v<T>) std::unique_ptr<T> make_unique_for_overwrite() {
  return std::unique_ptr<T>(new T);
}

template <class T>
requires std::is_unbounded_array_v<T> std::unique_ptr<T>
make_unique_for_overwrite(std::size_t n) {
  return std::unique_ptr<T>(new std::remove_extent_t<T>[n]);
}

template <class T, class... Args>
requires std::is_bounded_array_v<T>
void make_unique_for_overwrite(Args &&...) = delete;

namespace ranges {
template <>
inline constexpr bool enable_view<std::filesystem::directory_iterator> = true;
}

auto decode_url(std::string_view encoded) -> std::string {
  auto regex = std::regex{"[^%]|%.."};
  return encoded | ranges::views::tokenize(regex) |
         ranges::views::transform([](std::string const &encoded_char) -> char {
           if (encoded_char[0] == '%') {
             return static_cast<char>(
                 std::stoi(encoded_char.substr(1), nullptr, 16));
           } else {
             return encoded_char[0];
           }
         }) |
         ranges::to<std::basic_string>();
}

struct filesystem_link {
  std::string_view file_prefix;
  std::string_view dir_prefix;
  std::string const &path;
  std::filesystem::directory_entry entry;
};

template <> struct fmt::formatter<filesystem_link> {
  constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
    if (ctx.begin() != ctx.end() && *ctx.begin() != '}')
      throw format_error("invalid format");
    return ctx.begin();
  }

  auto format(filesystem_link const &dev, auto &ctx) const
      -> decltype(ctx.out()) {
    auto filename = dev.entry.path().filename().generic_string();
    if (dev.entry.is_regular_file()) {
      return fmt::format_to(
          ctx.out(),
          R"html(<a href="/{prefix}/{path}/{filename}">{filename}</a>)html",
          "prefix"_a = dev.file_prefix, "path"_a = dev.path,
          "filename"_a = filename);
    } else if (dev.entry.is_directory()) {
      return fmt::format_to(
          ctx.out(),
          R"html(<a href="/{prefix}/{path}/{filename}">{filename}</a>)html",
          "prefix"_a = dev.dir_prefix, "path"_a = dev.path,
          "filename"_a = filename);
    } else {
      return fmt::format_to(ctx.out(), "{filename}", "filename"_a = filename);
    }
  }
};

struct thumbnail {
  std::size_t index;
  std::size_t &active_slide;
  std::string base64;

  thumbnail(std::size_t index, std::size_t &active_slide)
      : index{index}, active_slide{active_slide}, base64{} {}
};

template <> struct fmt::formatter<thumbnail> {
  constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
    if (ctx.begin() != ctx.end() && *ctx.begin() != '}')
      throw format_error("invalid format");
    return ctx.begin();
  }

  auto format(thumbnail const &thumbnail_, auto &ctx) const
      -> decltype(ctx.out()) {
    return fmt::format_to(
        ctx.out(), R"html(
<img
  onclick="console.log('thumbnail {index} clicked')"
  style="{style}"
  src="data:image/jpeg;base64,{base64}"
/>
)html",
        "index"_a = thumbnail_.index, "base64"_a = thumbnail_.base64,
        "style"_a = (thumbnail_.index == thumbnail_.active_slide
                         ? "box-shadow: 0px 0px 4px #0000FF;"sv
                         : ""sv));
  }
};

void convert_slide(Magick::Image &img, triple_buffer::buffer &buffer,
                   thumbnail &thumbnail_) {
  try {
    img.resize({triple_buffer::width, triple_buffer::height});
    auto bg_colour = Magick::ColorRGB(0, 0, 0);
    bg_colour.alpha(0);
    img.extent({triple_buffer::width, triple_buffer::height}, bg_colour,
               Magick::CenterGravity);
    img.write(0, 0, triple_buffer::width, triple_buffer::height, "BGRA",
              Magick::CharPixel, buffer.video_frame.data());

    img.resize({192, 108});
    auto thumbnail_blob = Magick::Blob{};
    img.write(&thumbnail_blob, "PNG");
    thumbnail_.base64 = thumbnail_blob.base64();
  } catch (Magick::Exception const &e) {
    std::cerr << "Magick exception: " << e.what() << '\n' << std::flush;
  }
}

template <typename WriteFrame> class http_delegate {
public:
  // using body_type = beast::http::buffer_body;
  // using body_type = beast::http::file_body;
  using body_type = beast::http::string_body;

private:
  std::string_view name;
  std::string_view root_dir;
  std::vector<triple_buffer::buffer> &slides;
  std::vector<thumbnail> &thumbnails;
  std::size_t &active_slide;
  WriteFrame const &write_frame;

public:
  std::function<void()> reload_clients = [] {};

  http_delegate(std::string_view name, std::string_view root_dir,
                std::vector<triple_buffer::buffer> &slides,
                std::vector<thumbnail> &thumbnails, std::size_t &active_slide,
                WriteFrame const &write_frame)
      : name{name}, root_dir{root_dir}, slides{slides}, thumbnails{thumbnails},
        active_slide{active_slide}, write_frame{write_frame} {}

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
    <br/>
    <button
      onclick="window.parent.postMessage({{msg: 'show_detail_view', data: `http://${{window.location.host}}/open_dir/`}}, '*')"
    >
      Open Presentation
    </button>
    <button
      onclick="window.parent.postMessage({{msg: 'show_detail_view', data: `http://${{window.location.host}}/control`}}, '*')"
    >
      Control slides
    </button>
    Slide {active_slide} of {total_slides}
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
          "name"_a = name, "active_slide"_a = active_slide + 1,
          "total_slides"_a = slides.size());
      auto mime_type = "text/html"sv;

      return http::string_response(req, std::move(body), mime_type, send);
    } else if (req.target().starts_with("/open_dir/")) {
      auto regex = std::regex{R"(/open_dir/(.*))"};
      auto target = std::string{req.target()};
      if (std::smatch match; std::regex_match(target, match, regex)) {
        if (match.size() == 2) {
          auto rel_path = decode_url(match[1].str());
          if (rel_path.find("..") != std::string::npos) {
            return send(http::bad_request(req, "Invalid path"));
          }

          auto abs_path = std::string{root_dir} + rel_path;

          auto body = fmt::format(
              R"html(
<html>
  <head>
  </head>
  <body>
    {files}
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
              "files"_a = fmt::join(
                  std::filesystem::directory_iterator(abs_path) |
                      ranges::views::transform([&](auto entry) {
                        return filesystem_link{"open_file", "open_dir",
                                               rel_path, std::move(entry)};
                      }),
                  "<br/>"));
          auto mime_type = "text/html"sv;

          return http::string_response(req, std::move(body), mime_type, send);
        }
      }
      return send(http::bad_request(req, "Cannot parse url"));
    } else if (req.target().starts_with("/open_file/")) {
      auto regex = std::regex{R"(/open_file/(.*))"};
      auto target = std::string{req.target()};
      if (std::smatch match; std::regex_match(target, match, regex)) {
        if (match.size() == 2) {
          auto rel_path = decode_url(match[1].str());
          if (rel_path.find("..") != std::string::npos) {
            return send(http::bad_request(req, "Invalid path"));
          }

          auto abs_path = std::string{root_dir} + rel_path;

          auto magick_slides = std::vector<Magick::Image>{};
          try {
            auto file = std::ifstream{abs_path, std::ios::binary};
            auto const begin = file.tellg();
            file.seekg(0, std::ios::end);
            auto const end = file.tellg();
            auto const size = end - begin;
            std::cerr << "Opening: " << abs_path << " Size: " << size << '\n';
            auto data =
                make_unique_for_overwrite<char[]>(static_cast<size_t>(size));
            file.seekg(0, std::ios::beg);
            file.read(data.get(), size);
            auto blob = Magick::Blob{data.release(), static_cast<size_t>(size)};

            auto options = Magick::ReadOptions{};
            options.density({300, 300});

            Magick::readImages(&magick_slides, blob, options);
            std::cerr << "Read " << magick_slides.size() << " images\n";
          } catch (Magick::Exception const &e) {
            std::cerr << "Magick exception: " << e.what() << '\n';
          }
          slides.clear();
          slides.resize(magick_slides.size());
          thumbnails.clear();
          thumbnails.reserve(magick_slides.size());
          for (std::size_t i = 0; i < magick_slides.size(); i += 1) {
            auto &thumbnail_ = thumbnails.emplace_back(i, active_slide);
            convert_slide(magick_slides[i], slides[i], thumbnail_);
          }
          active_slide = 0;
          write_frame();
          reload_clients();
          return send(http::redirect_response(req, "/control"));
        }
      }
      return send(http::bad_request(req, "Cannot parse url"));
    } else if (req.target() == "/control") {
      auto body = fmt::format(
          R"html(
<html>
  <head>
  </head>
  <body>
    {thumbnails}
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
          "thumbnails"_a = fmt::join(thumbnails, ""));
      auto mime_type = "text/html"sv;

      return http::string_response(req, std::move(body), mime_type, send);
    } else if (req.target().starts_with("/activate_slide?slide=")) {
      auto regex = std::regex{R"(/activate_slide?slide=(\d*))"};
      auto target = std::string{req.target()};
      if (std::smatch match; std::regex_match(target, match, regex)) {
        if (match.size() == 2) {
          active_slide = std::stoul(match[1].str());
          write_frame();
          return send(http::empty_response(req));
        }
      }
      reload_clients();
      return send(http::bad_request(req, "Cannot parse url params"));
    } else {
      return send(http::not_found(req));
    }
  }
};

class index_iterator {
private:
  std::size_t index = 0;

public:
  auto operator*() { return index; }

  auto operator++() -> auto & {
    index += 1;
    return *this;
  }

  auto operator++(int) -> auto{
    auto tmp = *this;
    operator++();
    return tmp;
  }
};

int main(int argc, char **argv) {
  auto const name =
      argc >= 2 ? std::string_view{argv[1]} : "Presentation Source"sv;
  auto const root_dir = argc >= 3 ? std::string_view{argv[2]} : "."sv;

  Magick::InitializeMagick(nullptr);

  auto slides = std::vector<triple_buffer::buffer>{};
  auto thumbnails = std::vector<thumbnail>{};
  auto active_slide = std::size_t{0};

  auto output_buffer = std::optional<ipc_unmanaged_object<triple_buffer>>{};

  auto write_frame = [&] {
    if (active_slide < slides.size()) {
      if (output_buffer) {
        (*output_buffer)->write() = slides[active_slide];
        (*output_buffer)->done_writing();
      }
    } else {
      std::cerr << "Slide out of bounds\n";
    }
  };

  auto http_delegate_ = std::make_shared<http_delegate<decltype(write_frame)>>(
      name, root_dir, slides, thumbnails, active_slide, write_frame);
  auto websocket_delegate_ = std::make_shared<websocket::tracking_delegate>();
  auto server_ = server{http_delegate_, websocket_delegate_, "0.0.0.0", 0, 4};

  http_delegate_->reload_clients = [&] { websocket_delegate_->send(""s); };

  // TODO terminate on disconnect
  auto router_websocket_delegate_ = websocket::make_read_client_delegate(
      [&](std::any &, beast::flat_buffer &buffer) {
        auto name = std::string{static_cast<char const *>(buffer.data().data()),
                                buffer.data().size()};
        output_buffer.emplace(name.c_str());
        write_frame();
      });
  auto router_websocket = server_.connect_to_websocket(
      router_websocket_delegate_, "127.0.0.1", 8080,
      fmt::format("input_{port}", "port"_a = server_.port()));

  while (true) {
    std::this_thread::sleep_for(1h);
  }
}
