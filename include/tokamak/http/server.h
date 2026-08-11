#pragma once

#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace tokamak {

struct ServerConfig {
  std::string address = "0.0.0.0";
  std::uint16_t port = 8080;
  std::size_t num_threads = 1; // io_context thread pool size
};

class Server {
public:
  explicit Server(ServerConfig config);
  ~Server();

  void start();
  void stop();
  bool is_running() const;
  std::uint16_t port() const;
  void set_ready(bool ready);

private:
  boost::asio::awaitable<void> listener();
  boost::asio::awaitable<void>
  handle_connection(boost::asio::ip::tcp::socket socket);
  boost::beast::http::response<boost::beast::http::string_body> route(
      const boost::beast::http::request<boost::beast::http::string_body> &req);

  ServerConfig config_;
  boost::asio::io_context ioc_;
  std::vector<std::thread> threads_;
  std::atomic<bool> running_{false};
  std::atomic<bool> ready_{true};
  std::atomic<std::uint16_t> bound_port_{0};
};

} // namespace tokamak
