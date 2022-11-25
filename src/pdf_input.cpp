
#include "base64.hpp"
#include "ipc_shared_object.hpp"
#include "server/server.hpp"
#include "triple_buffer.hpp"

#include <poppler-document.h>
#include <poppler-image.h>
#include <poppler-page-renderer.h>
#include <poppler-page.h>

#include <png++/png.hpp>

#include <charconv>
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
  onclick="fetch('/activate_slide?slide={index}')"
  style="{style}"
  src="data:image/png;base64,{base64}"
/>
)html",
        "index"_a = thumbnail_.index, "base64"_a = thumbnail_.base64,
        "style"_a = (thumbnail_.index == thumbnail_.active_slide
                         ? "box-shadow: 0px 0px 4px #0000FF;"sv
                         : ""sv));
  }
};

auto encode_image(poppler::image image) -> std::string {
  auto png_img =
      png::image<png::rgba_pixel>{static_cast<png::uint_32>(image.width()),
                                  static_cast<png::uint_32>(image.height())};

  auto data = image.data();
  for (std::size_t y = 0; y < static_cast<std::size_t>(image.height());
       y += 1) {
    for (std::size_t x = 0; x < static_cast<std::size_t>(image.width());
         x += 1) {
      auto b = static_cast<unsigned char>(*(data++));
      auto g = static_cast<unsigned char>(*(data++));
      auto r = static_cast<unsigned char>(*(data++));
      auto a = static_cast<unsigned char>(*(data++));
      png_img.set_pixel(x, y, {r, g, b, a});
    }
  }

  auto ss = std::stringstream{};
  png_img.write_stream(ss);
  return base64(std::move(ss).str());
}

void key_image(poppler::image &image, std::string key) {
  auto parse_channel = [&](std::string_view str) {
    uint8_t value;
    std::from_chars(str.begin(), str.end(), value, 16);
    return static_cast<char>(value);
  };

  auto key_r = parse_channel(key.substr(1, 2));
  auto key_g = parse_channel(key.substr(3, 2));
  auto key_b = parse_channel(key.substr(5, 2));

  for (auto data = image.data();
       data != image.data() + image.bytes_per_row() * image.height();
       data += 4) {
    auto &b = data[0];
    auto &g = data[1];
    auto &r = data[2];
    auto &a = data[3];

    if (r == key_r && g == key_g && b == key_b) {
      r = 0;
      g = 0;
      b = 0;
      a = 0;
    }
  }
}

void convert_slide(poppler::page const &page, triple_buffer::buffer &buffer,
                   thumbnail &_thumbnail, std::optional<std::string> key) {
  auto page_rect = page.page_rect();
  auto width_pdf = page_rect.width();
  auto height_pdf = page_rect.height();
  auto width_in = width_pdf / 72;
  auto height_in = height_pdf / 72;

  auto dpi_x = triple_buffer::width / width_in;
  auto dpi_y = triple_buffer::height / height_in;

  auto width_thumb = 384;
  auto height_thumb = 216;
  auto dpi_thumb_x = width_thumb / width_in;
  auto dpi_thumb_y = height_thumb / height_in;

  auto renderer = poppler::page_renderer{};
  // It seems that endianness is backwards so this is actually bgra
  renderer.set_image_format(poppler::image::format_argb32);

  auto image = renderer.render_page(&page, dpi_x, dpi_y);
  auto thumb_img = renderer.render_page(&page, dpi_thumb_x, dpi_thumb_y);

  if (key) {
    key_image(image, *key);
    key_image(thumb_img, *key);
  }

  std::copy_n(image.const_data(), triple_buffer::size, buffer.begin());
  _thumbnail.base64 = encode_image(thumb_img);
}

