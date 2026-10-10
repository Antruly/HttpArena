// libuvcpp entry for HttpArena.
//
// Four listeners in one process, one per protocol family the entry subscribes
// to:
//
//   8080/tcp   cleartext HTTP/1.1        baseline, limited-conn, pipelined,
//                                        async, json-comp, echo-ws, ws-pipeline,
//                                        ws-limited, async-db, latency-*
//   8081/tcp   TLS, ALPN http/1.1        json-tls, 8gbit, static-tls
//   8443/tcp   TLS, ALPN h2              baseline-h2, static-h2
//   8443/udp   QUIC, ALPN h3             baseline-h3
//
// The three TCP legs are uvcpp_web_app instances. That is the framework's
// documented high-level server, and it is where routing, query and path
// parameters, static files and WebSocket come from -- the standard-mode rules
// require the documented high-level API when one exists.
//
// The QUIC leg goes through the low-level uvcpp_http_server because
// uvcpp_web_app has no QUIC support at all. It serves the single endpoint
// baseline-h3 needs and nothing else: one synchronous handler, no static
// route, no stream route, no deferred response. Those three are the things the
// h3 path cannot honour (it answers 500 and says so), so the h3 leg is kept
// clear of every one of them.

#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_table.h>
#include <handle/uvcpp_timer.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uvcpp/uvcpp_threadpool.h>
#include <web/uvcpp_http_server.h>
#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_ws.h>

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace uvcpp;

namespace {

const int kPortH1 = 8080;
const int kPortH1Tls = 8081;
const int kPortH2 = 8443;  // TCP, TLS + ALPN h2
const int kPortH3 = 8443;  // UDP, QUIC + ALPN h3

const char* kCertFile = "/certs/server.crt";
const char* kKeyFile = "/certs/server.key";
const char* kDatasetFile = "/data/dataset.json";
const char* kStaticRoot = "/data/static";

/// The 50-item dataset, read once at startup. Read-only afterwards, so every
/// worker loop can share it without a lock.
nlohmann::json g_dataset;

/// The Postgres pool behind `/async-db`. One pool for the process, not one per
/// worker loop: the pool is the thing that owns the connections, and
/// `uvcpp_db_pool` is built to be called from any thread (mutex + condvar
/// inside). A pool per loop would open `loops x max` connections and hit the
/// server's connection ceiling for nothing.
uvcpp_db_pool g_db;
bool g_db_ready = false;

/// What `uvcpp_db_table` cannot know: an unrecognised Postgres OID arrives as
/// its text form, which is exactly right for every column here except `tags` --
/// jsonb comes back as the JSON literal `["fast","new"]` and must be spliced
/// into the response as an array, not as a quoted string. See
/// build_async_db_json().
const char kAsyncDbSql[] =
    "SELECT id, name, category, price, quantity, active, tags, "
    "rating_score, rating_count FROM items "
    "WHERE price BETWEEN $1 AND $2 LIMIT $3";

/// The documented fallback, used when the pool is not up. Byte-for-byte the
/// response the profile asks for when Postgres is unreachable or matches
/// nothing, so there is one shape to produce rather than two.
const char kAsyncDbEmpty[] = "{\"items\":[],\"count\":0}";

bool file_exists(const char* path) { return ::access(path, R_OK) == 0; }

/// How many event loops and how many thread-pool threads to run.
///
/// Both are "the CPUs this process can actually run on", which is what
/// `uvcpp_default_threadpool_size()` reports (it is libuv's
/// `uv_available_parallelism()`, so a cpuset-limited container gets the cores it
/// really has rather than the host's total). `set_loops()` takes 1..64, so the
/// value is clamped for that call.
int worker_loops() {
  unsigned int n = uvcpp_default_threadpool_size();
  if (n < 1u) n = 1u;
  if (n > 64u) n = 64u;
  return static_cast<int>(n);
}

void load_dataset() {
  const char* env = std::getenv("DATASET_PATH");
  const std::string path = env != nullptr ? env : kDatasetFile;
  std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "libuvcpp: cannot open dataset `%s`\n", path.c_str());
    return;
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  try {
    nlohmann::json parsed = nlohmann::json::parse(text);
    if (parsed.is_array()) {
      g_dataset = parsed;
    } else {
      std::fprintf(stderr, "libuvcpp: `%s` is not a JSON array\n", path.c_str());
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "libuvcpp: cannot parse `%s`: %s\n", path.c_str(),
                 e.what());
  }
}

