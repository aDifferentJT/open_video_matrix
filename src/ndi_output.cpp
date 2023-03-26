
#include "NDI.hpp"

#include "ipc_shared_object.hpp"
#include "server/server.hpp"
#include "triple_buffer.hpp"

#include <fmt/format.h>

#include <optional>

using fmt::operator""_a;

using namespace std::literals;

template <typename ReloadSender> class http_delegate {
public:
  using body_type = beast::http::string_body;

private:
  std::string &name;
  ReloadSender const &reload_sender;

public:
  std::function<void()> reload_clients = [] {};

  http_delegate(std::string &name, ReloadSender const &reload_sender)
      : name{name}, reload_sender{reload_sender} {}

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
    <h2>NDI Output</h2>
    <input
      onchange="fetch('/name', {{method: 'POST', body: event.target.value}})"
      value="{name}"
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
          "name"_a = name);
      auto mime_type = "text/html"sv;

      return http::string_response(req, std::move(body), mime_type, send);
    } else if (req.target() == "/name" &&
               req.method() == beast::http::verb::post) {
      name = req.body();
      reload_sender();
      reload_clients();
      return send(http::empty_response(req));
    } else {
      return send(http::not_found(req));
    }
  }
};

int main(int argc, char **argv) {
  auto input_buffer = std::optional<ipc_unmanaged_object<triple_buffer>>{};

  auto const ndi = NDIlib{};

  auto name = argc >= 2 ? std::string{argv[1]} : "Open Video Matrix"s;

  NDIlib_send_instance_t sender;

  auto reload_sender = [&] {
    auto const send_create =
        NDIlib_send_create_t{name.c_str(), nullptr, true, false};
    sender = ndi->send_create(&send_create);
  };

  reload_sender();

  auto http_delegate_ =
      std::make_shared<http_delegate<decltype(reload_sender)>>(name,
                                                               reload_sender);
  auto websocket_delegate_ = std::make_shared<websocket::tracking_delegate>();
  auto server_ = server{http_delegate_, websocket_delegate_, "0.0.0.0", 0, 4};

  http_delegate_->reload_clients = [&] { websocket_delegate_->send(""s); };

  // TODO terminate on disconnect
  auto router_websocket_delegate_ = websocket::make_read_client_delegate(
      [&](std::any &, beast::flat_buffer &buffer) {
        auto name = std::string{static_cast<char const *>(buffer.data().data()),
                                buffer.data().size()};
        input_buffer.emplace(name.c_str());
      });
  auto router_websocket = server_.connect_to_websocket(
      router_websocket_delegate_, "127.0.0.1", 8080,
      fmt::format("/output_{port}", "port"_a = server_.port()));

  auto nextFrame = std::chrono::steady_clock::now();
  auto lastFrame = std::chrono::steady_clock::now();

  while (true) {
  auto thisFrame = std::chrono::steady_clock::now();
    std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(thisFrame - lastFrame).count() << "ms\n";
    lastFrame = thisFrame;

    //std::this_thread::sleep_until(nextFrame);
    nextFrame += 40ms;

    if (input_buffer) {
      (*input_buffer)->about_to_read();

      auto video_frame = NDIlib_video_frame_v2_t{
          triple_buffer::width,
          triple_buffer::height,
          NDIlib_FourCC_type_BGRA,
          25000,
          1000,
          0.0f,
          NDIlib_frame_format_type_progressive,
          0,
          const_cast<uint8_t *>((*input_buffer)->read().video_frame),
          triple_buffer::pitch};

      static constexpr auto audio_channel_stride =
          triple_buffer::audio_samples_per_frame / triple_buffer::num_channels;
      auto audio_frame_float32_planar =
          std::array<float, triple_buffer::audio_samples_per_frame>{};

      for (std::size_t i = 0; i < triple_buffer::audio_samples_per_frame;
           i += 1) {
        auto sample = i / triple_buffer::num_channels;
        auto channel = i % triple_buffer::num_channels;

        static constexpr auto audio_conversion_factor =
            static_cast<float>((std::numeric_limits<int32_t>::max)());

        audio_frame_float32_planar[channel * audio_channel_stride + sample] =
            static_cast<float>(
                (*input_buffer)
                    ->read()
                    .audio_frame[sample * triple_buffer::num_channels +
                                 channel]) /
            audio_conversion_factor;

        //audio_frame_float32_planar[channel * audio_channel_stride + sample] = static_cast<float>(i * 10 % triple_buffer::audio_samples_per_frame) / triple_buffer::audio_samples_per_frame;
      }

      auto audio_frame = NDIlib_audio_frame_v3_t{
          triple_buffer::sample_rate,
          triple_buffer::num_channels,
          triple_buffer::audio_samples_per_frame / triple_buffer::num_channels,
          NDIlib_send_timecode_synthesize,
          NDIlib_FourCC_type_FLTP,
          reinterpret_cast<uint8_t *>(audio_frame_float32_planar.data()),
          audio_channel_stride * sizeof(float)};

      // Using the async version would require holding the lock too long
      ndi->send_send_video_v2(sender, &video_frame);
      ndi->send_send_audio_v3(sender, &audio_frame);
    }
  }
}
