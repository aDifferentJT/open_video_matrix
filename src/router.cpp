#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
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

void alpha_over(triple_buffer::buffer &dst, triple_buffer::buffer const &src) {
#pragma omp parallel for schedule(static)
  for (std::size_t i = 0; i < triple_buffer::size; i += 4) {
    // Div by 256 is much cheaper, so factor should range from 1 to 256
    // mult by 1 div by 256 will be 0

    auto factor = 256 - src[i + 3];
    for (std::size_t j = i; j < i + 4; j += 1) {
      dst[j] = static_cast<uint8_t>(src[j] + (dst[j] * factor) / 256);
    }
  }
}

class io_device {
private:
  std::string _name;

  std::atomic<triple_buffer *> buffer = nullptr;
  std::atomic<std::atomic<unsigned short> *> _port = nullptr;
  std::atomic<std::atomic<bool> *> alive = nullptr;

  std::thread watchdog;

public:
  io_device(io_device const &) = delete;
  io_device(io_device &&) = delete;

  // TODO other args, ownership difficult
  io_device(std::string_view name, std::string executable)
      : _name{std::move(name)}, watchdog{[this, executable =
                                                    std::move(executable)]() {
          auto buffer = ipc_managed_object<triple_buffer>{};
          auto port = std::atomic<unsigned short>{0};
          auto alive = std::atomic<bool>{true};
          this->buffer = buffer.data();
          this->_port = &port;
          this->alive = &alive;

          auto is = boost::process::ipstream{};
          auto os = boost::process::opstream{};

          // Henceforth only use things owned by the thread
          [&buffer, &port, &alive, &executable, &is, &os] {
            while (alive) {
              try {
                auto p = boost::process::child{
                    executable.c_str(), buffer.name().c_str(),
                    boost::process::std_out > is, boost::process::std_in < os};
                {
                  unsigned short _port;
                  is >> _port;
                  port = _port;
                }
                // TODO reload_clients();
                p.wait();
              } catch (boost::process::process_error const &e) {
                std::cerr << "Error in " << executable << ": " << e.what()
                          << '\n';
              }
              std::this_thread::sleep_for(1s);
            }
          }();
        }} {
    // TODO a barrier would be a better choice here
    while (buffer == nullptr || _port == nullptr || alive == nullptr)
      ;
    watchdog.detach();
  }

  ~io_device() {
    if (alive != nullptr) {
      *alive = false;
    }
  }

  auto name() const -> std::string const & { return _name; }
  auto port() const -> unsigned short { return *_port; }

  auto operator->() const -> triple_buffer const * { return buffer; }
  auto operator->() -> triple_buffer * { return buffer; }
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
  std::vector<output_device *> outputs;

  input_device(auto &&...args)
      : device{std::forward<decltype(args)>(args)...} {}

  auto name() const -> std::string const & { return device.name(); }
  auto port() const -> unsigned short { return device.port(); }

  void about_to_read() { device->about_to_read(); }
  auto read() const -> triple_buffer::buffer const & { return device->read(); }

  auto has_output(output_device *output) -> bool {
    return std::find(outputs.begin(), outputs.end(), output) != outputs.end();
  }

  void add_output(output_device *output) {
    if (!has_output(output)) {
      outputs.push_back(output);
    }
  }

  void remove_output(output_device *output) { std::erase(outputs, output); }
};

class matrix {
public:
  std::vector<std::unique_ptr<input_device>> inputs;
  std::vector<std::unique_ptr<output_device>> outputs;

  std::function<void()> reload_clients = []{};

private:
  struct has_name {
    std::string_view name;

    auto operator()(auto const &device) -> bool {
      return device->name() == name;
    }
  };

  auto find_input(std::string_view name) {
    return std::find_if(inputs.begin(), inputs.end(), has_name{name});
  }

  auto find_output(std::string_view name) {
    return std::find_if(outputs.begin(), outputs.end(), has_name{name});
  }

public:
  void add_input(auto &&...args) {
    inputs.push_back(
        std::make_unique<input_device>(std::forward<decltype(args)>(args)...));
  }

