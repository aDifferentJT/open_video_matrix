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
  static constexpr auto size = 1920 * 1080 * 4;
  using buffer = std::array<uint8_t, size>;

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

  auto novel_to_read() { return _read != read_next; }

  void about_to_read() {
    auto lock = ipc::scoped_lock{mutex};
    if (_read != read_next) {
      write_next = _read;
    }
    _read = read_next;
  }

  void done_writing() {
    auto lock = ipc::scoped_lock{mutex};
    read_next = _write;
    std::swap(_write, write_next);
  }

  auto read() const -> buffer const & { return *_read; }
  auto write() -> buffer & { return *_write; }
};

#endif // TRIPLE_BUFFER_HPP
