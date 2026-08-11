# Learnings 012 — HTTP Server Skeleton: First Contact with Boost.Asio/Beast

**Date**: 2026-08-11
**Piece**: Milestone 2, Piece C (Asio server skeleton + `/healthz` + `/readyz`)
**Related**: ADR-011 (HTTP Frontend Architecture), learnings_011 (Engine thread)

---

## 1. `FetchContent` + `SYSTEM` — suppressing third-party warnings

Boost's internal implementation has numerous `-Wsign-conversion` violations
(e.g. `socket_ops.ipp` assigns `signed_size_type` to `size_t` pervasively).
Under our `-Werror` policy, these become hard errors the moment any
translation unit includes a Boost header.

The fix is a single keyword in `FetchContent_Declare`:

```cmake
FetchContent_Declare(Boost
    GIT_REPOSITORY https://github.com/boostorg/boost.git
    GIT_TAG        boost-1.86.0
    GIT_SHALLOW    TRUE
    SYSTEM                # <-- this
)
```

`SYSTEM` (CMake ≥ 3.25) causes all include directories exported by the
fetched target to be added via `-isystem` rather than `-I`. The compiler
then treats them as system headers and suppresses all warnings — exactly
what we want for code we don't own.

**Rule**: every `FetchContent_Declare` for a third-party dependency should
include `SYSTEM` unless we explicitly want our warning flags applied to it
(we never do). Catch2 and nlohmann_json don't trigger this issue today
because they happen to be warning-clean under our flags, but adding `SYSTEM`
to them too would be defensive and cost nothing.

---

## 2. Namespace aliases: `.cpp` only, never in headers

First attempt at `server.h` used bare `asio::`, `tcp::`, `http::` in
method signatures. The compiler immediately rejected this — those are
namespace aliases that don't exist in the header's scope.

**Rule**: headers use fully-qualified names (`boost::asio::awaitable<void>`,
`boost::asio::ip::tcp::socket`, `boost::beast::http::response<...>`).
Implementation files define short aliases at the top:

```cpp
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
```

This keeps headers self-contained (no implicit dependency on aliases
defined elsewhere) while keeping implementation code readable. If we put
aliases in a header, every file that includes it would silently inherit
them — polluting the namespace and creating brittle coupling to our
naming choices.

---

## 3. `reuse_address` must precede `bind`

The two-argument `tcp::acceptor` constructor:

```cpp
tcp::acceptor acceptor(executor, {address, port});
```

binds to the endpoint immediately during construction. Calling
`acceptor.set_option(reuse_address(true))` after this point is a no-op —
the socket is already bound and the option has no retroactive effect.

This "works" in practice on macOS (the kernel is forgiving about
`TIME_WAIT` rebinding) but is semantically incorrect and would fail on
stricter systems or under rapid start/stop cycles in tests.

**Correct sequence** (open → option → bind → listen):

```cpp
tcp::acceptor acceptor(executor);
acceptor.open(tcp::v4());
acceptor.set_option(tcp::acceptor::reuse_address(true));
acceptor.bind({asio::ip::make_address(address), port});
acceptor.listen(asio::socket_base::max_listen_connections);
```

This is verbose but explicit — each step happens in the only order that
makes it meaningful.

---

## 4. Atomic port for cross-thread test synchronization

The server coroutine (`listener()`) runs on the io_context thread and
discovers the actual bound port after calling `bind()` (important when
`config.port = 0` requests an OS-assigned ephemeral port). Tests call
`server.port()` from a different thread.

A plain `uint16_t` member would be a data race (undefined behavior per
the standard), even though ARM64 naturally-aligned 16-bit stores happen
to be atomic in hardware. The correct solution:

```cpp
std::atomic<std::uint16_t> bound_port_{0};
```

This enables a clean spin-wait pattern in tests:

```cpp
void wait_for_ready(const Server &server) {
  while (server.port() == 0) {
    std::this_thread::sleep_for(1ms);
  }
}
```

No arbitrary sleeps, no brittle timing assumptions. The test blocks until
the server is genuinely listening, then proceeds. The `port = 0` technique
itself eliminates port conflicts between concurrent test processes or
repeated runs.

---

## 5. `as_tuple` — non-throwing async completion