/// Leading decimal digits, 0 when there are none. Same reading of a query
/// value the other entries use: `/baseline11?a=13&b=42` is 13 + 42, and a
/// non-numeric parameter contributes nothing.
long long to_ll(const char* s, size_t n) {
  size_t i = 0;
  bool negative = false;
  while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
  if (i < n && (s[i] == '-' || s[i] == '+')) {
    negative = (s[i] == '-');
    ++i;
  }
  long long value = 0;
  bool any = false;
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    value = value * 10 + (s[i] - '0');
    any = true;
    ++i;
  }
  if (!any) return 0;
  return negative ? -value : value;
}

long long to_ll(const std::string& s) { return to_ll(s.data(), s.size()); }

/// The libuv thread pool size this process needs.
///
/// Two things ride that one pool here: the static handler's file I/O and every
/// `/async-db` query. They want different things from it, so the size is the
/// wider of the two:
///
///   * file I/O wants one thread per core -- not libuv's fixed default of 4,
///     whatever the machine has (`uvcpp_set_threadpool_size(0)` is exactly
///     "by CPU count", and `doc/webapp-guide.md:123` is where the framework
///     documents making this call);
///   * `/async-db` wants one slot per connection Postgres will accept. A libpq
///     call blocks in `recv`, so what caps in-flight queries is slots, not
///     cores; sizing below `DATABASE_MAX_CONN` would leave connections that can
///     never be used, which is the failure the profile's standard rule calls
///     out by name ("size the pool from DATABASE_MAX_CONN").
///
/// libuv spawns pool threads on demand and only up to this number, so the wider
/// figure costs nothing while the pool is idle -- which is every profile except
/// `async-db`. Values above 1024 are clamped by the library (libuv's own
/// ceiling), so there is no clamp here.
int threadpool_threads() {
  int n = static_cast<int>(uvcpp_default_threadpool_size());
  const char* env = std::getenv("DATABASE_MAX_CONN");
  if (env != nullptr) {
    const long conns = to_ll(env, std::strlen(env));
    if (conns > n) n = static_cast<int>(conns);
  }
  return n < 1 ? 1 : n;
}

/// `/baseline11` and `/baseline2`: the sum of every numeric query parameter,
/// plus the body when there is one. Both endpoints answer the same way, so the
/// h2 and h3 legs cannot drift apart.
long long baseline_sum(const uvcpp_web_request& req) {
  long long sum = 0;
  const std::vector<std::pair<std::string, std::string> >& q =
      req.query_params();
  for (size_t i = 0; i < q.size(); ++i) sum += to_ll(q[i].second);
  if (req.method() == http_method::HTTP_POST) sum += to_ll(req.body_str());
  return sum;
}

