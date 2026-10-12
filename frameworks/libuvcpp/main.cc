// libuvcpp entry for HttpArena.
//
// Five listeners in one process, one per protocol family the entry subscribes
// to:
//
//   8080/tcp   cleartext HTTP/1.1 + h2c  baseline, limited-conn, pipelined,
//                                        async, json-comp, echo-ws, ws-pipeline,
//                                        ws-limited, async-db, latency-*
//                                        -- and the backend hop for the
//                                        compose stacks: gateway-64 and
//                                        gateway-h3 forward /baseline2,
//                                        /json/{n} and /async-db here, and
//                                        production-stack forwards /public/*
//                                        and /api/* here
//                                        -- and unary-grpc, whose plaintext leg
//                                        is prior-knowledge h2c on the same port
//                                        as h1 (the two are told apart by the
//                                        24-byte connection preface, not by a
//                                        port number or an Upgrade dance)
//   8081/tcp   TLS, ALPN http/1.1        json-tls, 8gbit, static-tls
//   8082/tcp   cleartext h2c             baseline-h2c, json-h2c
//   8443/tcp   TLS, ALPN h2              baseline-h2, static-h2, unary-grpc-tls
//   8443/udp   QUIC, ALPN h3             baseline-h3, static-h3
//
// The four TCP legs are uvcpp_web_app instances. That is the framework's
// documented high-level server, and it is where routing, query and path
// parameters, static files and WebSocket come from -- the standard-mode rules
// require the documented high-level API when one exists.
//
// The QUIC leg goes through the low-level uvcpp_http_server because
// uvcpp_web_app has no QUIC support at all. It subscribes both h3 profiles:
// /baseline2 as a route, and the static tree through uvcpp_static_server
// mounted as the fallback handler -- uvcpp_http_server has no serve_static()
// of its own (that one belongs to uvcpp_web_app), and uvcpp_static_server
// answers from the thread pool, which means a deferred reply. The h3 dispatch
// path accepts those. The two profiles are the whole of what this leg answers:
// no stream route, no WebSocket, and set_stream_claim/post_stream still answer
// 500 on h3.

#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_table.h>
#include <grpc/uvcpp_grpc.h>
#include <grpc/uvcpp_grpc_server.h>
#include <handle/uvcpp_timer.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uvcpp/uvcpp_threadpool.h>
#include <web/uvcpp_http_server.h>
#include <web/uvcpp_static_server.h>
#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_ws.h>

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace uvcpp;

