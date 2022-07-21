#ifndef CONTROLLER_LOG_HPP
#define CONTROLLER_LOG_HPP

#include "beast.hpp"
#include "net.hpp"
#include <iostream>

inline void fail(beast::error_code ec, std::string_view what) {
  // Don't report these
  if (ec == net::error::operation_aborted || ec == websocket::error::closed)
    return;

  std::cerr << what << ": " << ec.message() << "\n";
}

#endif // CONTROLLER_LOG_HPP
