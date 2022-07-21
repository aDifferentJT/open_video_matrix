#
# Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/boostorg/beast
#

file (GLOB APP_FILES
  beast.hpp
  log.hpp
  http_session.cpp
  http_session.hpp
  listener.hpp
  main.cpp
  net.hpp
  websocket_session.hpp
  chat_client.html
  README.md
)

add_executable (controller
    ${APP_FILES}
    ${BOOST_BEAST_FILES}
)

