#include <catch2/catch_test_macros.hpp>

#include "tokamak/http/server.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

// Helper: synchronous HTTP GET against a running server
http::response<http::string_body> do_get(std::uint16_t port,
                                         const std::string &target) {
  asio::io_context ioc;
  tcp::socket socket(ioc);
  socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

  http::request<http::string_body> req{http::verb::get, target, 11};
  req.set(http::field::host, "localhost");
  http::write(socket, req);

  beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);

  return res;
}

// Wait for server to bind (port() becomes non-zero)
void wait_for_ready(const tokamak::Server &server) {
  while (server.port() == 0) {
    std::this_thread::sleep_for(1ms);
  }
}

} // namespace

TEST_CASE("Server start and stop lifecycle", "[server]") {
  tokamak::Server server(
      tokamak::ServerConfig{.address = "127.0.0.1", .port = 0});

  server.start();
  REQUIRE(server.is_running());
  wait_for_ready(server);
  REQUIRE(server.port() != 0);

  server.stop();
  REQUIRE_FALSE(server.is_running());

  // Double stop is safe
  server.stop();
  REQUIRE_FALSE(server.is_running());
}

TEST_CASE("GET /healthz returns 200 with status ok", "[server]") {
  tokamak::Server server(
      tokamak::ServerConfig{.address = "127.0.0.1", .port = 0});
  server.start();
  wait_for_ready(server);

  auto res = do_get(server.port(), "/healthz");

  REQUIRE(res.result() == http::status::ok);
  REQUIRE(res.body() == R"({"status":"ok"})");
  REQUIRE(res[http::field::content_type] == "application/json");

  server.stop();
}

TEST_CASE("GET /readyz returns 200 when ready, 503 when not", "[server]") {
  tokamak::Server server(
      tokamak::ServerConfig{.address = "127.0.0.1", .port = 0});
  server.start();
  wait_for_ready(server);

  auto res1 = do_get(server.port(), "/readyz");
  REQUIRE(res1.result() == http::status::ok);
  REQUIRE(res1.body() == R"({"status":"ready"})");

  server.set_ready(false);

  auto res2 = do_get(server.port(), "/readyz");
  REQUIRE(res2.result() == http::status::service_unavailable);
  REQUIRE(res2.body() == R"({"status":"not_ready"})");

  server.stop();
}

TEST_CASE("GET unknown path returns 404", "[server]") {
  tokamak::Server server(
      tokamak::ServerConfig{.address = "127.0.0.1", .port = 0});
  server.start();
  wait_for_ready(server);

  auto res = do_get(server.port(), "/foobar");

  REQUIRE(res.result() == http::status::not_found);
  REQUIRE(res.body() == R"({"error":"not_found"})");

  server.stop();
}
