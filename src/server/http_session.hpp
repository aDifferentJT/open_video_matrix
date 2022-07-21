//
// Copyright (c) 2016-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/vinniefalco/CppCon2018
//

#ifndef BOOST_BEAST_EXAMPLE_WEBSOCKET_CHAT_MULTI_HTTP_SESSION_HPP
#define BOOST_BEAST_EXAMPLE_WEBSOCKET_CHAT_MULTI_HTTP_SESSION_HPP

#include "beast.hpp"
#include "log.hpp"
#include "net.hpp"
#include "synchronised.hpp"
#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <unordered_set>

#include "websocket_session.hpp"
#include <boost/config.hpp>

namespace http {
// Returns a bad request response
template <typename Body, typename Allocator>
auto bad_request(
    beast::http::request<Body, beast::http::basic_fields<Allocator>> const &req,
    std::string_view why) {
  auto res = beast::http::response<beast::http::string_body>{
      beast::http::status::bad_request, req.version()};
  res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
  res.set(beast::http::field::content_type, "text/html");
  res.keep_alive(req.keep_alive());
  res.body() = std::string(why);
  res.prepare_payload();
  return res;
}

// Returns a not found response
template <typename Body, typename Allocator>
auto not_found(
    beast::http::request<Body, beast::http::basic_fields<Allocator>> const
        &req) {
  auto res = beast::http::response<beast::http::string_body>{
      beast::http::status::not_found, req.version()};
  res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
  res.set(beast::http::field::content_type, "text/html");
  res.keep_alive(req.keep_alive());
  res.body() =
      "The resource '" + std::string(req.target()) + "' was not found.";
  res.prepare_payload();
  return res;
}

// Returns a server error response
template <typename Body, typename Allocator>
auto server_error(
    beast::http::request<Body, beast::http::basic_fields<Allocator>> const &req,
    std::string_view what) {
  auto res = beast::http::response<beast::http::string_body>{
      beast::http::status::internal_server_error, req.version()};
  res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
  res.set(beast::http::field::content_type, "text/html");
  res.keep_alive(req.keep_alive());
  res.body() = "An error occurred: '" + std::string(what) + "'";
  res.prepare_payload();
  return res;
}

template <typename Body, typename Allocator>
auto string_response(
    beast::http::request<Body, beast::http::basic_fields<Allocator>> const &req,
    std::string body, std::string_view mime_type, auto &&send) {
  // Cache the size since we need it after the move
  auto const size = body.size();

  switch (req.method()) {
  case beast::http::verb::head: {
    // Respond to HEAD request
    auto res = beast::http::response<beast::http::empty_body>{
        beast::http::status::ok, req.version()};
    res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(beast::http::field::content_type, mime_type);
    res.content_length(size);
    res.keep_alive(req.keep_alive());
    return send(res);
  }

  case beast::http::verb::get: {
    // Respond to GET request
    auto res = beast::http::response<beast::http::string_body>{
        std::piecewise_construct, std::make_tuple(std::move(body)),
        std::make_tuple(beast::http::status::ok, req.version())};
    res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(beast::http::field::content_type, mime_type);
    res.content_length(size);
    res.keep_alive(req.keep_alive());
    return send(res);
  }

  default:
    return send(http::bad_request(req, "Unknown HTTP-method"));
  }
}

template <typename Body, typename Allocator>
auto empty_response(
    beast::http::request<Body, beast::http::basic_fields<Allocator>> const
        &req) {
  auto res = beast::http::response<beast::http::empty_body>{
      beast::http::status::ok, req.version()};
  res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
  res.keep_alive(req.keep_alive());
  return res;
}

template <typename T>
concept delegate = requires(
    T delegate,
    beast::http::request<typename T::body_type, beast::http::fields> req) {
  delegate.handle_request(req, [](auto &&) {});
};

template <delegate Delegate, websocket::delegate WebsocketDelegate>
class session {
  std::shared_ptr<Delegate> delegate_;
  std::shared_ptr<WebsocketDelegate> websocket_delegate_;
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;

