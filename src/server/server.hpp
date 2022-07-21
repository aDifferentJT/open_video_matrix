#ifndef SERVER_SERVER_HPP
#define SERVER_SERVER_HPP

#include "listener.hpp"
#include "synchronised.hpp"
#include "websocket_session.hpp"

#include <boost/asio/signal_set.hpp>
#include <boost/smart_ptr.hpp>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <vector>

template <http::delegate HttpDelegate, websocket::delegate WebsocketDelegate>
class server {
private:
  std::vector<std::thread> workers;

  net::io_context ioc;

  synchronised<std::unordered_set<websocket::session<WebsocketDelegate> *>>
      clients;

  unsigned short port_;

public:
  server(server const &) = delete;

  server(std::shared_ptr<HttpDelegate> http_delegate,
         std::shared_ptr<WebsocketDelegate> websocket_delegate,
         char const *address_, unsigned short port = 0, std::size_t no_threads = 4) {
    auto address = net::ip::make_address(address_);

    auto listener_ = listener::create(ioc, tcp::endpoint{address, port});
    port_ = listener_->local_endpoint().port();
    listener::run(std::move(http_delegate), std::move(websocket_delegate), ioc,
                  std::move(listener_), clients);

    workers.reserve(no_threads);
    for (std::size_t i = 0; i < no_threads; i += 1) {
      workers.emplace_back([this] { ioc.run(); });
    }
  }

  ~server() {
    ioc.stop();

    for (auto &t : workers) {
      t.join();
    }
  }

  auto port() { return port_; }

  void send(std::string msg) {
    auto shared_msg = std::make_shared<std::string const>(std::move(msg));

    auto locked_clients = clients.lock();
    for (auto client : locked_clients.get()) {
      websocket::send(client->shared_from_this(), shared_msg);
    }
  }
};

#endif // SERVER_SERVER_HPP