/// `{"items": [...], "count": N}` for the first `count` dataset items, each
/// carrying `total = price * quantity * m`. Serialized per request -- nothing
/// here is cached or pre-serialized.
std::string build_json(int count, long long m) {
  const int available = static_cast<int>(g_dataset.size());
  int n = count;
  if (n < 0) n = 0;
  if (n > available) n = available;

  nlohmann::json items = nlohmann::json::array();
  for (int i = 0; i < n; ++i) {
    nlohmann::json item = g_dataset[i];
    const long long price = item.value("price", 0LL);
    const long long quantity = item.value("quantity", 0LL);
    item["total"] = price * quantity * m;
    items.push_back(std::move(item));
  }

  nlohmann::json payload;
  payload["items"] = std::move(items);
  payload["count"] = n;
  return payload.dump();
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

void handle_baseline11(uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next) {
  resp.text(std::to_string(baseline_sum(req)));
  resp.end();
}

void handle_baseline2(uvcpp_web_request& req, uvcpp_web_response& resp,
                      uvcpp_web_next) {
  resp.text(std::to_string(baseline_sum(req)));
  resp.end();
}

void handle_pipeline(uvcpp_web_request&, uvcpp_web_response& resp,
                     uvcpp_web_next) {
  resp.text("ok");
  resp.end();
}

void handle_json(uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
  const std::string* count = req.param("count");
  const std::string* multiplier = req.query("m");
  const long long m = multiplier != nullptr ? to_ll(*multiplier) : 1;
  resp.json_str(build_json(count != nullptr ? static_cast<int>(to_ll(*count))
                                            : 0,
                           m));
  resp.end();
}

// ---------------------------------------------------------------------------
// /async-db -- Postgres, through the library's connection pool
// ---------------------------------------------------------------------------

/// The integer a query parameter names, or `fallback` when it is absent.
/// Absent and non-numeric are different things: `to_ll` gives the documented
/// reading of a value that is there but is not a number (0), while a parameter
/// that is not there at all takes the profile's default.
int query_int(const uvcpp_web_request& req, const char* name, int fallback) {
  const std::string* value = req.query(name);
  return value != nullptr ? static_cast<int>(to_ll(*value)) : fallback;
}

/// `tags` is `JSONB`, and the driver hands JSONB over as text: OID 3802 is not
/// one of the mapped types in `src/db/uvcpp_db_pgsql.cpp`, and every unmapped
/// OID degrades to "the string libpq returned" -- which for JSONB is the array
/// literal itself (`["fast", "new"]`). So re-parsing it is the whole
/// conversion, and the result splices in as an array rather than as a quoted
/// string. A value that will not parse becomes `[]` instead of failing the
/// request: the profile has exactly one documented fallback shape and this is
/// not it.
nlohmann::json parse_tags(const std::string& text) {
  if (!text.empty()) {
    try {
      nlohmann::json parsed = nlohmann::json::parse(text);
      if (parsed.is_array()) return parsed;
    } catch (const std::exception&) {
      // falls through to the empty array
    }
  }
  return nlohmann::json::array();
}

/// `{"items": [...], "count": N}` for the rows Postgres returned.
///
/// Column names are the profile's, not an invention: `id`, `name`, `category`,
/// `price`, `quantity`, `active`, `tags`, then `rating_score`/`rating_count`
/// folded into one nested `rating` object. Every number stays an integer --
/// the profile reads `price` and `rating.score` as INTEGER and says so.
/// `count` is the number of rows actually returned, never the requested limit.
std::string build_async_db_json(uvcpp_db_status status,
                                const uvcpp_db_table& table) {
  if (!uvcpp_db_ok(status)) return kAsyncDbEmpty;

  const std::vector<uvcpp_db_row>& rows = table.rows();
  nlohmann::json items = nlohmann::json::array();
  for (size_t i = 0; i < rows.size(); ++i) {
    const uvcpp_db_row& row = rows[i];
    nlohmann::json item;
    item["id"] = row["id"].to_int64();
    item["name"] = row["name"].to_text();
    item["category"] = row["category"].to_text();
    item["price"] = row["price"].to_int64();
    item["quantity"] = row["quantity"].to_int64();
    item["active"] = row["active"].to_bool();
    item["tags"] = parse_tags(row["tags"].to_text());
    nlohmann::json rating;
    rating["score"] = row["rating_score"].to_int64();
    rating["count"] = row["rating_count"].to_int64();
    item["rating"] = std::move(rating);
    items.push_back(std::move(item));
  }

  nlohmann::json payload;
  payload["count"] = static_cast<long long>(rows.size());
  payload["items"] = std::move(items);
  return payload.dump();
}

/// Bring the pool up from the two environment variables the profile provides.
///
/// Failure is not fatal, and deliberately so: `/async-db` has a documented
/// empty response, and the other profiles in this entry have no database at all
/// -- a container started without `DATABASE_URL` (a single-profile run of
/// anything else) must still serve them. `DATABASE_MAX_CONN` is the server's
/// own ceiling, so it is the pool's max; going above it is what makes Postgres
/// start refusing connections under load.
void init_database() {
  const char* url = std::getenv("DATABASE_URL");
  if (url == nullptr || *url == '\0') {
    std::fprintf(stderr,
                 "libuvcpp: DATABASE_URL is not set -- /async-db answers %s\n",
                 kAsyncDbEmpty);
    return;
  }
  const char* env_max = std::getenv("DATABASE_MAX_CONN");
  long max_conn = env_max != nullptr ? to_ll(env_max, std::strlen(env_max))
                                     : 256;
  if (max_conn < 1) max_conn = 1;

  const uvcpp_db_status status =
      g_db.init(url, 1, static_cast<size_t>(max_conn));
  if (!uvcpp_db_ok(status)) {
    std::fprintf(stderr, "libuvcpp: cannot open the Postgres pool: %s\n",
                 g_db.last_error().c_str());
    return;
  }
  g_db_ready = true;
  std::fprintf(stderr, "libuvcpp: Postgres pool up (max=%ld)\n", max_conn);
}

/// `/async-db?min=&max=&limit=`.
///
/// The query goes out through the pool's asynchronous interface, which borrows
/// a connection, runs the statement and returns it -- all on a libuv thread-pool
/// thread -- and then calls back **on this connection's loop thread**. So the
/// handler returns immediately and nothing on the loop is blocked by libpq; the
/// reply is written from the callback. That is the same resume shape
/// `handle_delay` uses, for the same reason: the copy of `next` held by the
/// callback is what tells the framework this link resumes later, and what keeps
/// the response alive until it does.
void handle_async_db(uvcpp_web_app* app, uvcpp_web_request& req,
                     uvcpp_web_response& resp, uvcpp_web_next next) {
  const int lo = query_int(req, "min", 10);
  const int hi = query_int(req, "max", 50);
  int limit = query_int(req, "limit", 50);
  if (limit < 1) limit = 1;
  if (limit > 50) limit = 50;

  if (!g_db_ready) {
    resp.json_str(kAsyncDbEmpty);
    resp.end();
    return;
  }

  // `$1/$2/$3`, not `?`. The driver does not translate placeholders between
  // dialects, on purpose -- `?` is a legal operator in Postgres for jsonb and
  // hstore, so a translation layer could not tell the two apart. This entry
  // names the dialect it is talking to; it is the only Postgres-specific line
  // in the file.
  uvcpp_db_params params;
  params.add(lo).add(hi).add(limit);

  uvcpp_web_response* rp = &resp;
  const int rc = g_db.query(
      app->loop(), kAsyncDbSql, params,
      [rp, next](uvcpp_db_status status, const uvcpp_db_table& table) {
        rp->json_str(build_async_db_json(status, table));
        rp->end();
        next();
      });
  if (rc < 0) {
    // Not dispatched: per the pool's contract the callback will not run, so
    // this is the only reply this request gets. `next` was never copied, so
    // the chain is not left suspended.
    resp.json_str(kAsyncDbEmpty);
    resp.end();
  }
}

void handle_echo(uvcpp_web_request& req, uvcpp_web_response& resp,
                 uvcpp_web_next) {
  // req.body_str() is the decoded body -- chunked framing is already gone, so
  // this echoes byte for byte whether or not the request carried a
  // Content-Length. application/octet-stream is what keeps the framework's
  // compression out of the way of that promise.
  const std::string body = req.body_str();
  resp.binary(body.data(), body.size());
  resp.end();
}

/// `/delay/{ms}`: hand the reply to a timer on this connection's own loop
/// rather than blocking the handler, so every other connection keeps being
/// served while this one waits. The body is the path parameter verbatim.
void handle_delay(uvcpp_web_app* app, uvcpp_web_request& req,
                  uvcpp_web_response& resp, uvcpp_web_next next) {
  const std::string* param = req.param("ms");
  const std::string body = param != nullptr ? *param : std::string("0");
  const long long ms = to_ll(body);
  if (ms <= 0) {
    resp.text("0");
    resp.end();
    return;
  }

  // Capturing `next` by value is what tells the framework this link resumes
  // asynchronously, and that same copy is what keeps the response object
  // alive until the timer fires.
  uvcpp_web_response* rp = &resp;
  uvcpp_timer* timer = new uvcpp_timer(app->loop());
  timer->start(
      [rp, next, timer, body](uvcpp_timer*) {
        rp->text(body);
        rp->end();
        next();
        delete timer;  // deleting the timer from inside its own callback is
                       // the documented pattern for a one-shot (see
                       // uvcpp_timer::callback_start).
      },
      static_cast<uint64_t>(ms), 0);
}

/// `/ws`: echo text and binary back on the same opcode.
void handle_ws(uvcpp_web_ws_request& ws) {
  uvcpp_ws_connection* conn = ws.connection();
  if (conn == nullptr) return;
  conn->on_text([conn](const std::string& msg) {
    conn->send_text(msg.data(), msg.size());
  });
  conn->on_binary([conn](const uint8_t* data, size_t len) {
    conn->send_binary(reinterpret_cast<const char*>(data), len);
  });
}

/// One request over the QUIC leg. The low-level request exposes the raw URL
/// and nothing else -- no path, no query accessor -- so the query string is
/// split here. This is the only hand-rolled parsing in the entry, and it
/// exists because uvcpp_web_app, which does have a query parser, has no QUIC
/// support. The arithmetic is the same function the h2 leg calls.
void handle_baseline2_h3(uvcpp_http_request& req, uvcpp_http_response& resp,
                         uvcpp_tcp_client*) {
  long long sum = 0;
  const std::string& url = req.url;
  const size_t question = url.find('?');
  if (question != std::string::npos) {
    size_t pos = question + 1;
    while (pos < url.size()) {
      size_t amp = url.find('&', pos);
      if (amp == std::string::npos) amp = url.size();
      const size_t eq = url.find('=', pos);
      if (eq != std::string::npos && eq < amp) sum += to_ll(url.data() + eq + 1, amp - eq - 1);
      pos = amp + 1;
    }
  }

  const std::string body = std::to_string(sum);
  resp = uvcpp_http_response::ok(body.c_str(), body.size());
}

}  // namespace

