# libuvcpp

libuvcpp on the libuv event loop, one loop per available core.

## Stack

- **Language:** C++11
- **Library:** libuvcpp 1.6.0 ([repo](https://github.com/Antruly/libuvcpp))
- **Build:** the official `libuvcpp-1.6.0-linux-x64` release package, compiled
  with `g++ -std=c++11 -O2 -DNDEBUG` on `ubuntu:24.04`

The release package carries libuv, OpenSSL, nghttp2, ngtcp2 and zlib inside
`libuvcpp.so`, so the image installs no OpenSSL, no HTTP/2 and no QUIC library.

## Listeners

One process opens four listeners, which is what the port map asks for:

| Port | Protocol | Serves |
|------|----------|--------|
| 8080 | HTTP/1.1 | `baseline`, `limited-conn`, `pipelined`, `async`, `json-comp`, `echo-ws`, `latency-*` |
| 8081 | TLS, ALPN `http/1.1` | `json-tls`, `8gbit`, `static-tls` |
| 8443/tcp | TLS, ALPN `h2` | `baseline-h2`, `static-h2` |
| 8443/udp | QUIC, ALPN `h3` | `baseline-h3` |

The first three are `uvcpp_web_app` instances — the library's documented
high-level server, and where routing, path and query parameters, static files
and WebSocket come from. HTTP/2 is on by default once `enable_ssl` is called, so
8443 uses the framework's own ALPN list; 8081 pins `set_http2_enabled(false)` so
it cannot answer h2.

The QUIC leg is the low-level `uvcpp_http_server`, because `uvcpp_web_app` has
no QUIC API at all. It serves `/baseline2` and nothing else.

## Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/baseline11` | GET | Sums query parameter values |
| `/baseline11` | POST | Sums query parameters + request body |
| `/baseline2` | GET | Same, on 8443 over h2 and h3 |
| `/pipeline` | GET | Returns `ok` (plain text) |
| `/delay/{ms}` | GET | Answers after `ms`, with the parameter echoed verbatim |
| `/json/{count}?m=N` | GET | First `count` dataset items with `total = price * quantity * m` |
| `/echo` | POST | Returns the request body back verbatim |
| `/static/*` | GET | Files from the mounted `/data/static` |
| `/ws` | WS | Echoes text and binary frames back on the same opcode |

## Notes

- **Routing, params, static and WebSocket are the framework's.** Handlers are
  registered with `app.get` / `app.post` / `app.websocket`, path parameters use
  `:name`, and static files go through `serve_static`, which is libuvcpp's
  documented static handler. Nothing in the entry reads or caches static bytes
  itself — the handler revalidates each request against the file's size and
  `mtime` (seconds and nanoseconds), so a replaced file is served immediately.
- **JSON is serialized per request** through `nlohmann::json` (which libuvcpp
  exposes as `uvcpp_json`) and the `total` field is computed inside the loop.
  Nothing is pre-serialized or cached.
- **Compression is the framework's built-in response compression** (gzip), which
  answers `Accept-Encoding` per request and adds no `Content-Encoding` when the
  client sends none. The library has no brotli, so `Accept-Encoding: gzip, br`
  returns gzip.
- **`uvcpp_set_threadpool_size(0)` is called at the top of `main()`**, as
  `doc/webapp-guide.md` instructs applications to do. The static handler
  dispatches its file reads through the libuv thread pool, whose default is 4
  threads regardless of the machine. `0` means "by available CPU count", which
  is libuv's `uv_available_parallelism()` — so a cpuset-limited container (the
  `latency-500k-8cpu` profile pins the container to 8 logical CPUs) sizes both
  the pool and the event loops to the cores it actually has.
- **`set_log_level(log_level::WARN)` keeps per-request access logging off.** An
  access line per request at 4096 concurrent connections is a real cost that has
  nothing to do with serving; warnings and errors — including the framework's
  4xx/5xx access lines — still print.
- **The `/delay/{ms}` reply is a timer on the connection's own loop**, not a
  blocking sleep, so other connections are served while it waits.
- **`/echo` answers `application/octet-stream`**, which is outside the
  framework's compression set, so the bytes come back unchanged.

### The h3 leg

Two things about 8443/udp are worth stating, because they are deliberate:

- **The handler splits the query string by hand.** The low-level
  `uvcpp_http_request` exposes only `url`, with no path or query accessor, so
  `?a=13&b=42` is parsed in the handler. The arithmetic is the same function the
  h2 leg calls, so the two cannot disagree. This is the entry's only hand-rolled
  parsing, and it exists because the high-level app has no QUIC support.
- **`/static` is not served over h3.** libuvcpp has no h3-capable static
  handler, which is why `static-h3` is not subscribed. The framework's web-layer
  static server marks every response deferred, and the h3 dispatch path rejects
  deferred responses with a 500 — there is no "send later" entry point on a QUIC
  stream. Writing one for this entry would be a static handler written for the
  benchmark, which the standard-mode rules exclude.

### Not subscribed

`baseline-h2c` and `json-h2c` (libuvcpp has no cleartext h2), `static-h3` (see
above), `async-db` / `fortunes` (no database), `unary-grpc` (no gRPC), and the
gateway / production-stack compose profiles.
