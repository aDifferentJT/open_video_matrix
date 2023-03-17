
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

#include <vlcpp/vlc.hpp>

#include <fmt/format.h>

using fmt::operator""_a;

using namespace std::literals;

template <typename ReloadClients> class http_delegate {
public:
  using body_type = beast::http::string_body;

private:
  ReloadClients const &reload_clients;

public:
  http_delegate(ReloadClients const &reload_clients)
      : reload_clients{reload_clients} {}

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
    VLC
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
)html"sv);
      auto mime_type = "text/html"sv;

      return http::string_response(req, std::move(body), mime_type, send);
    } else {
      return send(http::not_found(req));
    }
  }
};

int main(int argc, char **argv) {
  auto output_buffer = std::optional<ipc_unmanaged_object<triple_buffer>>{};

  auto vlc = VLC::Instance{argc, argv};

  auto media_player = VLC::MediaPlayer{vlc};
  media_player.setVideoFormat("RV32", triple_buffer::width, triple_buffer::height, triple_buffer::pitch);
  media_player.setVideoCallbacks(
      [&](void **planes) -> void * {
        if (output_buffer) {
          planes[0] = (*output_buffer)->write().video_frame.data();
        }
        return nullptr;
      },
      nullptr,
      [&](void *) {
        if (output_buffer) {
          (*output_buffer)->done_writing();
        }
      });

  auto media_list = VLC::MediaList{vlc};

  auto media_list_player = VLC::MediaListPlayer{vlc};
  media_list_player.setMediaPlayer(media_player);
  media_list_player.setMediaList(media_list);

  auto media =
      VLC::Media{vlc, "/mnt/av_resources/test_files/big-buck-bunny_trailer.webm", VLC::Media::FromType::FromPath};

  {
    auto lock = std::scoped_lock{media_list};
    media_list.addMedia(media);
  }

  media_list_player.play();

  auto websocket_delegate_ = std::make_shared<websocket::tracking_delegate>();

  auto reload_clients = [&] { websocket_delegate_->send(""s); };

  auto http_delegate_ =
      std::make_shared<http_delegate<decltype(reload_clients)>>(reload_clients);
  auto server_ = server{http_delegate_, websocket_delegate_, "0.0.0.0", 0, 4};

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