  // The parser is stored in an optional container so we can
  // construct it from scratch it at the beginning of each new message.
  boost::optional<beast::http::request_parser<typename Delegate::body_type>>
      parser_;

  struct send_lambda;

  using clients_t =
      synchronised<std::unordered_set<websocket::session<WebsocketDelegate> *>>;

  friend void do_read(std::unique_ptr<session> self, clients_t &clients) {
    auto &stream = self->stream_;
    auto &buffer = self->buffer_;
    auto &parser = self->parser_;

    parser.emplace();
    // parser->body_limit(1 << 30); // 1 GiB
    parser->body_limit(boost::none);

    // Set the timeout.
    stream.expires_after(std::chrono::seconds(30));

    // Read a request
    beast::http::async_read(
        stream, buffer, parser->get(),
        [self = std::move(self), &clients](
            beast::error_code ec, std::size_t bytes_transferred) mutable {
          on_read(std::move(self), clients, ec, bytes_transferred);
        });
  }

  static void on_read(std::unique_ptr<session> self, clients_t &clients,
                      beast::error_code ec, std::size_t) {
    auto &delegate = self->delegate_;
    auto &websocket_delegate = self->websocket_delegate_;
    auto &stream = self->stream_;
    auto &parser = self->parser_;

    // This means they closed the connection
    if (ec == beast::http::error::end_of_stream) {
      stream.socket().shutdown(tcp::socket::shutdown_send, ec);
      return;
    }

    // Handle the error, if any
    if (ec) {
      return fail(ec, "read");
    }

    // See if it is a WebSocket Upgrade
    if (beast::websocket::is_upgrade(parser->get())) {
      // Create a websocket session, transferring ownership
      // of both the socket and the HTTP request.
      websocket::run(websocket_delegate, stream.release_socket(), clients,
                     parser->release());
      return;
    }

    // Send the response
    delegate->handle_request(parser->release(), [self = std::move(self),
                                                 &clients](auto &&msg) mutable {
      auto &stream = self->stream_;

      // The lifetime of the message has to extend
      // for the duration of the async operation so
      // we use a unique_ptr to manage it.
      auto msg_ptr = std::make_unique<std::decay_t<decltype(msg)>>(
          std::forward<decltype(msg)>(msg));

      auto &msg_ref = *msg_ptr;

      // Write the response
      beast::http::async_write(
          stream, msg_ref,
          [self = std::move(self), &clients, msg = std::move(msg_ptr)](
              beast::error_code ec, std::size_t bytes) mutable {
            on_write(std::move(self), clients, ec, bytes, msg->need_eof());
          });
    });
  }

  static void on_write(std::unique_ptr<session> self, clients_t &clients,
                       beast::error_code ec, std::size_t, bool close) {
    // Handle the error, if any
    if (ec)
      return fail(ec, "write");

    if (close) {
      // This means we should close the connection, usually because
      // the response indicated the "Connection: close" semantic.
      self->stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
      return;
    }

    // Read another request
    do_read(std::move(self), clients);
  }

public:
  session(std::shared_ptr<Delegate> delegate,
          std::shared_ptr<WebsocketDelegate> websocket_delegate,
          tcp::socket &&socket)
      : delegate_{std::move(delegate)},
        websocket_delegate_{std::move(websocket_delegate)}, stream_{std::move(
                                                                socket)} {}

  template <delegate Delegate_, websocket::delegate WebsocketDelegate_>
  friend void
  run(std::shared_ptr<Delegate_> delegate,
      std::shared_ptr<WebsocketDelegate_> websocket_delegate,
      tcp::socket &&socket,
      typename session<Delegate_, WebsocketDelegate_>::clients_t &clients);
};

template <delegate Delegate, websocket::delegate WebsocketDelegate>
inline void
run(std::shared_ptr<Delegate> delegate,
    std::shared_ptr<WebsocketDelegate> websocket_delegate, tcp::socket &&socket,
    typename session<Delegate, WebsocketDelegate>::clients_t &clients) {
  do_read(std::make_unique<session<Delegate, WebsocketDelegate>>(
              std::move(delegate), std::move(websocket_delegate),
              std::move(socket)),
          clients);
}

} // namespace http

#endif