template <typename WriteFrame, typename ReloadDocument> class http_delegate {
public:
  // using body_type = beast::http::buffer_body;
  // using body_type = beast::http::file_body;
  using body_type = beast::http::string_body;

private:
  std::string_view name;
  std::string_view root_dir;
  std::unique_ptr<poppler::document> &document;
  std::vector<thumbnail> &thumbnails;
  std::size_t &active_slide;
  std::optional<std::string> &key;
  WriteFrame const &write_frame;
  ReloadDocument const &reload_document;

public:
  std::function<void()> reload_clients = [] {};

  http_delegate(std::string_view name, std::string_view root_dir,
                std::unique_ptr<poppler::document> &document,
                std::vector<thumbnail> &thumbnails, std::size_t &active_slide,
                std::optional<std::string> &key, WriteFrame const &write_frame,
                ReloadDocument const &reload_document)
      : name{name}, root_dir{root_dir}, document{document},
        thumbnails{thumbnails}, active_slide{active_slide}, key{key},
        write_frame{write_frame}, reload_document{reload_document} {}

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
      Open PDF
    </button>
    <button
      onclick="window.parent.postMessage({{msg: 'show_detail_view', data: `http://${{window.location.host}}/control`}}, '*')"
    >
      Control slides
    </button>
    Slide {active_slide} of {total_slides}
    <br/>
    Key:
    <input
      type="checkbox"
      onchange="
        if (event.target.checked) {{
          fetch('/activate_key', {{method: 'POST', body: '#00ff00'}})
        }} else {{
          fetch('/deactivate_key')
        }}
      "
      {key_active_checked}
    >
    <input
      type="color"
      onchange="fetch('/activate_key', {{method: 'POST', body: event.target.value}})"
      value="{key_colour}"
      {key_colour_disabled}
    >
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
          "total_slides"_a = thumbnails.size(),
          "key_active_checked"_a = key ? "checked"sv : ""sv,
          "key_colour"_a = key.value_or(""),
          "key_colour_disabled"_a = key ? ""sv : "disabled"sv);
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

          document = std::unique_ptr<poppler::document>{
              poppler::document::load_from_file(abs_path)};
          reload_document();

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
      auto regex = std::regex{R"(/activate_slide\?slide=(\d*))"};
      auto target = std::string{req.target()};
      if (std::smatch match; std::regex_match(target, match, regex)) {
        if (match.size() == 2) {
          active_slide = std::stoul(match[1].str());
          write_frame();
          reload_clients();
          return send(http::empty_response(req));
        }
      }
      return send(http::bad_request(req, "Cannot parse url params"));
    } else if (req.target() == "/activate_key" &&
               req.method() == beast::http::verb::post) {
      key = req.body();
      reload_document();
      return send(http::empty_response(req));
    } else if (req.target() == "/deactivate_key") {
      key = std::nullopt;
      reload_document();
      return send(http::empty_response(req));
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
  auto const name = argc >= 2 ? std::string_view{argv[1]} : "PDF Source"sv;
  auto const root_dir = argc >= 3 ? std::string_view{argv[2]} : "."sv;

  auto document = std::unique_ptr<poppler::document>{};
  auto slides = std::vector<triple_buffer::buffer>{};
  auto thumbnails = std::vector<thumbnail>{};
  auto active_slide = std::size_t{0};

  auto key = std::optional<std::string>{};

  auto output_buffer = std::optional<ipc_unmanaged_object<triple_buffer>>{};

  auto websocket_delegate_ = std::make_shared<websocket::tracking_delegate>();
  auto reload_clients = [&] { websocket_delegate_->send(""s); };

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

  auto reload_document = [&] {
    if (document) {
      slides.clear();
      slides.resize(static_cast<std::size_t>(document->pages()));
      thumbnails.clear();
      thumbnails.reserve(static_cast<std::size_t>(document->pages()));

      for (auto i = 0; i < document->pages(); i += 1) {
        auto &thumbnail_ = thumbnails.emplace_back(i, active_slide);
        auto page = std::unique_ptr<poppler::page>{document->create_page(i)};
        convert_slide(*page, slides[static_cast<std::size_t>(i)], thumbnail_,
                      key);
      }

      active_slide = 0;
      write_frame();
      reload_clients();
    } else {
      std::cerr << "No document\n";
    }
  };

  auto http_delegate_ = std::make_shared<
      http_delegate<decltype(write_frame), decltype(reload_document)>>(
      name, root_dir, document, thumbnails, active_slide, key, write_frame,
      reload_document);
  auto server_ = server{http_delegate_, websocket_delegate_, "0.0.0.0", 0, 4};

  http_delegate_->reload_clients = reload_clients;

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