  void add_output(auto &&...args) {
    auto &output = outputs.emplace_back(
        std::make_unique<output_device>(std::forward<decltype(args)>(args)...));
    output->write() = {};
    output->done_writing();
  }

  void remove_input(std::string_view name) {
    auto input = find_input(name);
    if (input == inputs.end()) {
      std::cerr << "Invalid input: " << name << '\n';
    } else {
      inputs.erase(input);
    }
  }

  void remove_output(std::string_view name) {
    auto output = find_output(name);
    if (output == outputs.end()) {
      std::cerr << "Invalid output: " << name << '\n';
    } else {
      for (auto &input : inputs) {
        input->remove_output(output->get());
      }
      outputs.erase(output);
    }
  }

  auto is_connected(std::string_view input_name, std::string_view output_name)
      -> bool {
    auto input = find_input(input_name);
    auto output = find_output(output_name);
    if (input == inputs.end() && output == outputs.end()) {
      std::cerr << "Invalid input: " << input_name
                << " and output: " << output_name << '\n';
      return false;
    } else if (input == inputs.end()) {
      std::cerr << "Invalid input: " << input_name << '\n';
      return false;
    } else if (output == outputs.end()) {
      std::cerr << "Invalid output: " << output_name << '\n';
      return false;
    } else {
      return (*input)->has_output(output->get());
    }
  }

  void connect(std::string_view input_name, std::string_view output_name,
               bool value = true) {
    auto input = find_input(input_name);
    auto output = find_output(output_name);
    if (input == inputs.end() && output == outputs.end()) {
      std::cerr << "Invalid input: " << input_name
                << " and output: " << output_name << '\n';
    } else if (input == inputs.end()) {
      std::cerr << "Invalid input: " << input_name << '\n';
    } else if (output == outputs.end()) {
      std::cerr << "Invalid output: " << output_name << '\n';
    } else if (value) {
      (*input)->add_output(output->get());
    } else {
      (*input)->remove_output(output->get());
    }
    reload_clients();
  }

  void run() {
    while (true) {
      for (auto &output : outputs) {
        output->write() = {};
      }
      for (auto &input : inputs) {
        if (!input->outputs.empty()) {
          input->about_to_read();
          for (auto &output : input->outputs) {
            alpha_over(output->write(), input->read());
          }
        }
      }
      for (auto &output : outputs) {
        output->done_writing();
      }
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
          "input_rows"_a = fmt::join(
              matrix_.inputs |
                  ranges::views::transform(
                      [&](std::unique_ptr<input_device> const &input) {
                        return input_row{*input, matrix_.outputs};
                      }),
              ""));
      auto mime_type = "text/html"sv;

      return http::string_response(req, std::move(body), mime_type, send);
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

class websocket_delegate {
public:
  void on_read(beast::flat_buffer &buffer) {
    // TODO
  }
};

int main([[maybe_unused]] int argc, [[maybe_unused]] char **argv) {
  auto matrix_ = matrix{};

  auto http_delegate_ = std::make_shared<http_delegate>(matrix_);
  auto websocket_delegate_ = std::make_shared<websocket_delegate>();
  auto server_ =
      server{http_delegate_, websocket_delegate_, "0.0.0.0", 8080, 4};

  matrix_.reload_clients = [&] { server_.send(""s); };

  matrix_.add_input(
      "web_source 1",
      "./web_source/Release/web_source.app/Contents/MacOS/web_source");
  matrix_.add_input(
      "web_source 2",
      "./web_source/Release/web_source.app/Contents/MacOS/web_source");
  matrix_.add_input("colour_source", "./colour_source");
  matrix_.add_input("presentation_source", "./presentation_source");
  matrix_.add_output("ndi_output", "./ndi_output");
  matrix_.add_output("decklink_output", "./decklink_output");

  matrix_.connect("web_source 1", "ndi_output");

  matrix_.run();

  auto volatile x = 0;
  while (true) {
    x = 0;
  }
}
