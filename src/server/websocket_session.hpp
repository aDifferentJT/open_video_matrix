#ifndef CONTROLLER_WEBSOCKET_SESSION_HPP
#define CONTROLLER_WEBSOCKET_SESSION_HPP

#include "beast.hpp"
#include "log.hpp"
#include "net.hpp"
#include "synchronised.hpp"

#include <any>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace websocket {
class session;

class delegate {
public:
  virtual ~delegate() = default;

private:
  virtual auto on_connect(session &, std::string_view target) -> std::any = 0;
  virtual void on_disconnect(std::any &, session &) = 0;
  virtual void on_read(std::any &, beast::flat_buffer &) = 0;

  friend class session;

  template <typename Body, typename Allocator>
  friend auto
  run(std::shared_ptr<delegate> delegate, tcp::socket &&socket,
      beast::http::request<Body, beast::http::basic_fields<Allocator>> req)
      -> std::shared_ptr<session>;
};

class session : public std::enable_shared_from_this<session> {
private:
  std::shared_ptr<delegate> _delegate;
  beast::flat_buffer _buffer;
  beast::websocket::stream<beast::tcp_stream> ws_;
  std::vector<std::shared_ptr<std::string const>> queue_;

  std::any user_data;

  static void on_read(std::shared_ptr<session> self, beast::error_code ec,
                      [[maybe_unused]] std::size_t bytes_transferred) {
    auto &delegate = self->_delegate;
    auto &buffer = self->_buffer;
    auto &ws = self->ws_;

    // Handle the error, if any
    if (ec) {
      return fail(ec, "read");
    }

    delegate->on_read(self->user_data, buffer);

    // Read another message
    ws.async_read(buffer,
                  [self = std::move(self)](beast::error_code ec,
                                           std::size_t bytes_transferred) {
                    on_read(std::move(self), ec, bytes_transferred);
                  });
  }

  static void on_write(std::shared_ptr<session> self, beast::error_code ec,
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
  session(std::shared_ptr<delegate> _delegate, tcp::socket &&socket)
      : _delegate{std::move(_delegate)}, ws_{std::move(socket)} {}

  session(std::shared_ptr<delegate> _delegate, net::io_context &ioc)
      : _delegate{std::move(_delegate)}, ws_{net::make_strand(ioc)} {}

  ~session() { _delegate->on_disconnect(user_data, *this); }

  template <typename Body, typename Allocator>
  friend auto
  run(std::shared_ptr<delegate> delegate, tcp::socket &&socket,
      beast::http::request<Body, beast::http::basic_fields<Allocator>> req)
      -> std::shared_ptr<session>;

  friend auto connect_to_server(std::shared_ptr<delegate> _delegate,
                                net::ip::basic_endpoint<tcp> endpoint,
                                std::string_view target, net::io_context &ioc,
                                std::any user_data) -> std::shared_ptr<session>;

  friend void send(std::shared_ptr<session> self,
                   std::shared_ptr<std::string const> const &ss);
};

template <typename Body, typename Allocator>
inline auto
run(std::shared_ptr<delegate> _delegate, tcp::socket &&socket,
    beast::http::request<Body, beast::http::basic_fields<Allocator>> req)
    -> std::shared_ptr<session> {
  auto self =
      std::make_shared<session>(std::move(_delegate), std::move(socket));

  auto &ws = self->ws_;

  // Set suggested timeout settings for the websocket
  ws.set_option(beast::websocket::stream_base::timeout::suggested(
      beast::role_type::server));

  // Set a decorator to change the Server of the handshake
  ws.set_option(beast::websocket::stream_base::decorator(
      [](beast::websocket::response_type &res) {
        res.set(beast::http::field::server,
                std::string(BOOST_BEAST_VERSION_STRING) +
                    " open-video-matrix-server");
      }));

  ws.binary(true);

  // Accept the websocket handshake
  ws.async_accept(req, [self, req](beast::error_code ec) mutable {
    auto &buffer = self->_buffer;
    auto &ws = self->ws_;

    // Handle the error, if any
    if (ec) {
      return fail(ec, "accept");
    }

    self->user_data = self->_delegate->on_connect(*self, req.target());

    // Read a message
    ws.async_read(buffer,
                  [self = std::move(self)](beast::error_code ec,
                                           std::size_t bytes_transferred) {
                    session::on_read(std::move(self), ec, bytes_transferred);
                  });
  });

  return self;
}

inline auto connect_to_server(std::shared_ptr<delegate> _delegate,
                              tcp::endpoint endpoint, std::string_view target,
                              net::io_context &ioc, std::any user_data = {})
    -> std::shared_ptr<session> {
  auto self = std::make_shared<session>(std::move(_delegate), ioc);

  self->user_data = std::move(user_data);

  auto &ws = self->ws_;

  beast::get_lowest_layer(ws).async_connect(
      endpoint, [self, endpoint, target](beast::error_code ec) {
        if (ec) {
          return fail(ec, "connect");
        }

        beast::get_lowest_layer(self->ws_).expires_never();

        auto &ws = self->ws_;

        // Set suggested timeout settings for the websocket
        ws.set_option(beast::websocket::stream_base::timeout::suggested(
            beast::role_type::server));

        // Set a decorator to change the Server of the handshake
        ws.set_option(beast::websocket::stream_base::decorator(
            [](beast::websocket::response_type &res) {
              res.set(beast::http::field::user_agent,
                      std::string(BOOST_BEAST_VERSION_STRING) +
                          " open-video-matrix-client");
            }));

        ws.binary(true);

        ws.async_handshake(
            endpoint.address().to_string(), target,
            [self](beast::error_code ec) {
              if (ec) {
                return fail(ec, "connect");
              }

              auto &buffer = self->_buffer;
              auto &ws = self->ws_;

              ws.async_read(buffer, [self = std::move(self)](
                                        beast::error_code ec,
                                        std::size_t bytes_transferred) {
                session::on_read(std::move(self), ec, bytes_transferred);
              });
            });
      });

  return self;
}

inline void send(std::shared_ptr<session> self,
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
        beast::bind_front_handler(&session::on_write, std::move(self)));
  });
}

class client_delegate : public delegate {
  synchronised<std::unordered_set<websocket::session *>> clients;

  auto on_connect(session &, [[maybe_unused]] std::string_view target)
      -> std::any override {
    std::terminate();
  }
  void on_disconnect(std::any &, session &) override {}
};

template <typename F> class read_client_delegate : public client_delegate {
private:
  F f;

public:
  read_client_delegate(F f) : f{std::move(f)} {}

private:
  void on_read(std::any &user_data, beast::flat_buffer &buffer) override {
    f(user_data, buffer);
  }
};

template <typename F>
auto make_read_client_delegate(F f) -> std::shared_ptr<read_client_delegate<F>> {
  return std::make_shared<read_client_delegate<F>>(std::move(f));
}

class tracking_delegate : public delegate {
private:
  synchronised<std::unordered_set<websocket::session *>> clients;

public:
  auto on_connect(session &client, [[maybe_unused]] std::string_view target)
      -> std::any override {
    clients->insert(&client);
    return {};
  }
  void on_disconnect(std::any &, session &client) override {
    clients->erase(&client);
  }

  void on_read(std::any &, beast::flat_buffer &) override {}

  void send(std::string msg) {
    auto shared_msg = std::make_shared<std::string const>(std::move(msg));

    auto locked_clients = clients.lock();
    for (auto client : locked_clients.get()) {
      websocket::send(client->shared_from_this(), shared_msg);
    }
  }
};
} // namespace websocket

#endif // CONTROLLER_WEBSOCKET_SESSION_HPP
