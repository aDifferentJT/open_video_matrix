#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <thread>

#include <boost/process/child.hpp>
#include <boost/process/io.hpp>
#include <boost/process/search_path.hpp>

#include <fmt/format.h>

#include <range/v3/view/transform.hpp>

#include "ipc_shared_object.hpp"
#include "server/server.hpp"
#include "triple_buffer.hpp"

namespace ipc = boost::interprocess;

using fmt::operator""_a;

void alpha_over(triple_buffer::video_frame_t &dst,
                triple_buffer::video_frame_t const &src) {
  for (std::size_t i = 0; i < triple_buffer::size; i += 4) {
    // Div by 256 is much cheaper, so factor should range from 1 to 256
    // mult by 1 div by 256 will be 0

    auto factor = 256 - src[i + 3];
    for (std::size_t j = i; j < i + 4; j += 1) {
      dst[j] = static_cast<uint8_t>(src[j] + (dst[j] * factor) / 256);
    }
  }
}

void alpha_over(triple_buffer::buffer &dst, triple_buffer::buffer const &src) {
  alpha_over(dst.video_frame, src.video_frame);

  std::transform(src.audio_frame.begin(), src.audio_frame.end(),
                 dst.audio_frame.begin(), dst.audio_frame.begin(), std::plus{});
}

class io_device {
private:
  unsigned short _port;

  ipc_managed_object<triple_buffer> buffer;

public:
  io_device(io_device const &) = delete;
  io_device(io_device &&) = delete;

  io_device(unsigned short port) : _port{port} {}

  auto name() const -> std::string const & { return buffer.name(); }
  auto port() const -> unsigned short { return _port; }

  auto operator->() const -> triple_buffer const * { return buffer.data(); }
  auto operator->() -> triple_buffer * { return buffer.data(); }
};

class output_device {
private:
  io_device device;

public:
  output_device(auto &&...args)
      : device{std::forward<decltype(args)>(args)...} {}

  auto name() const -> std::string const & { return device.name(); }
  auto port() const -> unsigned short { return device.port(); }

  void done_writing() { device->done_writing(); }
  auto write() -> triple_buffer::buffer & { return device->write(); }
};

class input_device {
private:
  io_device device;

public:
  std::vector<std::weak_ptr<output_device>> outputs;

  input_device(auto &&...args)
      : device{std::forward<decltype(args)>(args)...} {}

  auto name() const -> std::string const & { return device.name(); }
  auto port() const -> unsigned short { return device.port(); }

  void about_to_read() { device->about_to_read(); }
  auto read() const -> triple_buffer::buffer const & { return device->read(); }

  auto has_output(output_device const *output) -> bool {
    for (auto &output2 : outputs) {
      if (output == output2.lock().get()) {
        return true;
      }
    }
    return false;
  }

  void add_output(std::shared_ptr<output_device> output) {
    if (!has_output(output.get())) {
      outputs.push_back(output);
    }
  }

  void remove_output(output_device const *output) {
    std::erase_if(outputs, [&](std::weak_ptr<output_device> const &output2) {
      return output == output2.lock().get();
    });
  }
};

class matrix {
public:
  std::vector<std::weak_ptr<input_device>> inputs;
  std::vector<std::weak_ptr<output_device>> outputs;

  std::function<void()> reload_clients = [] {};

private:
  struct has_name {
    std::string_view name;

    auto operator()(auto const &device) -> bool {
      return device.lock()->name() == name;
    }
  };

  auto find_input(std::string_view name) -> std::shared_ptr<input_device> {
    auto input = std::find_if(inputs.begin(), inputs.end(), has_name{name});
    if (input == inputs.end()) {
      return {};
    } else {
      return input->lock();
    }
  }

  auto find_output(std::string_view name) -> std::shared_ptr<output_device> {
    auto output = std::find_if(outputs.begin(), outputs.end(), has_name{name});
    if (output == outputs.end()) {
      return {};
    } else {
      return output->lock();
    }
  }

public:
  void add_input(std::weak_ptr<input_device> input) {
    inputs.push_back(std::move(input));
  }

  void add_output(std::weak_ptr<output_device> _output) {
    if (auto output = _output.lock()) {
      output->write() = {};
      output->done_writing();
      outputs.push_back(output);
    }
  }

  void remove_input(std::string_view name) {
    if (auto input = find_input(name)) {
      std::erase_if(inputs, [&](std::weak_ptr<input_device> const &input2) {
        return input == input2.lock();
      });
    } else {
      std::cerr << "Invalid input: " << name << '\n';
    }
  }

  void remove_output(std::string_view name) {
    if (auto output = find_output(name)) {
      for (auto &_input : inputs) {
        if (auto input = _input.lock()) {
          input->remove_output(output.get());
        }
      }
      std::erase_if(outputs, [&](std::weak_ptr<output_device> const &output2) {
        return output == output2.lock();
      });
    } else {
      std::cerr << "Invalid output: " << name << '\n';
    }
  }

  void bring_input_forward(std::string_view name) {
    auto input = std::find_if(inputs.begin(), inputs.end(), has_name{name});
    if (input != inputs.end() && input + 1 != inputs.end()) {
      std::swap(*input, *(input + 1));
    }
    reload_clients();
  }

  void bring_input_backward(std::string_view name) {
    auto input = std::find_if(inputs.begin(), inputs.end(), has_name{name});
    if (input != inputs.end() && input != inputs.begin()) {
      std::swap(*input, *(input - 1));
    }
    reload_clients();
  }

