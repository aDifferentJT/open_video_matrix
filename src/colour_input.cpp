
#include "ipc_shared_object.hpp"
#include "server/server.hpp"
#include "triple_buffer.hpp"

#include <charconv>
#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include <fmt/format.h>

using fmt::operator""_a;

using namespace std::literals;

template <typename WriteFrame> class http_delegate {
public:
  using body_type = beast::http::string_body;

private:
  std::string &colour;
  WriteFrame const &write_frame;

public:
  std::function<void()> reload_clients = [] {};

  http_delegate(std::string &colour, WriteFrame const &write_frame)
      : colour{colour}, write_frame{write_frame} {}

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
    Colour
    <input
      type="color"
      onchange="fetch('/colour', {{method: 'POST', body: event.target.value}})"
      value="{colour}"
    >
    </input>
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
          "colour"_a = colour);
      auto mime_type = "text/html"sv;

      return http::string_response(req, std::move(body), mime_type, send);
    } else if (req.target() == "/colour" &&
               req.method() == beast::http::verb::post) {
      colour = req.body();
      write_frame();
      reload_clients();
      return send(http::empty_response(req));
    } else {
      return send(http::not_found(req));
    }
  }
};

int main(int, char **) {
  auto output_buffer = std::optional<ipc_unmanaged_object<triple_buffer>>{};

  auto colour = "#abcdef"s;

  auto write_frame = [&]() {
    auto parse_channel = [&](std::string_view str) {
      uint8_t value;
      std::from_chars(str.begin(), str.end(), value, 16);
      return value;
    };

    auto r = parse_channel(colour.substr(1, 2));
    auto g = parse_channel(colour.substr(3, 2));
    auto b = parse_channel(colour.substr(5, 2));

    if (output_buffer) {
      auto &buffer = (*output_buffer)->write();
      for (std::size_t i = 0; i < triple_buffer::size; i += 4) {
        buffer[i + 0] = b;
        buffer[i + 1] = g;
        buffer[i + 2] = r;
        buffer[i + 3] = 255;
      }
      (*output_buffer)->done_writing();
    }
  };

  auto http_delegate_ = std::make_shared<http_delegate<decltype(write_frame)>>(
      colour, write_frame);
  auto websocket_delegate_ = std::make_shared<websocket::tracking_delegate>();
  auto server_ = server{http_delegate_, websocket_delegate_, "0.0.0.0", 0, 4};

  http_delegate_->reload_clients = [&] { websocket_delegate_->send(""s); };

  // TODO terminate on disconnect
  auto router_websocket_delegate_ =
      websocket::make_read_client_delegate([&](std::any &, beast::flat_buffer &buffer) {
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
