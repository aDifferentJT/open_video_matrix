//
// Copyright (c) 2016-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/vinniefalco/CppCon2018
//

#ifndef BOOST_BEAST_EXAMPLE_WEBSOCKET_CHAT_MULTI_LISTENER_HPP
#define BOOST_BEAST_EXAMPLE_WEBSOCKET_CHAT_MULTI_LISTENER_HPP

#include "beast.hpp"
#include "http_session.hpp"
#include "log.hpp"
#include "net.hpp"
#include "synchronised.hpp"
#include "websocket_session.hpp"
#include <boost/smart_ptr.hpp>
#include <memory>
#include <string>
#include <unordered_set>

// Forward declaration
class shared_state;

// Accepts incoming connections and launches the sessions
namespace listener {
static auto create(net::io_context &ioc, tcp::endpoint endpoint)
    -> std::unique_ptr<tcp::acceptor> {
  auto acceptor = std::make_unique<tcp::acceptor>(ioc);

  beast::error_code ec;

  // Open the acceptor
  acceptor->open(endpoint.protocol(), ec);
  if (ec) {
    fail(ec, "open");
    return acceptor; // TODO is this right?
  }

  // Allow address reuse
  acceptor->set_option(net::socket_base::reuse_address(true), ec);
  if (ec) {
    fail(ec, "set_option");
    return acceptor; // TODO is this right?
  }

  // Bind to the server address
  acceptor->bind(endpoint, ec);
  if (ec) {
    fail(ec, "bind");
    return acceptor; // TODO is this right?
  }

  // Start listening for connections
  acceptor->listen(net::socket_base::max_listen_connections, ec);
  if (ec) {
    fail(ec, "listen");
    return acceptor; // TODO is this right?
  }

  return acceptor;
}

// Start accepting incoming connections
template <http::delegate HttpDelegate>
static void run(std::shared_ptr<HttpDelegate> http_delegate,
                std::shared_ptr<websocket::delegate> websocket_delegate,
                net::io_context &ioc, std::unique_ptr<tcp::acceptor> acceptor) {
  auto &acceptor_ = *acceptor;

  // The new connection gets its own strand
  acceptor_.async_accept(
      net::make_strand(ioc),
      [http_delegate = std::move(http_delegate),
       websocket_delegate = std::move(websocket_delegate), &ioc,
       acceptor = std::move(acceptor)](beast::error_code ec,
                                       tcp::socket socket) mutable {
        if (ec) {
          return fail(ec, "accept");
        } else {
          // Launch a new session for this connection
          http::run(http_delegate, websocket_delegate, std::move(socket));
        }

        run(http_delegate, websocket_delegate, ioc, std::move(acceptor));
      });
}
}; // namespace listener

#endif
