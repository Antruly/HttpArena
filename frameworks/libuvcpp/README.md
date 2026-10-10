# libuvcpp

libuvcpp on the libuv event loop, one loop per available core.

## Stack

- **Language:** C++11
- **Library:** libuvcpp 1.6.x ([repo](https://github.com/Antruly/libuvcpp)),
  built from source in the image
- **Build:** `cmake -DCMAKE_BUILD_TYPE=Release` on `ubuntu:24.04`, with libuv,
  llhttp, zlib, nghttp2, ngtcp2, nghttp3 and OpenSSL 3.5 all built statically
  into `libuvcpp.so`; the entry is `g++ -std=c++11 -O2 -DNDEBUG`

The library is built from source rather than installed from the published
`libuvcpp-1.6.x-linux-x64` release package, because that package is built with
both database backends off — the release workflow asserts it with an `ldd` check
that fails if `libpq.so` appears — and `/async-db` needs the Postgres backend.
Everything else about the build follows that same release recipe. OpenSSL is
built from source as well: its QUIC API (`SSL_set_quic_tls_cbs`) is what the
HTTP/3 listener needs, and Ubuntu 24.04's own OpenSSL is too old to have it.

## Listeners

One process opens four listeners, which is what the port map asks for:

| Port | Protocol | Serves |
|------|----------|--------|
| 8080 | HTTP/1.1 | `baseline`, `limited-conn`, `pipelined`, `async`, `async-db`, `json-comp`, `echo-ws`, `echo-ws-pipeline`, `echo-ws-limited`, `latency-*` |
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
| `/async-db` | GET | Postgres range query, `min`/`max`/`limit` |

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
- **The libuv thread pool is sized at the top of `main()`.** Two things ride
  that one pool: the static handler's file I/O, and every `/async-db` query. The
  size is the wider of "one thread per available core" (libuv's default is 4
  regardless of the machine; `latency-500k-8cpu` pins the container to 8 CPUS,
  and libuv's `uv_available_parallelism()` is cpuset-aware) and
  `DATABASE_MAX_CONN`, because a libpq call blocks in `recv` — what caps
  in-flight queries is slots, not cores. libuv spawns pool threads on demand, so
  the wider figure costs nothing while the pool is idle.
- **The `/delay/{ms}` reply is a timer on the connection's own loop**, not a
  blocking sleep, so other connections are served while it waits.
- **`/echo` answers `application/octet-stream`**, which is outside the
  framework's compression set, so the bytes come back unchanged.

### `/async-db`

- **The pool is the library's `uvcpp_db_pool`**, one per process, opened at
  startup from `DATABASE_URL` and sized from `DATABASE_MAX_CONN` (1..max, max
  from the variable, defaulting to 256). Not one pool per worker loop: the pool
  is the thing that owns the connections, and it is written to be called from
  any thread.
- **The query is asynchronous**, through `pool.query(loop, sql, params, cb)`.
  That call borrows a connection, runs the statement and returns the connection
  — all on a libuv thread-pool thread — and then invokes the callback on the
  connection's own loop thread. The handler returns before libpq is touched; the
  reply is written from the callback, which holds a copy of `next` to keep the
  response alive. Nothing on the event loop ever blocks on the database.
- **`$1/$2/$3`, not `?`.** libuvcpp's driver does not translate placeholders
  between dialects, on purpose — `?` is a legal operator in Postgres for jsonb
  and hstore, so a translation layer could not tell the two apart. This is the
  entry's only Postgres-specific line.
- **`tags` is re-parsed.** Postgres returns JSONB as text (OID 3802 is not one
  of the driver's mapped types, and an unmapped OID degrades to the string libpq
  returned), and that string *is* the JSON array — so it is parsed back into an
  array rather than spliced in as a quoted string.
- **Failure is not fatal and not silent-in-the-wrong-way.** With no
  `DATABASE_URL`, or a pool that will not open, `/async-db` answers the
  documented `{"items":[],"count":0}` and the other profiles serve normally.
- **At the profile's own generator shape the pool saturates without failing.**
  `scripts/lib/tools/gcannon.sh` drives this endpoint with `-r 25`, so each
  socket serves 25 requests and is then retired and replaced; 1024 is how many
  are live at once, not how many exist. Measured in that shape, against a
  Postgres seeded with the profile's own 100,000 rows, with
  `DATABASE_MAX_CONN=256` and the server's `max_connections` the same figure:
  84,807 requests over 10 s across 3,072 socket retirements, **zero** empty
  bodies and zero with the wrong `count`. That combination is the tight one,
  because the pool's ceiling and the server's are the same number — the pool
  filled it and later requests waited for a free connection instead of being
  rejected. A pool sized *above* `DATABASE_MAX_CONN` would answer `count: 0`
  under exactly this load, which is the failure the profile's standard rule
  names.

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
above), `fortunes` (libuvcpp has no HTML template engine), `unary-grpc` (no
gRPC), and the gateway / production-stack compose profiles.