namespace {

const int kPortH1 = 8080;
const int kPortH1Tls = 8081;
const int kPortH2c = 8082;  // TCP, cleartext h2 (prior-knowledge)
const int kPortH2 = 8443;   // TCP, TLS + ALPN h2
const int kPortH3 = 8443;   // UDP, QUIC + ALPN h3

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

// ---------------------------------------------------------------------------
// production-stack -- cache-aside, in process
// ---------------------------------------------------------------------------
//
// The production-stack profile requires cache-aside on `/api/items/{id}` and
// `/api/me`: read the cache, fall through to Postgres on a miss, populate on
// the way back, and invalidate on a write. *How* the cache is built is the
// entry's choice -- the profile's implementation guide lists "in-process only:
// no Redis for caching, just a local hashmap with TTL" next to the L1+L2 and
// Redis-only options. This entry takes that one, and the reason is in the
// deployment rather than in the benchmark: the profile runs exactly one server
// process, so a shared L2 would add a network hop and a serialization step to
// reads this process is already holding in memory. The compose stack still
// ships the Redis the profile requires, and the server still waits for it to
// report healthy, so a cache that cannot bind fails the stack instead of
// leaving the run measured against something else.
//
// Why the map is process-global rather than one per worker loop: the two calls
// that must see MISS then HIT are two separate connections, and a connection
// is served by whichever loop owns it. A per-loop map would give each of those
// calls its own empty cache, and the second would miss as well.

/// Item entries live for one second and user entries for thirty. Both are the
/// profile's own ceilings (implementation guide, "Cache TTLs"), and the short
/// item TTL is deliberate there: IDs churn so Postgres keeps seeing traffic.
const int kItemTtlMs = 1000;
const int kUserTtlMs = 30000;

/// Sharded so the common path takes a mutex on one bucket rather than on the
/// whole cache -- at the connection counts this profile runs, a single lock
/// over a map that everything reads and writes is a serialization point, not a
/// formality.
const size_t kCacheShards = 64;

/// A hard ceiling on resident entries, so a long run over many distinct IDs
/// cannot grow the map without bound. Expired entries are only reclaimed on
/// lookup, and an item that is read once is never looked up again.
const size_t kCacheMaxEntriesPerShard = 4096;

struct cache_entry {
  std::string body;
  std::chrono::steady_clock::time_point expires;
};

struct cache_shard {
  std::mutex mutex;
  std::unordered_map<std::string, cache_entry> entries;
};

std::array<cache_shard, kCacheShards> g_cache;

cache_shard& cache_shard_of(const std::string& key) {
  return g_cache[std::hash<std::string>()(key) % kCacheShards];
}

/// The cached body for @p key, or false when it is absent or has expired.
bool cache_lookup(const std::string& key, std::string* body) {
  cache_shard& shard = cache_shard_of(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  std::unordered_map<std::string, cache_entry>::iterator it =
      shard.entries.find(key);
  if (it == shard.entries.end()) return false;
  if (it->second.expires <= std::chrono::steady_clock::now()) {
    shard.entries.erase(it);
    return false;
  }
  *body = it->second.body;
  return true;
}

void cache_store(const std::string& key, const std::string& body, int ttl_ms) {
  cache_shard& shard = cache_shard_of(key);
  cache_entry entry;
  entry.body = body;
  entry.expires =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms);
  std::lock_guard<std::mutex> lock(shard.mutex);
  if (shard.entries.size() >= kCacheMaxEntriesPerShard) {
    // Arbitrary rather than oldest-first: maintaining a recency order would
    // cost more than the eviction is worth, and every entry here is already
    // within a second of expiring on its own.
    shard.entries.erase(shard.entries.begin());
  }
  shard.entries[key] = std::move(entry);
}

void cache_erase(const std::string& key) {
  cache_shard& shard = cache_shard_of(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  shard.entries.erase(key);
}

std::string item_cache_key(long long id) {
  return "item:" + std::to_string(id);
}

std::string user_cache_key(long long id) {
  return "user:" + std::to_string(id);
}

/// The two statements behind the production-stack read paths. Both are single
/// row lookups by primary key, so no index work is involved.
const char kItemByIdSql[] =
    "SELECT id, name, category, price, quantity, active, tags, "
    "rating_score, rating_count FROM items WHERE id = $1";
const char kUserByIdSql[] =
    "SELECT id, name, email, plan FROM users WHERE id = $1";

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

// ---------------------------------------------------------------------------
// production-stack -- /api/* (behind the edge's forward_auth)
// ---------------------------------------------------------------------------
//
// The edge terminates TLS, verifies the JWT against the shared authsvc sidecar
// and passes the verified user id down as `X-User-Id`. Nothing here parses a
// token or looks at `Authorization` -- the profile's rules put JWT verification
// at the edge and forbid the framework from doing it. What the server does do
// is refuse a request that arrives without `X-User-Id`, so a misconfigured edge
// that forwards `/api/*` straight through cannot silently serve private rows.

/// A request that reached the framework without the edge's verified user id.
/// 401 rather than a redirect: the profile's auth wall is checked as a status.
void send_unauthorized(uvcpp_web_response& resp) {
  resp.status(401).json_str("{\"error\":\"unauthorized\"}");
  resp.end();
}

/// One `items` row as the profile's JSON, with `total` folded in. Same column
/// names and the same nested `rating` object `/async-db` emits, so the two
/// endpoints cannot drift apart -- and `total` is price x quantity, which is
/// what the JSON profile checks its rows against.
nlohmann::json item_row_json(const uvcpp_db_row& row) {
  nlohmann::json item;
  item["id"] = row["id"].to_int64();
  item["name"] = row["name"].to_text();
  item["category"] = row["category"].to_text();
  item["price"] = row["price"].to_int64();
  item["quantity"] = row["quantity"].to_int64();
  item["active"] = row["active"].to_bool();
  item["tags"] = parse_tags(row["tags"].to_text());
  item["total"] = row["price"].to_int64() * row["quantity"].to_int64();
  nlohmann::json rating;
  rating["score"] = row["rating_score"].to_int64();
  rating["count"] = row["rating_count"].to_int64();
  item["rating"] = std::move(rating);
  return item;
}

/// The body cached for `/api/items/{id}`, or the empty string when the row is
/// not there. Only a real row is ever cached: caching the miss would answer a
/// later read with a body the database no longer agrees with, and the profile's
/// anti-cheat reads a row it just wrote.
std::string item_body(uvcpp_db_status status, const uvcpp_db_table& table) {
  if (!uvcpp_db_ok(status) || table.rows().empty()) return std::string();
  return item_row_json(table.rows()[0]).dump();
}

/// The body cached for `/api/me`.
std::string user_body(uvcpp_db_status status, const uvcpp_db_table& table) {
  if (!uvcpp_db_ok(status) || table.rows().empty()) return std::string();
  const uvcpp_db_row& row = table.rows()[0];
  nlohmann::json user;
  user["id"] = row["id"].to_int64();
  user["name"] = row["name"].to_text();
  user["email"] = row["email"].to_text();
  user["plan"] = row["plan"].to_text();
  return user.dump();
}

/// `X-Cache: HIT` / `MISS` on every `/api/items/{id}` read, including the ones
/// that fall through to Postgres. The profile's cache-aside check reads exactly
/// this header, twice in a row, and expects the pair to differ.
void send_cached(uvcpp_web_response& resp, const std::string& body,
                 bool hit) {
  resp.set_header("X-Cache", hit ? "HIT" : "MISS");
  resp.json_str(body);
  resp.end();
}

/// `GET /api/items/{id}` -- cache-aside, L1 only, one-second TTL.
void handle_api_item_get(uvcpp_web_app* app, uvcpp_web_request& req,
                         uvcpp_web_response& resp, uvcpp_web_next next) {
  if (req.header("X-User-Id").empty()) {
    send_unauthorized(resp);
    return;
  }
  const std::string* param = req.param("id");
  const long long id = param != nullptr ? to_ll(*param) : 0;
  const std::string key = item_cache_key(id);

  std::string body;
  if (cache_lookup(key, &body)) {
    send_cached(resp, body, true);
    return;
  }
  if (!g_db_ready) {
    send_cached(resp, "{}", false);
    return;
  }

  uvcpp_db_params params;
  params.add(id);
  uvcpp_web_response* rp = &resp;
  const int rc = g_db.query(
      app->loop(), kItemByIdSql, params,
      [rp, next, key](uvcpp_db_status status, const uvcpp_db_table& table) {
        const std::string json = item_body(status, table);
        if (!json.empty()) cache_store(key, json, kItemTtlMs);
        send_cached(*rp, json.empty() ? std::string("{}") : json, false);
        next();
      });
  if (rc < 0) {
    // Not dispatched, so no callback is coming. `next` was never copied and
    // the chain is not left suspended.
    send_cached(resp, "{}", false);
  }
}

/// `POST /api/items/{id}` -- update the row, then drop it from the cache.
///
/// The invalidation is unconditional and happens before the reply: the
/// profile's check warms an id, writes it, and expects the next read to be a
/// MISS. Dropping the key only when the UPDATE reports a row would leave the
/// stale copy in place for an id that does not exist yet -- the profile's write
/// set is IDs the reads also touch, but nothing promises the row is there.
void handle_api_item_post(uvcpp_web_app* app, uvcpp_web_request& req,
                          uvcpp_web_response& resp, uvcpp_web_next next) {
  if (req.header("X-User-Id").empty()) {
    send_unauthorized(resp);
    return;
  }
  const std::string* param = req.param("id");
  const long long id = param != nullptr ? to_ll(*param) : 0;

  // The body is the profile's `{"name":..,"price":..,"quantity":..}`. A body
  // that will not parse is not an error worth inventing a status for: the
  // write simply goes through with the values that are there, defaulting to
  // what the row already had is not possible without a read, so name keeps the
  // empty string and the numbers keep zero.
  std::string name;
  long long price = 0;
  long long quantity = 0;
  try {
    const nlohmann::json body = nlohmann::json::parse(req.body_str());
    name = body.value("name", std::string());
    price = body.value("price", 0LL);
    quantity = body.value("quantity", 0LL);
  } catch (const std::exception&) {
    // defaults above
  }

  if (!g_db_ready) {
    resp.status(204);
    resp.end();
    return;
  }

  uvcpp_db_params params;
  params.add(name).add(price).add(quantity).add(id);
  uvcpp_web_response* rp = &resp;
  const std::string key = item_cache_key(id);
  const int rc = g_db.query(
      app->loop(),
      "UPDATE items SET name = $1, price = $2, quantity = $3 WHERE id = $4",
      params,
      [rp, next, key](uvcpp_db_status, const uvcpp_db_table&) {
        cache_erase(key);
        rp->status(204);
        rp->end();
        next();
      });
  if (rc < 0) {
    cache_erase(key);
    resp.status(204);
    resp.end();
  }
}

/// `GET /api/me` -- the user the edge authenticated, cache-aside at 30s.
void handle_api_me(uvcpp_web_app* app, uvcpp_web_request& req,
                   uvcpp_web_response& resp, uvcpp_web_next next) {
  const std::string uid = req.header("X-User-Id");
  if (uid.empty()) {
    send_unauthorized(resp);
    return;
  }
  const long long id = to_ll(uid);
  const std::string key = user_cache_key(id);

  std::string body;
  if (cache_lookup(key, &body)) {
    resp.set_header("X-Cache", "HIT");
    resp.json_str(body);
    resp.end();
    return;
  }
  if (!g_db_ready) {
    resp.status(503).json_str("{}");
    resp.end();
    return;
  }

  uvcpp_db_params params;
  params.add(id);
  uvcpp_web_response* rp = &resp;
  const int rc = g_db.query(
      app->loop(), kUserByIdSql, params,
      [rp, next, key](uvcpp_db_status status, const uvcpp_db_table& table) {
        const std::string json = user_body(status, table);
        if (!json.empty()) cache_store(key, json, kUserTtlMs);
        rp->json_str(json.empty() ? std::string("{}") : json);
        rp->end();
        next();
      });
  if (rc < 0) {
    // Unlike `/api/items`, there is no cache header to get right here and no
    // documented empty shape -- an unavailable database is a 503, not a body
    // that claims a user.
    resp.status(503).json_str("{}");
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

// ---------------------------------------------------------------------------
// unary-grpc -- benchmark.BenchmarkService/GetSum
// ---------------------------------------------------------------------------
//
// The wire shape is the framework's, not the entry's: grpc_parse_unary_body()
// unwraps the 5-byte length-prefixed frame and grpc_field_get_int32() walks the
// protobuf message, while grpc_web_send_unary() / grpc_web_send_status() put
// the response's three sections (HEADERS, DATA, trailers) in order -- the entry
// only says which fields to read and what to write back. Both gRPC profiles are
// "two random integers in, their sum out", so the handler is a handful of lines
// with no framing, no varint and no trailer construction in it.
//
// That is the standard-mode argument in one place. The gRPC module is the
// library's documented high-level API for this, and hand-rolling the encoder
// here would be exactly the "build a faster path around the framework" the
// rules forbid.
//
// SumRequest is `{int32 a = 1; int32 b = 2;}`, SumReply is `{int32 result = 1;}`.
// proto3 omits a field whose value is 0 and grpc_field_get_int32() answers
// UV_ENOENT for an absent one -- which is not an error, it is the default, and
// the out-param already holds it. The two addresses are never 0 in this profile,
// but reading the default that way is what makes the handler right for the wire
// format rather than right only for the benchmark.
//
// A malformed body does not go through the normal path at all: the parser hands
// back the status code it decided on (INVALID_ARGUMENT for a broken frame,
// RESOURCE_EXHAUSTED for an oversized one) plus the sentence for `grpc-message`,
// so the handler never repeats that table. The reply is Trailers-Only -- HTTP
// 200, no body, the conclusion in the trailing block -- which is why the entry
// does not set a status code here.
void handle_grpc_sum(uvcpp_web_request& req, uvcpp_web_response& resp,
                     uvcpp_web_next) {
  std::string message;
  std::string err;
  grpc_status fail_status = grpc_status::INTERNAL;
  if (grpc_parse_unary_body(req.body_str(), message, fail_status, err) != 0) {
    grpc_web_send_status(resp, fail_status, err);
    return;
  }

  int32_t a = 0;
  int32_t b = 0;
  grpc_field_get_int32(message.data(), message.size(), 1, a);
  grpc_field_get_int32(message.data(), message.size(), 2, b);

  std::string reply;
  grpc_field_put_int32(reply, 1, a + b);
  grpc_web_send_unary(resp, reply);
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
  // 8080 -- cleartext HTTP/1.1 + h2c
  // -------------------------------------------------------------------------
  uvcpp_web_app h1;
  // set_access_log(false) removes the access-log middleware from the chain
  // entirely (uvcpp_web_app.cpp:1900 inserts it only when the flag is on).
  // Left on, it costs a clock read and a response closure per request even with
  // the output level filtered out.
  h1.set_port(kPortH1)
      .set_log_level(log_level::OFF)
      .set_access_log(false);
  // Both protocols on one port, separated by the first 24 bytes of each
  // connection: the RFC 9113 preface is byte-exact, so this is a decision and
  // not a guess, and it settles once per connection -- a connection that starts
  // with anything else is HTTP/1.1 for the rest of its life and pays nothing
  // further (uvcpp_http_server.h:377). What it buys is unary-grpc's plaintext
  // leg, whose URL names this very port (`http://localhost:8080`), without
  // opening a second one for the h1 profiles to trip over.
  h1.set_h2c_enabled(true);
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

  // ---------------------------------------------------------------------
  // The routes the compose-orchestrated profiles reach over their own hop
  // ---------------------------------------------------------------------
  //
  // gateway-64 / gateway-h3 put a stock Caddy in front and let it serve
  // /static/* off disk; everything else it forwards over plain HTTP/1.1 to
  // 127.0.0.1:8080, which is this listener. So the three dynamic endpoints the
  // gateway URI mix names -- /baseline2, /json/{n}, /async-db -- all have to
  // answer on h1, not only on the 8443 listener the isolated profiles use.
  h1.get("/baseline2", handle_baseline2);

  // ---------------------------------------------------------------------
  // production-stack -- /public/* unauthenticated, /api/* behind the edge
  // ---------------------------------------------------------------------
  //
  // /public/baseline is the same query-sum /baseline2 answers, and
  // /public/json/{n} is the same serializer /json/{n} uses; the profile simply
  // spells the two paths differently because they sit on the anonymous side of
  // the edge's split.
  h1.get("/public/baseline", handle_baseline2);
  h1.get("/public/json/:count", handle_json);
  h1.get("/api/items/:id", [&h1](uvcpp_web_request& req,
                                 uvcpp_web_response& resp,
                                 uvcpp_web_next next) {
    handle_api_item_get(&h1, req, resp, next);
  });
  h1.post("/api/items/:id", [&h1](uvcpp_web_request& req,
                                  uvcpp_web_response& resp,
                                  uvcpp_web_next next) {
    handle_api_item_post(&h1, req, resp, next);
  });
  h1.get("/api/me", [&h1](uvcpp_web_request& req, uvcpp_web_response& resp,
                          uvcpp_web_next next) {
    handle_api_me(&h1, req, resp, next);
  });

  // ---------------------------------------------------------------------
  // unary-grpc -- h2c on this port
  // ---------------------------------------------------------------------
  //
  // Registered on h1 as well as on 8443 because the two gRPC profiles name the
  // same service on two transports: unary-grpc over h2c here, unary-grpc-tls
  // over TLS on 8443. A route is one row in the router regardless of which
  // connection it arrives on, so the same line serves both -- what differs is
  // only which listener the client reached.
  h1.post("/benchmark.BenchmarkService/GetSum", handle_grpc_sum);

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
  // 8082 -- cleartext h2c (prior-knowledge)
  // -------------------------------------------------------------------------
  //
  // The platform's dedicated h2c port (scripts/validate.sh sets H2C_PORT=8082),
  // so the h2c profiles never share a port with h1 and never need the preface
  // split above. Both routes already exist on other listeners -- /baseline2 and
  // /json/{n} are the same handlers -- and h2load is explicit about the
  // protocol (`-p h2c`), so a server that quietly answered HTTP/1.1 here would
  // be caught rather than scored.
  //
  // No certificate gate: this leg is cleartext, so it comes up whether or not
  // /certs is mounted.
  uvcpp_web_app h2c;
  h2c.set_port(kPortH2c)
      .set_log_level(log_level::OFF)
      .set_access_log(false);
  h2c.set_h2c_enabled(true);
  h2c.set_loops(loops);

  h2c.get("/baseline2", handle_baseline2);
  h2c.get("/json/:count", handle_json);

  {
    const int rc = h2c.start_background();
    if (rc != 0) {
      std::fprintf(stderr, "libuvcpp: cannot listen on %d (rc=%d)\n", kPortH2c,
                   rc);
      return 1;
    }
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
    // unary-grpc-tls: the same service the h2c leg answers, reached over TLS.
    h2.post("/benchmark.BenchmarkService/GetSum", handle_grpc_sum);

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
  // Declared before h3 on purpose: the handler it hands out captures `this`,
  // so it has to outlive the server (the destructor order of locals is the
  // reverse of this line's).
  uvcpp_static_server h3_static(kStaticRoot, "/static");
  // Off, to match the h1/h2 legs: uvcpp_web_static_options defaults
  // spa_fallback to false, while uvcpp_static_server defaults it to true. Left
  // on, a missing file would come back as index.html with a 200 and the three
  // static profiles would disagree about what a miss is.
  h3_static.set_spa_fallback(false);
  uvcpp_http_server h3;
  if (have_certs) {
    if (!quic_tls.load_certificate_file(kCertFile) ||
        !quic_tls.load_private_key_file(kKeyFile)) {
      std::fprintf(stderr, "libuvcpp: QUIC TLS context rejected the cert pair\n");
      return 1;
    }
    // Order matters: the context has to be in place before listen_quic(), and
    // QUIC has no cleartext mode to fall back on; set_loops() has to come
    // before both (it answers UV_EBUSY once the endpoint exists).
    h3.set_quic_ssl_context(&quic_tls);
    h3.get("/baseline2", handle_baseline2_h3);
    // static-h3: the same tree the h1/h2 legs serve. on_request() is the
    // no-route-match handler, so /baseline2 above still wins and everything
    // else is looked up on disk -- which is exactly what the profile asks for
    // (/static/*, and a 404 rather than the fallback page for a miss).
    h3.on_request(h3_static.handler(&h3));
    h3.set_loops(loops);
    const int rc = h3.listen_quic(kPortH3);
    if (rc != 0) {
      std::fprintf(stderr, "libuvcpp: cannot listen on %d/udp (rc=%d)\n",
                   kPortH3, rc);
      return 1;
    }
  }

  // run() drives the acceptor's loop on this thread. With set_loops(n > 1) the
  // QUIC endpoints fan out to n-1 worker loops, each on its own thread and each
  // bound to the same port with SO_REUSEPORT, so the kernel hashes connections
  // across them; they take the same `loops` value the TCP legs above took, and
  // every TCP leg is already serving on its own threads by this point.
  if (have_certs) {
    h3.run();
  } else {
    h1.join();
  }

  h1.stop();
  h2c.stop();
  if (have_certs) {
    h1_tls.stop();
    h2.stop();
    h3.stop();
  }
  return 0;
}
