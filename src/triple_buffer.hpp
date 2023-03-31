#ifndef TRIPLE_BUFFER_HPP
#define TRIPLE_BUFFER_HPP

#include <array>
#include <utility>

#include <boost/interprocess/offset_ptr.hpp>
#include <boost/interprocess/sync/interprocess_condition_any.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

namespace ipc = boost::interprocess;

class triple_buffer {
public:
  static constexpr auto width = 1920;
  static constexpr auto pitch = width * 4;
  static constexpr auto height = 1080;
  static constexpr auto size = pitch * height;

  using video_frame_t = uint8_t[size];

  static constexpr auto sample_rate = 48'000;
  static constexpr auto frame_rate = 25;
  static constexpr auto num_channels = 2;
  static constexpr auto audio_samples_per_frame_per_channel =
      sample_rate / frame_rate;
  static constexpr auto audio_samples_per_frame_all_channels =
      audio_samples_per_frame_per_channel * num_channels;

  using audio_frame_t = int32_t[audio_samples_per_frame_all_channels];

  struct buffer {
    uint8_t video_frame[size];
    int32_t audio_frame[audio_samples_per_frame_all_channels];

    void clear() {
      std::fill(std::begin(video_frame), std::end(video_frame), 0);
      std::fill(std::begin(audio_frame), std::end(audio_frame), 0);
    }
  };

private:
  ipc::interprocess_mutex mutex;
  ipc::interprocess_condition_any sync;

  std::array<buffer, 3> buffers;
  ipc::offset_ptr<buffer> _read;
  ipc::offset_ptr<buffer> read_next;
  ipc::offset_ptr<buffer> _write;
  ipc::offset_ptr<buffer> write_next;

public:
  triple_buffer()
      : buffers{}, _read{&buffers[0]}, read_next{&buffers[0]},
        _write{&buffers[1]}, write_next{&buffers[2]} {}

  auto novel_to_read() const {
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

  void trigger_sync() { sync.notify_all(); }

  void wait_for_sync() {
    struct dummy_lock {
      bool locked = true;
      void lock() { locked = true; }
      void unlock() { locked = false; }
      operator bool() const { return locked; }
    };
    auto lock = dummy_lock{};
    sync.wait(lock);
  }
};

#endif // TRIPLE_BUFFER_HPP
