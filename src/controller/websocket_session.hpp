#ifndef CONTROLLER_WEBSOCKET_SESSION_HPP
#define CONTROLLER_WEBSOCKET_SESSION_HPP

#include "beast.hpp"
#include "log.hpp"
#include "net.hpp"
#include "synchronised.hpp"

#include "../open_rpc.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

/** Represents an active WebSocket connection to the server
 */
class websocket_session
    : public std::enable_shared_from_this<websocket_session> {
  beast::flat_buffer buffer_;
  websocket::stream<beast::tcp_stream> ws_;
  std::vector<std::shared_ptr<std::string const>> queue_;
  synchronised<std::unordered_set<websocket_session *>> &clients;

  static void on_read(std::shared_ptr<websocket_session> self,
                      synchronised<std::ostream &> &ws_out,
                      beast::error_code ec, [[maybe_unused]] std::size_t bytes_transferred) {
    auto &buffer = self->buffer_;
    auto &ws = self->ws_;

    // Handle the error, if any
    if (ec) {
      return fail(ec, "read");
    }

    try {
      std::size_t bytes_read = 0;
      auto bytes =
          std::span<char const>{static_cast<char const *>(buffer.data().data()),
                                buffer.data().size()};
      auto msg = ORPC::message_view::from(bytes, bytes_read);
      ws_out.lock().get() << msg << std::flush;
      buffer.consume(bytes_read);
    } catch (ORPC::insufficient_data const &) {
      std::cerr << "Incomplete message\n";
    }

    buffer.clear();

    // Read another message
    ws.async_read(buffer,
                  [self = std::move(self), &ws_out](
                      beast::error_code ec, std::size_t bytes_transferred) {
                    on_read(std::move(self), ws_out, ec, bytes_transferred);
                  });
  }

  static void on_write(std::shared_ptr<websocket_session> self,
                       beast::error_code ec, [[maybe_unused]] std::size_t bytes_transferred) {
    auto &ws = self->ws_;
    auto &queue = self->queue_;

    // Handle the error, if any
    if (ec)
      return fail(ec, "write");

    // Remove the string from the queue
    queue.erase(queue.begin());

    // Send the next message if any
    if (!queue.empty())
      ws.async_write(net::buffer(*queue.front()),
                     beast::bind_front_handler(&websocket_session::on_write,
                                               std::move(self)));
  }

public:
  websocket_session(
      tcp::socket &&socket,
      synchronised<std::unordered_set<websocket_session *>> &clients)
      : ws_(std::move(socket)), clients{clients} {}

  ~websocket_session() { clients->erase(this); }

  template <class Body, class Allocator>
  static void run(std::shared_ptr<websocket_session> self,
                  synchronised<std::ostream &> &ws_out,
                  http::request<Body, http::basic_fields<Allocator>> req) {
    auto &ws = self->ws_;

    // Set suggested timeout settings for the websocket
    ws.set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::server));

    // Set a decorator to change the Server of the handshake
    ws.set_option(
        websocket::stream_base::decorator([](websocket::response_type &res) {
          res.set(http::field::server, std::string(BOOST_BEAST_VERSION_STRING) +
                                           " open-video-matrix-controller");
        }));

    ws.binary(true);
    ws.read_message_max(1ull << 27);

    // Accept the websocket handshake
    ws.async_accept(
        req, [self = std::move(self), &ws_out](beast::error_code ec) mutable {
          auto &buffer = self->buffer_;
          auto &ws = self->ws_;

          // Handle the error, if any
          if (ec) {
            return fail(ec, "accept");
          }

          self->clients->insert(self.get());

          // Read a message
          ws.async_read(buffer, [self = std::move(self),
                                 &ws_out](beast::error_code ec,
                                          std::size_t bytes_transferred) {
            on_read(std::move(self), ws_out, ec, bytes_transferred);
          });
        });
  }

  static void send(std::shared_ptr<websocket_session> self,
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
      ws.async_write(net::buffer(*queue.front()),
                     beast::bind_front_handler(&websocket_session::on_write,
                                               std::move(self)));
    });
  }
};

#endif // CONTROLLER_WEBSOCKET_SESSION_HPP
