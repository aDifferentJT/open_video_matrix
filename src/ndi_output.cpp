
#include "NDI.hpp"

#include "ipc_shared_object.hpp"
#include "triple_buffer.hpp"

int main([[maybe_unused]] int argc, char **argv) {
  auto const ndi = NDIlib{};

  auto const send_create =
      NDIlib_send_create_t{"Open Video Matrix", nullptr, true, true};
  auto sender = ndi->send_create(&send_create);

  auto output_buffer = ipc_unmanaged_object<triple_buffer>{argv[1]};

  /*
  auto rpc = ORPC::stream_parent<ORPC::router>{std::cin, std::cout};
  rpc->add("name", [&](std::string_view s) mutable {
    auto name = std::string{s};
    auto const send_create =
        NDIlib_send_create_t{name.c_str(), nullptr, true, true};
    sender = ndi->send_create(&send_create);
  });
  */

  std::cout << "1234\n" << std::flush;

  while (true) {
    while (!output_buffer->novel_to_read()) {
    }

    output_buffer->about_to_read();

    auto frame = NDIlib_video_frame_v2_t{
        1920,
        1080,
        NDIlib_FourCC_type_BGRA,
        25000,
        1000,
        0.0f,
        NDIlib_frame_format_type_progressive,
        0,
        const_cast<uint8_t *>(output_buffer->read().data()),
        1920 * 4};

    // Using the async version would require holding the lock too long
    ndi->send_send_video_v2(sender, &frame);
  }
}
