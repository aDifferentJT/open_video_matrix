#ifndef TRIPLE_BUFFER_HPP
#define TRIPLE_BUFFER_HPP

#include <array>
#include <utility>

#include <boost/interprocess/offset_ptr.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

namespace ipc = boost::interprocess;

class triple_buffer {
public:
  static constexpr auto width = 1920;
  static constexpr auto pitch = width * 4;
  static constexpr auto height = 1080;
  static constexpr auto size = pitch * height;

  using video_frame_t = std::array<uint8_t, size>;

  static constexpr auto sample_rate = 48'000;
  static constexpr auto frame_rate = 25;
  static constexpr auto num_channels = 2;
  static constexpr auto audio_samples_per_frame = sample_rate * num_channels / frame_rate;
  using audio_frame_t = std::array<int32_t, audio_samples_per_frame>;

  struct buffer {
    video_frame_t video_frame;
    audio_frame_t audio_frame;
  };

private:
  ipc::interprocess_mutex mutex;

  std::array<buffer, 3> buffers;
  ipc::offset_ptr<buffer> _read;
  ipc::offset_ptr<buffer> read_next;
  ipc::offset_ptr<buffer> _write;
  ipc::offset_ptr<buffer> write_next;

public:
  triple_buffer()
      : buffers{}, _read{&buffers[0]}, read_next{&buffers[0]},
        _write{&buffers[1]}, write_next{&buffers[2]} {}

  auto novel_to_read() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return _read != read_next;
  }

  void about_to_read() {
    auto lock = ipc::scoped_lock{mutex};
    if (_read != read_next) {
      write_next = _read;
    }
    _read = read_next;
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  void done_writing() {
    auto lock = ipc::scoped_lock{mutex};
    std::atomic_thread_fence(std::memory_order_seq_cst);
    read_next = _write;
    std::swap(_write, write_next);
  }

  auto read() const -> buffer const & { return *_read; }
  auto write() -> buffer & { return *_write; }
};

#endif // TRIPLE_BUFFER_HPP