int main() {
  // Before anything else, and before any uvcpp_web_app exists. The static
  // handler dispatches its file I/O through the libuv thread pool, so the pool
  // is how much static-file work can be in flight at once -- and it is also
  // where every `/async-db` query runs. libuv's default is 4 threads whatever
  // the machine has. doc/webapp-guide.md tells applications to set this at the
  // top of main(), and it has to be set here rather than later: once the pool
  // has been submitted to, libuv has already read the size and cached it
  // (uvcpp_set_threadpool_size would answer UV_EALREADY). See
  // threadpool_threads() for why it is not simply "by CPU count".
  uvcpp_set_threadpool_size(threadpool_threads());

  // Console logging off, wholesale. log_level::OFF is a *threshold*, not a
  // level: a filtered call site stops after two relaxed atomic reads, before
  // any stream object exists, so this removes the cost instead of hiding it.
  // Set globally as well as per app because the HTTP/3 listener is a bare
  // uvcpp_http_server (the webapp layer has no QUIC support) and so is not
  // covered by the per-app setting.
  uvcpp_logger::instance().set_level(log_level::OFF);

  load_dataset();
  if (g_dataset.empty()) {
    std::fprintf(stderr, "libuvcpp: dataset is empty -- /json will answer "
                         "with an empty item list\n");
  }

  // The one connection pool for the process, opened before any listener so a
  // bad DATABASE_URL is reported at startup rather than on the first request.
  // A pool per worker loop would be wrong here: the pool owns the connections,
  // and `acquire()` may block, so N loops x per-loop pools would open N times
  // the connections Postgres allows for no added concurrency.
  init_database();

  const int loops = worker_loops();

  // -------------------------------------------------------------------------
  // 8080 -- cleartext HTTP/1.1
  // -------------------------------------------------------------------------
  uvcpp_web_app h1;
  // set_access_log(false) removes the access-log middleware from the chain
  // entirely (uvcpp_web_app.cpp:1900 inserts it only when the flag is on).
  // Left on, it costs a clock read and a response closure per request even with
  // the output level filtered out.
  h1.set_port(kPortH1)
      .set_log_level(log_level::OFF)
      .set_access_log(false);
  h1.set_loops(loops);

  h1.get("/baseline11", handle_baseline11);
  h1.post("/baseline11", handle_baseline11);
  h1.get("/pipeline", handle_pipeline);
  h1.get("/json/:count", handle_json);
  h1.post("/echo", handle_echo);
  h1.get("/delay/:ms", [&h1](uvcpp_web_request& req, uvcpp_web_response& resp,
                             uvcpp_web_next next) {
    handle_delay(&h1, req, resp, next);
  });
  h1.get("/async-db", [&h1](uvcpp_web_request& req, uvcpp_web_response& resp,
                            uvcpp_web_next next) {
    handle_async_db(&h1, req, resp, next);
  });
  h1.websocket("/ws", handle_ws);

  // Every route is registered above, before start(): with set_loops(n > 1)
  // registering afterwards races on the process-shared chain cache.
  const int rc_h1 = h1.start_background();
  if (rc_h1 != 0) {
    std::fprintf(stderr, "libuvcpp: cannot listen on %d (rc=%d)\n", kPortH1,
                 rc_h1);
    return 1;
  }

  // -------------------------------------------------------------------------
  // 8081 -- TLS, ALPN http/1.1
  // -------------------------------------------------------------------------
  uvcpp_web_app h1_tls;
  const bool have_certs = file_exists(kCertFile) && file_exists(kKeyFile);
  if (have_certs) {
    h1_tls.set_port(kPortH1Tls)
        .set_log_level(log_level::OFF)
        .set_access_log(false);
    // Pinned off: static-tls asserts that :8081 negotiates HTTP/1.1 and
    // nothing else.
    h1_tls.set_http2_enabled(false);
    h1_tls.set_loops(loops);
    h1_tls.enable_ssl(kCertFile, kKeyFile);

    h1_tls.get("/json/:count", handle_json);
    h1_tls.post("/echo", handle_echo);
    h1_tls.serve_static("/static", kStaticRoot);

    const int rc = h1_tls.start_background();
    if (rc != 0) {
      std::fprintf(stderr, "libuvcpp: cannot listen on %d (rc=%d)\n",
                   kPortH1Tls, rc);
      return 1;
    }
  } else {
    // The harness mounts /certs only for entries that subscribe to an h2 or
    // h1-tls profile, so a single-profile run legitimately has none. The other
    // listeners must still come up.
    std::fprintf(stderr,
                 "libuvcpp: no %s / %s -- :%d stays closed\n", kCertFile,
                 kKeyFile, kPortH1Tls);
  }

  // -------------------------------------------------------------------------
  // 8443/tcp -- TLS, ALPN h2
  // -------------------------------------------------------------------------
  uvcpp_web_app h2;
  if (have_certs) {
    h2.set_port(kPortH2)
        .set_log_level(log_level::OFF)
        .set_access_log(false);
    h2.set_loops(loops);
    h2.enable_ssl(kCertFile, kKeyFile);  // HTTP/2 is on by default; the ALPN
                                         // list stays the framework's own
                                         // {h2, http/1.1}.

    h2.get("/baseline2", handle_baseline2);
    h2.serve_static("/static", kStaticRoot);

    const int rc = h2.start_background();
    if (rc != 0) {
      std::fprintf(stderr, "libuvcpp: cannot listen on %d (rc=%d)\n", kPortH2,
                   rc);
      return 1;
    }
  }

  // -------------------------------------------------------------------------
  // 8443/udp -- QUIC, ALPN h3
  // -------------------------------------------------------------------------
  uvcpp_ssl_context quic_tls(tls_mode::SERVER, tls_version::TLS_1_3);
  uvcpp_http_server h3;
  if (have_certs) {
    if (!quic_tls.load_certificate_file(kCertFile) ||
        !quic_tls.load_private_key_file(kKeyFile)) {
      std::fprintf(stderr, "libuvcpp: QUIC TLS context rejected the cert pair\n");
      return 1;
    }
    // Order matters: the context has to be in place before listen_quic(), and
    // QUIC has no cleartext mode to fall back on.
    h3.set_quic_ssl_context(&quic_tls);
    h3.get("/baseline2", handle_baseline2_h3);
    const int rc = h3.listen_quic(kPortH3);
    if (rc != 0) {
      std::fprintf(stderr, "libuvcpp: cannot listen on %d/udp (rc=%d)\n",
                   kPortH3, rc);
      return 1;
    }
  }

  // The QUIC endpoint is not multi-loop: uvcpp_quic_server has one loop, and
  // it is the same one run()/stop() manage. So the main thread runs it, and
  // every TCP leg above is already serving on its own threads.
  if (have_certs) {
    h3.run();
  } else {
    h1.join();
  }

  h1.stop();
  if (have_certs) {
    h1_tls.stop();
    h2.stop();
    h3.stop();
  }
  return 0;
}
