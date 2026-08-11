#include "tokamak/http/server.h"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/write.hpp>
#include <utility>

namespace tokamak {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

Server::Server(ServerConfig config) : config_(std::move(config)) {}

Server::~Server() { stop(); }

void Server::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  asio::co_spawn(ioc_, listener(), asio::detached);
  for (std::size_t i = 0; i < config_.num_threads; i++) {
    threads_.emplace_back([this] { ioc_.run(); });
  }
}

void Server::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }
  ioc_.stop();
  for (auto &t : threads_) {
    t.join();
  }
  threads_.clear();
}

bool Server::is_running() const { return running_.load(); }

std::uint16_t Server::port() const { return bound_port_.load(); }

void Server::set_ready(bool ready) { ready_.store(ready); }

asio::awaitable<void> Server::listener() {
  auto executor = co_await asio::this_coro::executor;
  tcp::acceptor acceptor(executor);
  acceptor.open(tcp::v4());
  acceptor.set_option(tcp::acceptor::reuse_address(true));
  acceptor.bind({asio::ip::make_address(config_.address), config_.port});
  acceptor.listen(asio::socket_base::max_listen_connections);
  bound_port_.store(acceptor.local_endpoint().port());

  while (running_.load()) {
    auto [ec, socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (ec) {
      break;
    }
    asio::co_spawn(executor, handle_connection(std::move(socket)),
                   asio::detached);
  }
}

asio::awaitable<void> Server::handle_connection(tcp::socket socket) {
  beast::flat_buffer buffer;

  while (true) {
    http::request<http::string_body> req;
    auto [read_ec, bytes_read] = co_await http::async_read(
        socket, buffer, req, asio::as_tuple(asio::use_awaitable));
    (void)bytes_read;
    if (read_ec) {
      break;
    }

    auto response = route(req);
    response.set(http::field::content_type, "application/json");
    response.keep_alive(req.keep_alive());
    response.prepare_payload();

    auto [write_ec, bytes_written] = co_await http::async_write(
        socket, response, asio::as_tuple(asio::use_awaitable));
    (void)bytes_written;
    if (write_ec || !req.keep_alive()) {
      break;
    }
  }
  beast::error_code ec;
  socket.shutdown(tcp::socket::shutdown_send, ec);
  (void)ec;
}

http::response<http::string_body>
Server::route(const http::request<http::string_body> &req) {
  auto make_response = [&](http::status status, std::string body) {
    http::response<http::string_body> res{status, req.version()};
    res.body() = std::move(body);
    return res;
  };

  if (req.method() == http::verb::get && req.target() == "/healthz") {
    return make_response(http::status::ok, R"({"status":"ok"})");
  }

  if (req.method() == http::verb::get && req.target() == "/readyz") {
    if (ready_.load()) {
      return make_response(http::status::ok, R"({"status":"ready"})");
    }
    return make_response(http::status::service_unavailable,
                         R"({"status":"not_ready"})");
  }

  return make_response(http::status::not_found, R"({"error":"not_found"})");
}

} // namespace tokamak
