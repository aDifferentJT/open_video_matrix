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

/** Represents an established HTTP connection
 */
class http_session {
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;

  // The parser is stored in an optional container so we can
  // construct it from scratch it at the beginning of each new message.
  boost::optional<http::request_parser<http::string_body>> parser_;

  struct send_lambda;

  friend void
  do_read(std::unique_ptr<http_session> self,
          synchronised<std::unordered_set<websocket_session *>> &clients,
          synchronised<std::ostream &> &ws_out, std::string_view doc_root) {
    auto &stream = self->stream_;
    auto &buffer = self->buffer_;
    auto &parser = self->parser_;

    // Construct a new parser for each message
    parser.emplace();

    // Apply a reasonable limit to the allowed size
    // of the body in bytes to prevent abuse.
    parser->body_limit(10000);

    // Set the timeout.
    stream.expires_after(std::chrono::seconds(30));

    // Read a request
    http::async_read(stream, buffer, parser->get(),
                     [self = std::move(self), &clients, &ws_out,
                      doc_root](beast::error_code ec,
                                std::size_t bytes_transferred) mutable {
                       on_read(std::move(self), clients, ws_out, doc_root, ec,
                               bytes_transferred);
                     });
  }

  static void
  on_read(std::unique_ptr<http_session> self,
          synchronised<std::unordered_set<websocket_session *>> &clients,
          synchronised<std::ostream &> &ws_out, std::string_view doc_root,
          beast::error_code ec, std::size_t bytes_transferred);

  static void
  on_write(std::unique_ptr<http_session> self,
           synchronised<std::unordered_set<websocket_session *>> &clients,
           synchronised<std::ostream &> &ws_out, std::string_view doc_root,
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
    do_read(std::move(self), clients, ws_out, doc_root);
  }

public:
  http_session(tcp::socket &&socket) : stream_{std::move(socket)} {}

  static void
  run(std::unique_ptr<http_session> self,
      synchronised<std::unordered_set<websocket_session *>> &clients,
      synchronised<std::ostream &> &ws_out, std::string_view doc_root) {
    do_read(std::move(self), clients, ws_out, doc_root);
  }
};

#endif
