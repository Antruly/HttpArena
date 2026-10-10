// libuvcpp entry for HttpArena.
//
// Four listeners in one process, one per protocol family the entry subscribes
// to:
//
//   8080/tcp   cleartext HTTP/1.1        baseline, limited-conn, pipelined,
//                                        async, json-comp, echo-ws
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
  // is how much static-file work can be in flight at once; libuv's default is 4
  // threads whatever the machine has. doc/webapp-guide.md tells applications to
  // set this at the top of main(), and it has to be set here rather than later:
  // once the pool has been submitted to, libuv has already read the size and
  // cached it (uvcpp_set_threadpool_size would answer UV_EALREADY). Passing 0
  // means "by CPU count", which is what the guide recommends.
  uvcpp_set_threadpool_size(0);

  load_dataset();
  if (g_dataset.empty()) {
    std::fprintf(stderr, "libuvcpp: dataset is empty -- /json will answer "
                         "with an empty item list\n");
  }

  const int loops = worker_loops();

  // -------------------------------------------------------------------------
  // 8080 -- cleartext HTTP/1.1
  // -------------------------------------------------------------------------
  uvcpp_web_app h1;
  h1.set_port(kPortH1)
      .set_log_level(log_level::WARN);  // access logging off: a line per request
                                        // costs more than the request does
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
    h1_tls.set_port(kPortH1Tls).set_log_level(log_level::WARN);
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
    h2.set_port(kPortH2).set_log_level(log_level::WARN);
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
