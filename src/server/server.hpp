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

template <http::delegate HttpDelegate> class server {
private:
  std::vector<std::thread> workers;

  net::io_context ioc;

  unsigned short port_;

public:
  server(server const &) = delete;

  server(std::shared_ptr<HttpDelegate> http_delegate,
         std::shared_ptr<websocket::delegate> websocket_delegate,
         char const *address_, unsigned short port = 0,
         std::size_t no_threads = 4) {
    auto address = net::ip::make_address(address_);

    auto listener_ = listener::create(ioc, {address, port});
    port_ = listener_->local_endpoint().port();
    listener::run(std::move(http_delegate), std::move(websocket_delegate), ioc,
                  std::move(listener_));

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

  auto connect_to_websocket(std::shared_ptr<websocket::delegate> _delegate,
                            char const *address, unsigned short port,
                            std::string_view target, std::any user_data = {})
      -> std::shared_ptr<websocket::session> {
    return websocket::connect_to_server(std::move(_delegate),
                                        {net::ip::make_address(address), port},
                                        target, ioc, std::move(user_data));
  }
};

#endif // SERVER_SERVER_HPP
