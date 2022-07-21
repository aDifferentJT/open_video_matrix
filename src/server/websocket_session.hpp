#ifndef CONTROLLER_WEBSOCKET_SESSION_HPP
#define CONTROLLER_WEBSOCKET_SESSION_HPP

#include "beast.hpp"
#include "log.hpp"
#include "net.hpp"
#include "synchronised.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace websocket {

template <typename T>
concept delegate = requires(T delegate, beast::flat_buffer buffer) {
  delegate.on_read(buffer);
};

template <delegate Delegate>
class session
    : public std::enable_shared_from_this<session<Delegate>> {
  std::shared_ptr<Delegate> delegate_;
  beast::flat_buffer buffer_;
  beast::websocket::stream<beast::tcp_stream> ws_;
  std::vector<std::shared_ptr<std::string const>> queue_;
  synchronised<std::unordered_set<session *>> &clients;

  static void on_read(std::shared_ptr<session> self,
                      beast::error_code ec,
                      [[maybe_unused]] std::size_t bytes_transferred) {
    auto &delegate = self->delegate_;
    auto &buffer = self->buffer_;
    auto &ws = self->ws_;

    // Handle the error, if any
    if (ec) {
      return fail(ec, "read");
    }

    delegate->on_read(buffer);

    // Read another message
    ws.async_read(buffer,
                  [self = std::move(self)](beast::error_code ec,
                                           std::size_t bytes_transferred) {
                    on_read(std::move(self), ec, bytes_transferred);
                  });
  }

  static void on_write(std::shared_ptr<session> self,
                       beast::error_code ec,
                       [[maybe_unused]] std::size_t bytes_transferred) {
    auto &ws = self->ws_;
    auto &queue = self->queue_;

    // Handle the error, if any
    if (ec) {
      return fail(ec, "write");
    }

    // Remove the string from the queue
    queue.erase(queue.begin());

    // Send the next message if any
    if (!queue.empty()) {
      ws.async_write(net::buffer(*queue.front()),
                     [self = std::move(self)](beast::error_code ec,
                                              std::size_t bytes_transferred) {
                       on_write(std::move(self), ec, bytes_transferred);
                     });
    }
  }

public:
  session(
      std::shared_ptr<Delegate> delegate, tcp::socket &&socket,
      synchronised<std::unordered_set<session *>> &clients)
      : delegate_{std::move(delegate)}, ws_{std::move(socket)}, clients{
                                                                    clients} {}

  ~session() { clients->erase(this); }

  template <delegate Delegate_>
  friend void send(std::shared_ptr<session<Delegate_>> self,
                             std::shared_ptr<std::string const> const &ss);

  template <delegate Delegate_, typename Body, typename Allocator>
  friend auto run(
      std::shared_ptr<Delegate_> delegate, tcp::socket &&socket,
      synchronised<std::unordered_set<session<Delegate_> *>> &clients,
      beast::http::request<Body, beast::http::basic_fields<Allocator>> req)
      -> std::shared_ptr<session<Delegate_>>;
};

template <delegate Delegate, typename Body, typename Allocator>
inline auto run(
    std::shared_ptr<Delegate> delegate, tcp::socket &&socket,
    synchronised<std::unordered_set<session<Delegate> *>> &clients,
    beast::http::request<Body, beast::http::basic_fields<Allocator>> req)
    -> std::shared_ptr<session<Delegate>> {
  auto self =
      std::make_shared<session<Delegate>>(std::move(delegate), std::move(socket), clients);

  auto &ws = self->ws_;

  // Set suggested timeout settings for the websocket
  ws.set_option(
      beast::websocket::stream_base::timeout::suggested(beast::role_type::server));

  // Set a decorator to change the Server of the handshake
  ws.set_option(
      beast::websocket::stream_base::decorator([](beast::websocket::response_type &res) {
        res.set(beast::http::field::server, std::string(BOOST_BEAST_VERSION_STRING) +
                                         " open-video-matrix-controller");
      }));

  ws.binary(true);
  ws.read_message_max(1ull << 27);

  // Accept the websocket handshake
  ws.async_accept(req, [self](beast::error_code ec) mutable {
    auto &buffer = self->buffer_;
    auto &ws = self->ws_;

    // Handle the error, if any
    if (ec) {
      return fail(ec, "accept");
    }

    self->clients->insert(self.get());

    // Read a message
    ws.async_read(buffer,
                  [self = std::move(self)](beast::error_code ec,
                                           std::size_t bytes_transferred) {
                    session<Delegate>::on_read(std::move(self), ec,
                                                         bytes_transferred);
                  });
  });

  return self;
}

template <delegate Delegate>
inline void send(std::shared_ptr<session<Delegate>> self,
                           std::shared_ptr<std::string const> const &ss) {
  auto &ws = self->ws_;
  // Post our work to the strand, this ensures
  // that the members of `this` will not be
  // accessed concurrently.

  net::post(ws.get_executor(), [self = std::move(self), ss] {
    auto &ws = self->ws_;
    auto &queue = self->queue_;

    // Always add to queue
    queue.push_back(ss);

    // Are we already writing?
    if (queue.size() > 1)
      return;

    // We are not currently writing, so send this immediately
    ws.async_write(
        net::buffer(*queue.front()),
        beast::bind_front_handler(&session<Delegate>::on_write,
                                  std::move(self)));
  });
}

}

#endif // CONTROLLER_WEBSOCKET_SESSION_HPP
