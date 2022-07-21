#include "listener.hpp"
#include "synchronised.hpp"
#include "websocket_session.hpp"

#include <boost/asio/signal_set.hpp>
#include <boost/smart_ptr.hpp>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <vector>

void send_to_all(synchronised<std::unordered_set<websocket_session *>> &clients,
                 std::string msg) {
  auto shared_msg = std::make_shared<std::string const>(std::move(msg));

  auto locked_clients = clients.lock();
  for (auto client : locked_clients.get()) {
    websocket_session::send(client->shared_from_this(), shared_msg);
  }
}

int main(int argc, char *argv[]) {
  // Check command line arguments.
  if (argc != 5) {
    std::cerr << "Usage: controller <address> <port> <doc_root> <threads>\n"
              << "Example:\n"
              << "    websocket-chat-server 0.0.0.0 8080 . 5\n";
    return EXIT_FAILURE;
  }
  auto address = net::ip::make_address(argv[1]);
  auto port = static_cast<unsigned short>(std::atoi(argv[2]));
  auto doc_root = argv[3];
  auto const threads =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::atoi(argv[4])));

  net::io_context ioc;

  auto clients = synchronised<std::unordered_set<websocket_session *>>{};

  auto synchronised_cout = synchronised<std::ostream &>{std::cout};

  listener::run(ioc, listener::create(ioc, tcp::endpoint{address, port}),
                clients, synchronised_cout, doc_root);

  std::vector<std::thread> v;
  v.reserve(threads);
  for (auto i = threads; i > 0; --i) {
    v.emplace_back([&ioc] { ioc.run(); });
  }

  while (true) {
    ORPC::message msg;
    std::cin >> msg;
    auto ss = std::stringstream{};
    ss << msg;

    send_to_all(clients, std::move(ss).str());
  }
  // TODO Wait for stdin to close, this is the signal to shutdown
  while (std::cin.get() != -1 || !std::cin.eof()) {
  }

  ioc.stop();

  for (auto &t : v) {
    t.join();
  }

  return EXIT_SUCCESS;
}