  auto is_connected(std::string_view input_name, std::string_view output_name)
      -> bool {
    auto input = find_input(input_name);
    auto output = find_output(output_name);

    if (input) {
      if (output) {
        return input->has_output(output.get());
      } else {
        std::cerr << "Invalid output: " << output_name << '\n';
        return false;
      }
    } else {
      if (output) {
        std::cerr << "Invalid input: " << input_name << '\n';
        return false;
      } else {
        std::cerr << "Invalid input: " << input_name
                  << " and output: " << output_name << '\n';
        return false;
      }
    }
  }

  void connect(std::string_view input_name, std::string_view output_name,
               bool value = true) {
    auto input = find_input(input_name);
    auto output = find_output(output_name);

    if (input) {
      if (output) {
        if (value) {
          input->add_output(output);
        } else {
          input->remove_output(output.get());
        }
      } else {
        std::cerr << "Invalid output: " << output_name << '\n';
      }
    } else {
      if (output) {
        std::cerr << "Invalid input: " << input_name << '\n';
      } else {
        std::cerr << "Invalid input: " << input_name
                  << " and output: " << output_name << '\n';
      }
    }

    reload_clients();
  }

  void run(auto duration) {
    auto nextFrame = std::chrono::steady_clock::time_point{};

    while (true) {
      nextFrame = std::chrono::steady_clock::now() + duration;

      std::erase_if(inputs, [](std::weak_ptr<input_device> const &input) {
        return input.expired();
      });
      std::erase_if(outputs, [](std::weak_ptr<output_device> const &output) {
        return output.expired();
      });
      for (auto &_input : inputs) {
        if (auto input = _input.lock()) {
          std::erase_if(input->outputs,
                        [](std::weak_ptr<output_device> const &output) {
                          return output.expired();
                        });
        }
      }
      // TODO if anything was erased, reload

      for (auto &_output : outputs) {
        if (auto output = _output.lock()) {
          output->write() = {};
        }
      }
      for (auto &_input : inputs) {
        if (auto input = _input.lock()) {
          if (!input->outputs.empty()) {
            input->about_to_read();
            for (auto &_output : input->outputs) {
              if (auto output = _output.lock()) {
                alpha_over(output->write(), input->read());
              }
            }
          }
        }
      }
      for (auto &_output : outputs) {
        if (auto output = _output.lock()) {
          output->done_writing();
        }
      }

      std::this_thread::sleep_until(nextFrame);
    }
  }
};

#include "router_html.hpp"

struct http_delegate {
  using body_type = beast::http::string_body;

  matrix &matrix_;

  http_delegate(matrix &matrix_) : matrix_{matrix_} {}

  template <typename Body, typename Allocator>
  void handle_request(
      beast::http::request<Body, beast::http::basic_fields<Allocator>> &&req,
      auto &&send) {
    if (req.target() == "/") {
      auto body = fmt::format(
          router_html,
          "output_headers"_a =
              fmt::join(matrix_.outputs |
                            ranges::views::transform(device_header_cell::make),
                        ""),
          "input_rows"_a =
              fmt::join(matrix_.inputs |
                            ranges::views::transform(
                                [&](std::weak_ptr<input_device> const &_input) {
                                  if (auto input = _input.lock()) {
                                    return input_row{*input, matrix_.outputs};
                                  } else {
                                    return input_row{};
                                  }
                                }),
                        ""));
      auto mime_type = "text/html"sv;

      return http::string_response(req, std::move(body), mime_type, send);
    } else if (req.target() == "/bring_input_forward") {
      matrix_.bring_input_forward(req.body());
      return send(http::empty_response(req));
    } else if (req.target() == "/bring_input_backward") {
      matrix_.bring_input_backward(req.body());
      return send(http::empty_response(req));
    } else if (req.target() == "/connect") {
      auto regex = std::regex{"([^&]*)&([^&]*)&(true|false)"};
      auto body = std::string{req.body()};
      if (std::smatch match; std::regex_match(body, match, regex)) {
        if (match.size() == 4) {
          auto const input = match[1].str();
          auto const output = match[2].str();
          auto const value = match[3] == "true";
          matrix_.connect(input, output, value);
          return send(http::empty_response(req));
        }
      }
      return send(http::bad_request(req, "Cannot parse body"));
    } else {
      return send(http::not_found(req));
    }
  }
};

class websocket_delegate : public websocket::tracking_delegate {
private:
  matrix &_matrix;

public:
  websocket_delegate(matrix &_matrix) : _matrix{_matrix} {}

  auto on_connect(websocket::session &client, std::string_view _target)
      -> std::any override {
    auto target = std::string{_target};
    if (auto matches = std::smatch{};
        std::regex_match(target, matches, std::regex{R"(input_(\d*))"})) {
      auto port = static_cast<unsigned short>(std::stoi(matches[1]));
      auto device = std::make_shared<input_device>(port);
      websocket::send(client.shared_from_this(),
                      std::make_shared<std::string>(device->name()));
      _matrix.add_input(device);
      return device;
    } else if (auto matches = std::smatch{}; std::regex_match(
                   target, matches, std::regex{R"(output_(\d*))"})) {
      auto port = static_cast<unsigned short>(std::stoi(matches[1]));
      auto device = std::make_shared<output_device>(port);
      websocket::send(client.shared_from_this(),
                      std::make_shared<std::string>(device->name()));
      _matrix.add_output(device);
      return device;
    } else {
      return this->websocket::tracking_delegate::on_connect(client, target);
    }
  }
};

int main(int, char **) {
  auto matrix_ = matrix{};

  auto http_delegate_ = std::make_shared<http_delegate>(matrix_);
  auto websocket_delegate_ = std::make_shared<websocket_delegate>(matrix_);
  auto server_ =
      server{http_delegate_, websocket_delegate_, "0.0.0.0", 8080, 4};

  matrix_.reload_clients = [&] { websocket_delegate_->send(""s); };

  matrix_.run(40ms);
}