Asio's default `use_awaitable` completion token throws on error (the
`co_await` expression throws `boost::system::system_error`). For a
connection handler that needs to distinguish "client disconnected" from
"malformed frame" and handle both gracefully, exception-based control flow
is awkward — you'd need a try/catch around every await point.

`asio::as_tuple(asio::use_awaitable)` changes the return type from `T` to
`std::tuple<error_code, T>`, giving structured-binding-friendly error
handling:

```cpp
auto [ec, bytes] = co_await http::async_read(
    socket, buffer, req, asio::as_tuple(asio::use_awaitable));
(void)bytes;
if (ec) break;
```

The `(void)bytes;` pattern suppresses `-Wunused-variable` for structured
binding members we don't need. This is the standard idiom when a tuple
member exists only because the API returns it, not because we use it.

**When to use which**:
- `use_awaitable` (throwing): when any failure is truly exceptional and
  should abort the entire coroutine. Good for initialization steps.
- `as_tuple(use_awaitable)` (non-throwing): when failure is expected/normal
  (client disconnect, timeout) and needs per-call handling. Good for
  connection loops.

---

## 6. Keep-alive: a three-line addition, not an architecture

HTTP/1.1 keep-alive requires no architectural change — it's a `while(true)`
loop in the connection handler that breaks on read error or when
`!req.keep_alive()`:

```cpp
while (true) {
  // ... read request ...
  if (read_ec) break;
  // ... write response ...
  response.keep_alive(req.keep_alive());
  if (write_ec || !req.keep_alive()) break;
}
```

The key insight: `response.keep_alive(req.keep_alive())` echoes the
client's `Connection:` header back, telling the client whether we'll
honor keep-alive. `prepare_payload()` then sets `Content-Length` (or
`Transfer-Encoding: chunked`) correctly for the response.

Implementing this from the start means tests can issue multiple requests
per connection without reconnecting, and the server behaves correctly
with real HTTP clients (browsers, curl, load generators) that default to
keep-alive.

---

## 7. IDE integration with FetchContent dependencies

`FetchContent` downloads dependencies into `build/<preset>/_deps/`. The
headers live at paths like:

```
build/dev/_deps/boost-src/libs/asio/include/boost/asio.hpp
```

These paths are baked into `compile_commands.json` as `-isystem` flags.
For an IDE to resolve them, it must:

1. **Find** `compile_commands.json` — either via a symlink at the repo
   root (`ln -sf build/dev/compile_commands.json .`) or via explicit
   config.
2. **Re-index** after first build — the `_deps/` directory doesn't exist
   until `cmake --preset dev` has run at least once.

Editor-specific notes:

- **Neovim + clangd**: respects a `.clangd` file with
  `CompileFlags: { CompilationDatabase: build/dev }`. May also need
  `rm -rf .cache/clangd/index` to clear stale state.
- **Zed**: does not reliably read `.clangd` files. Needs
  `.zed/settings.json`:
  ```json
  {
    "lsp": {
      "clangd": {
        "binary": {
          "arguments": [
            "--compile-commands-dir=build/dev"
          ]
        }
      }
    }
  }
  ```
- **VSCode + clangd extension**: uses `"clangd.arguments"` in
  `.vscode/settings.json` with the same `--compile-commands-dir` flag.

**Common failure mode**: IDE reports "cannot find `<boost/asio.hpp>`"
despite successful builds. Root cause is always stale index or missing
compile-commands path — the headers physically exist, the compiler finds
them (build succeeds), but the language server doesn't know where to look.

---

## Summary of patterns established in Piece C

| Pattern | Rationale |
|---------|-----------|
| `SYSTEM` on all FetchContent deps | Prevent third-party warnings from breaking our `-Werror` build |
| Fully-qualified types in headers, aliases in .cpp | Self-contained headers, readable implementations |
| Four-step acceptor setup (open/option/bind/listen) | Only correct ordering for socket options |
| `atomic<uint16_t>` + spin-wait in tests | Race-free port discovery, no arbitrary sleeps |
| `as_tuple` for connection-loop awaits | Non-throwing error handling where failure is normal |
| `port = 0` for test servers | OS-assigned ports eliminate conflicts |
| Keep-alive from day one | Matches real-world behavior, simplifies tests |
