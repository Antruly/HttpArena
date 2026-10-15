# libuvcpp entry for HttpArena.
#
# The library is built from source here, and it has to be: the published
# `libuvcpp-<version>-linux-x64` release package is built with
# `-DUVCPP_ENABLE_DB_MYSQL=OFF -DUVCPP_ENABLE_DB_PGSQL=OFF`, and the release
# workflow asserts it stays that way with an `ldd` check that fails the build if
# `libpq.so` or `libmysqlclient.so` shows up. That is deliberate -- a release
# package that quietly acquires a database client dependency is a release
# package that stops running on machines that do not have one. So the async-db
# profile cannot be served by the release artifact, and the entry builds the
# library instead, which is also what the other merged C++ entries (cutelyst,
# hical) do.
#
# Everything except libpq is built statically into libuvcpp.so, following
# `.github/workflows/release.yml`'s linux-x64 job, which is the recipe behind
# the artifact users install. The one flag that differs is the Postgres
# backend. The other two backends are left off because this entry does not use
# them: SQLite would otherwise be an extra source download at build time and an
# extra shared object at run time for nothing.
#
# **OpenSSL is built from source.** Not a preference: `SSL_set_quic_tls_cbs`
# is the QUIC API the HTTP/3 leg needs, it landed in OpenSSL 3.5, and Ubuntu
# 24.04's libssl-dev is 3.0.13. Pointing the build at the distro's OpenSSL makes
# CMake's configure-time precheck turn UVCPP_ENABLE_QUIC off with a warning --
# the cache still reads ON, the library just ships without QUIC, and the h3 leg
# dies at run time. So the build is pinned to 3.5.0, sources, static, with
# `-fPIC` (which `no-shared` does not imply, and without which the static
# archive cannot go into a shared object). libpq is linked against the distro's
# OpenSSL, which is a separate copy in a separate object -- see the note on the
# runtime stage.

FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

# The library source, pinned by commit rather than by tag: a tag can be moved
# and the bytes under it re-uploaded, so a SHA is the only thing that fixes
# which revision gets built. `3fdd6208` was `v1.6.1` -- what the first version
# of this entry built, and what the platform measured as the board's 58 req/s on
# `baseline-h3-64`. The pin has since moved four times: to `2d5fe74`
# (`1.6.20-dev`), then to `6eede33` (`1.6.51-dev`), which was the tip of the
# library's `master` when it was taken and is the revision the platform's fourth
# run measured, then to `8150720` (`1.7.2-dev`), which is the `v1.7.0` tag plus
# one documentation commit and two that change code, and now to `02ae418`
# (`1.7.2-dev`), which is four code commits past it.
# Each move was measured against the previous pin before being taken; the
# criteria for the `6eede33` batch are further down this file, and each commit's
# own criterion is in the library's `CHANGELOG.md`.
#
# The `02ae418` move is for the next run, and like the `8150720` move it is a
# batch whose members carry local deterministic counters rather than on-board
# measurements. The four code commits between the two pins -- the other two are
# documentation -- and what each one's criterion is:
#
#   * `e716010` stops the database path from building a log line when no log
#     callback is installed: `106.16 / 106.15 -> 102.18 / 102.22` allocations
#     per response (A B B A), with `utime + stime` unmoved -- 3.96 allocations
#     is about 0.12 us, under the rig's resolution, so the allocation count is
#     the criterion. `async-db` is Reference-only and contributes nothing to
#     the score; this one is a counter and nothing more.
#   * `3cbfc9a` and `370d6ef` are the QUIC send path. The first lets one
#     datagram carry STREAM frames from several streams (`sendmsg` per request
#     `1.0316 / 1.0360 -> 0.0428 / 0.0561`), the second sizes each send chunk
#     from what that write needs instead of reserving 64 KiB flat (`72 374.8 ->
#     7 507.5` bytes allocated per request, `-89.6%`, peak RSS 91 -> 50 MiB on
#     the local h3 rig, where the flat reserve is the same shape as the 238 MiB
#     the board reports for `baseline-h3-64`). Both are measured on the h3
#     profiles this entry subscribes to, on this machine's rig rather than the
#     board's, and are not offered as predictions.
#   * `02ae418` lets `resp.json()` hand the dumped body to the response instead
#     of copying it: `-1.0000` `memcpy` per request and `-8 397` bytes copied
#     per request -- the body's own length, so it is that copy and not some
#     other one -- with `-0.83%` instructions per request on `/json/50?m=6`.
#     The instruction figure is 0.83% of one handler, smaller than the spread
#     between two board readings; the counters are the criterion.
#
# What this run also carries, and what is *not* in the pin, is the entry side:
# the JSON profiles answer through the framework's standard serializer again
# (see the `/json` note below), and the brotli encoder is built on. The first of
# those is the larger effect, and the fifth run measured its size for us by
# accident: that run's tree had the entry on `uvcpp_json_writer` (9257623), the
# revision the maintainer then objected to, and it reported `async-db-1024` at
# `271.85` us of CPU per response against the fourth run's `407.2` -- a fall that
# the doc attributes to that one entry commit and to nothing in the library.
# This entry has the DOM path back, so its `async-db` row starts from the higher
# number, and its `json-*` rows are likewise the ones to compare against the
# fourth run's rather than the fifth's. Brotli works the other way, on a profile
# whose score is quadratic in bytes per response (see the note above
# `ARG LIBUVCPP_REF`).
#
# The `8150720` move, like the `02ae418` one above and for the same reason, is a
# batch whose criterion is *not* an on-board measurement. Its two commits carry
# a local deterministic counter as their criterion -- `ebeaa20` is `-1.90%`
# user-mode instructions per request on the h1 shape (22,459.6 -> 22,032.4 Ir,
# callgrind, the path normalisation collapsing to one function with a fast path
# for the already-normal form), and `8150720` is `204.78 -> 102.77` `malloc`s per
# response on the async-db shape (the result-set vectors now reserve, which
# `PQntuples`/`PQnfields` let them do exactly). Neither is offered as a predicted
# board movement. The h1 profiles
# are all `pipeline = 1`, so `-1.90%` of user-mode instructions is about `0.35%`
# of the end-to-end time on those profiles, which is smaller than the spread
# between two platform readings of the same profile; and `async-db` is
# Reference-only, contributing nothing to the board's score and showing up only
# in its own `cpu_per_req_us`, which is user plus system time where the change
# is on the user side. So both are accounted for as "the counter certainly went
# down", not as a score. The fifth run has since reported on them, and neither is
# separable in it: that round's rps carried an upward bias of up to 3.2% even on
# the profiles pinned by external factors, where the paragraph above puts
# `ebeaa20`'s `-1.90%` of user-mode instructions at about 0.35% of end-to-end
# time; and `async-db` is one row per round, where that same round's entry-side
# change moved the number by 135 us. The library-side
# criteria and the guards (byte-identical response bodies, the postgres suite
# byte-identical across arms, 127/127 ctest) are in its `CHANGELOG.md`.
#
# `6eede33` is also what the library's `v1.7.0` release carries, in the part
# that decides behaviour: `git diff 6eede33 v1.7.0 -- src/ ':!src/db'` is one
# hunk, the version macros in `src/uvcpp/uvcpp_version.h` -- and nothing else.
# The `src/db/` part of that range is the MySQL/PG backends moving behind a
# `dlopen` seam that is off by default, plus the status enum the seam added; a
# from-source build that does not ask for it is unaffected. Everything else in
# the range is packaging, build system and CI, and only one piece of that
# reaches the ELF build: an `--exclude-libs,ALL` link option that hides symbols
# arriving from static archives and leaves the library's own API exported -- it
# changes the dynamic symbol table, not the code. So the fourth run's numbers
# are `v1.7.0`'s numbers, and the distance from the tag to this pin is small and
# fully accounted for: `git diff --stat v1.7.0 8150720 -- src/ ':!src/db'` is
# the version macros plus four files under `src/webapp/` -- all of them
# `ebeaa20`, the path normalisation -- and `git diff v1.7.0 8150720 -- src/db/`
# is `8150720`, the result-set reservation. The third commit in the range,
# `d9581d9`, is the download table in `README.md`: documentation only. The pin
# stays on the commit rather than moving to the tag because the two behavioural
# commits have no tag -- they are unreleased, and they are the whole reason this
# revision is here.
#
# The tip is a revision whose CI is green on all four platforms -- Linux,
# macOS, MSVC and MinGW-w64 -- which is worth commits that do not otherwise
# matter to this entry. Two of the commits below it fix defects this entry can
# reach:
#
#   * `reply_alive()` no longer dereferences the connection pointer it is handed.
#     It exists to be called *after* a connection may be gone -- it is what the
#     deferred static handler asks before answering from the thread pool, which is
#     the path every `static-*` cache miss takes here -- and it used to read
#     `client->loop_index()` to pick one of the per-loop registration tables.
#     With one loop a fast path skips that read; with `set_loops(n > 1)`, which
#     this entry calls, it is a read of freed memory. ASan caught it on Linux
#     five runs out of five; on Windows the same code is a SEGFAULT, because
#     Windows hands the pages back to the OS.
#   * 413 responses read two bytes past the end of their own status text
#     (`"413 Payload Too Large"` measured as 23 instead of 21). Pre-existing and
#     ASan-only, but a real out-of-bounds read on that branch.
#
# What this pin carries that these profiles depend on, by family:
#
#   static -- serving the on-disk `.br` / `.gz` twin of a file instead of
#     deflating it per request (`precompressed`, on for every static leg this
#     entry mounts: h1-tls, h2 and h3). `uvcpp_static_server` -- which is what
#     the h3 leg uses, there being no `uvcpp_web_app` listener on QUIC -- did not
#     even have a compressed-variant cache, so it ran zlib over the whole file on
#     the loop thread once per request: 933 -> 82,451 rps on the entry's own
#     `data/static` (8 loops, `h2load -p http/1.1 -c 64 -m 2 -n 8000`), while the
#     bytes on the wire fell 24.5%. The `uvcpp_web_app` legs, which already
#     cached the deflated variant, gain +67% and 23.7% fewer bytes. `font/woff`
#     and `font/woff2` also left the MIME table: those containers hold compressed
#     streams, and re-deflating them produced *bigger* output.
#
#   h3 -- `fix(quic): 短包头按「本端 scid_len」解 DCID`. The server decoded a
#     short-header DCID with the *peer's* SCID length, so after the handshake
#     every 1-RTT packet was read as an unknown CID and dropped. On the platform
#     the baseline-h3 run answered 294 responses in its 5 s window -- 4.6 per
#     connection, i.e. one initial PTO per request -- against 9,086,240 rps from
#     the leader on the same box. Locally the same rig goes 63 -> 86,000 req/s
#     with this one line. `fix(quic): 流关闭时把额度还给对端` stops a connection
#     from dying at 100 requests (the server never re-sent MAX_STREAMS), and
#     `feat(quic): QUIC 端点也能扇到多条循环上` gives the endpoint one socket
#     per loop under SO_REUSEPORT. Both are in this pin.
#   h2 -- `fix(webapp): 在途上限不再套在 h2 上`. The default
#     `max_pipelined_requests` of 8 was applied to h2 streams as well as to h1
#     pipelining, so a compliant client was answered `503` + `connection: close`
#     once 8 responses were in flight. Measured locally against the 1.6.0
#     package at the static-h2 shape: 578,096 of 742,441 requests answered 503
#     (78%); after the fix, 49 to 289 of ~163,000 (~0.1%). This is the one that
#     moved the h2 score.
#   h2c and gRPC -- `feat(web): 明文 h2` (prior-knowledge h2c on a cleartext
#     listener, off by default) and the four `feat(grpc)` commits: the
#     length-prefixed frame, the varint field walk, and the `grpc-status`
#     trailer that h2 had to learn to send first. No release has either.
#   static-h3 -- `feat(web): h3 上的稍后应答`. h3 dispatch rejected a deferred
#     response with a 500, which is what kept the static handler off QUIC.
#
# Three more are in the pin because they are fixed, not because these numbers
# depend on them. `fix(http2): WINDOW_UPDATE 不再算进控制帧预算` capped a
# connection at ~1 MiB/s when the peer honours its own flow control, but h2load
# raises its window to 2^30-1 and never sends a WINDOW_UPDATE, so the bucket is
# unreachable from these profiles. `perf(net): 接受连接时默认关 Nagle` is real --
# an h2 reply is written as two segments, and an idle peer's 40 ms delayed ACK
# parks the second -- but it only bites at one request in flight per connection,
# and these profiles run `-m 32` and `-m 100`, where the two arms overlap.
# `perf(web): 压缩不再每请求新建一个 deflate 内核` (+2.4% on json-comp,
# non-overlapping over 5 alternating rounds each way) is folded in as well.
#
# The pin moves past `fix(web): ws 发送队列整批合并` (219b0da), which is the one
# change here that a previous round already measured the absence of. Turning
# TCP_NODELAY on by default removed the incidental Nagle coalescing that had been
# batching the WebSocket echo replies, and `echo-ws-pipeline` fell from
# 10.85M / 12.63M / 10.63M req/s (512 / 4096 / 16384) to
# 4.30M / 3.94M / 3.81M -- while the single-depth `echo-ws` cases did not move
# at all, which is what pointed at the send path rather than the frame path.
# The fix batches a read-dispatch's whole send queue into one `uv_write`; the
# criterion is segments per frame on a 1500-byte veth (1.0000 -> 0.0625) and,
# end to end, 524k-859k frames/s -> 4,111k-5,215k, with a v1.6.1 control arm at
# 0.71 segments/frame and 1,014k frames/s.
#
# The pin then sat on `perf(json): 写出器的键与字符串值改成一次追加` (c8950ae), the
# change this entry's `/json` handler was rewritten onto. **The entry no longer
# uses it.** `uvcpp_json_writer` is an alternative JSON serializer, and the JSON
# profiles measure serialization, so Standard mode wants the framework's
# standard serializer: the response is built as an `nlohmann::json` DOM and sent
# through `resp.json()`. That is a deliberate, measured loss on `json-comp`,
# `json-tls` and `/async-db` (see the README's `/json` note); the paragraphs
# below are kept as the record of why the pin moved when it did, not as a
# description of what this entry does now. `uvcpp_json_writer`'s
# `key()` used to append the comma, the opening quote, the escaped body, the
# closing quote and the colon separately -- four or five out-of-line
# `std::string::append` calls per key, three per string value, 4073 of them on a
# 50-item response. They are built in a 256-byte stack buffer and appended once
# now: 4073 -> 1536, byte-identical output. Measured end to end on this entry at
# `/json/50?m=6`, server pinned to 6 single-threaded cores and saturated: 80.4 ->
# 63.2 us of server CPU per request (a repeat run put the pair at 73.8 -> 59.2),
# and 155.0 -> 141.1 us when the client sends `Accept-Encoding: gzip, br`, where
# compressing the 8.4 KB body costs more than writing it. The library-side
# criterion -- the append count, the byte-identity check over 70 `(count, m)`
# pairs and the per-family timings -- is in the library's `CHANGELOG.md`.
#
# The pin sits on `fix(build): brotli 取源码支路把 DLL 路径写进了链接行`
# (6eede33, 1.6.51-dev): the tip of the library's `master` when it was taken, 29
# commits past `c8950ae`, and the first revision whose four CI legs -- Linux,
# macOS, MSVC and MinGW-w64 -- are all green. This is a batch, not one named
# change, so the evidence is per-profile rather than per-commit; each change's
# own criterion is in the library's `CHANGELOG.md`, and what follows is what
# this entry's profiles do with them.
#
# The change that decides the move is the one `baseline-h3-64` was measuring the
# absence of. At `v1.6.1` -- the revision the merged entry pins, and the one that
# produced the board's 58 req/s -- the server decodes the short-header DCID from
# the *peer's* SCID length, so every 1-RTT packet after the handshake is dropped.
# `745608c` fixes it and is not in `v1.6.1`. Both arms below are this repository's
# entry, both libraries are built with the platform's own flag set
# (`BUILD_SHARED_LIBS=OFF`, `CMAKE_POSITION_INDEPENDENT_CODE=ON`, WEB / WEBAPP /
# ZLIB / OPENSSL / NGHTTP2 / QUIC / HTTP3 / DB / DB_PGSQL on, everything else off)
# and both are driven by the platform's own command lines, three runs each,
# median:
#
#   profile (platform command line)        merged @ v1.6.1    this branch @ 6eede33
#   baseline-h2   -c 64 -m 100                   618,420 rps             724,800 rps
#   baseline-h3   -c 64 -m 64, --alpn-list=h3         45 rps             200,999 rps
#   static-h2     -c 64 -m 32                     13,027 rps              28,937 rps
#   static-h3     -c 64 -m 64, --alpn-list=h3         33 rps              14,223 rps
#
# Every 6eede33 row above is 0 3xx / 0 4xx / 0 5xx across all three runs. The two
# h3 rows are not like-for-like and are not offered as such: the merged entry has
# no HTTP/3 static leg at all -- it mounts `serve_static` on the h1-TLS and h2
# listeners only -- so its 33 req/s is 404s, and adding `uvcpp_static_server` on
# the QUIC listener is what makes `static-h3` a profile this entry can even
# subscribe to. The h2 rows are the same shape on both sides, so the 1.17x and
# 2.22x there are the batch's own effect on those two profiles.
#
# What else is in the 29 commits, by the criterion each one carries in the
# `CHANGELOG.md` (these are library-side numbers; the entry-level effect is the
# h2 row above): the static hot path's `realpath` calls per request 2.734 -> 0.001
# and its lock acquisitions 9 -> 1, `slot_here()`'s 18.4 -> 0; the revalidation
# `stat` can move off the thread pool onto the loop thread
# (`uvcpp_web_revalidate`, default unchanged); the h2 outbound batch is capped at
# 64 KiB, which is where `static-h2`'s multi-gigabyte residency came from; the
# static whole-read quota is returned on the worker instead of the loop, which is
# what the batch of `503`s was; the h3 static leg gets `cache_tag` and a bounded
# LRU and stops copying the whole body on a hit; an h1 pipeline's final
# `Connection: close` no longer takes the earlier responses with it; and the h2
# outbound body is held by reference rather than copied per response. Two optional
# modules arrive in the same range and both default to OFF. A yyjson parse backend
# is one of them and stays off, so the flag set above builds it out. The brotli
# encoder is the other, and it is the one optional module this entry turns on.
#
# Why brotli is on. It is not a tuning knob -- it is the framework's own response
# compression middleware, reached only when the client asks for `br`, at the
# framework's own default quality (`k_brotli_quality_default`, which the header
# records as the maximum of its own score model). The profile that asks for it is
# `json-compressed`, whose guide puts "the framework or engine's built-in response
# compression" under `standard=`, names brotli in its own title, and whose request
# carries `Accept-Encoding: gzip, br`; equal `q` resolves to `br` by the rank rule in
# `web/uvcpp_http_compress.cpp`. Measured on the three json-comp bodies -- same
# entry binary, only the `.so` swapped -- gzip gives 970/1330/1567 B and `br` gives
# 856/1182/1389 B, which is the encoder's own table in `web/uvcpp_http_compress.h`.
# The cost lands where the profile pays for it: json-comp is the only profile scored
# on bytes (`rps * (minBpr/bpr)^2`) and it runs at 101% of its CPU budget, so ~+31.5
# us per response buys ~11% fewer bytes -- about x1.07 on both json-comp profiles,
# net of the requests per second that the extra CPU costs. The profiles that merely
# pass `br` through -- gateway-64 at 58% of budget, gateway-h3 at 58%,
# production-stack at 49% -- have the CPU headroom to absorb the same compression and
# collect the smaller bodies for free. `json-tls`, `json-h2c`, `async-db` and the h1
# baseline profiles send no `Accept-Encoding` at all, and the static legs already
# serve the `.br` twins shipped in `data/static` whether or not this flag is set: that
# selection is runtime, in `http_pick_precompressed_variants()`.
ARG LIBUVCPP_REPO=https://github.com/Antruly/libuvcpp.git
ARG LIBUVCPP_REF=02ae41806a25112ee7050ccfd4132138b1eb2630

ARG OPENSSL_VERSION=3.5.0

# `make` is here for OpenSSL, and only for OpenSSL: `./Configure` writes a
# Makefile whatever generator the project uses, so the `make -j"$(nproc)"` two
# steps down needs it, and `ninja-build` next to it does not stand in for it.
# The release workflow's linux legs do not list `make` and do not need to --
# they run on a GitHub-hosted image, which has build-essential already. A bare
# ubuntu:24.04 does not: `g++` pulls in gcc, binutils and libc6-dev but not
# `make`. Measured, not assumed -- the first run of this entry died at
# `[build 3/8]` with `/bin/sh: 1: make: not found` (exit 127).
#
# `zlib1g-dev` was listed here at the `v1.6.1` pin and has been dropped, because
# the install rules ship the headers themselves as of 1.6.3. The reason it was
# listed: the library fetches zlib and links it in statically
# (`-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON` below), so nothing on the system path
# provides `zlib.h` -- yet the installed public header `web/uvcpp_ws_parser.h`
# does `#include <zlib.h>` under `#if UVCPP_ZLIB_ENABLE`, and the generated
# `uvcpp_config.h` defines that to 1 (with an `#error` against defining it to 0
# yourself), so a consumer compiling against `/opt/uvcpp/include` cannot opt out
# of it. Before the fix, `[build 8/8]` died with
# `/opt/uvcpp/include/web/uvcpp_ws_parser.h:31:10: fatal error: zlib.h: No such
# file or directory`. Confirmed against this pin rather than assumed: `g++ -H`
# resolves `<zlib.h>` to `<prefix>/include/zlib.h`, and it keeps resolving there
# with the package gone, since `-I` is searched before the system directories.
# `libbrotli-dev` is here for `-DUVCPP_ENABLE_BROTLI=ON` below, and only the build
# stage needs it: the library uses `<brotli/encode.h>` inside one `.cpp`, so no
# installed public header reaches for it and the consumer below compiles against
# `/opt/uvcpp/include` without it. The CMake side takes the system copy through
# pkg-config (`libbrotlienc` / `libbrotlidec`) and falls back to `find_path` /
# `find_library`, so this is the package that keeps the encoder out of the
# FetchContent branch -- which would otherwise clone brotli at build time.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates cmake curl g++ git libbrotli-dev libpq-dev make ninja-build \
 && rm -rf /var/lib/apt/lists/*

# OpenSSL with the QUIC API: static, PIC, no test suite.
#
# `nm` on the archive is the assertion that matters. Without it a wrong
# OpenSSL silently produces a QUIC-less library (see the header), and the
# failure surfaces much later as an h3 listener that never comes up. Checking
# the library itself, not the headers, is the point: a header can declare a
# symbol the archive never defines. ` [TW]$` requires a definition, and the
# members of an archive carry no `@@OPENSSL_x.y.z` suffix, so the anchor is
# correct here.
RUN set -eux; \
    mkdir -p /opt/openssl-src; \
    curl -fsSL --retry 3 --retry-delay 5 -o /tmp/openssl.tar.gz \
        "https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/openssl-${OPENSSL_VERSION}"; \
    tar xzf /tmp/openssl.tar.gz -C /opt/openssl-src --strip-components=1; \
    cd /opt/openssl-src; \
    ./Configure --prefix=/opt/openssl --libdir=lib no-shared no-tests -fPIC; \
    make -j"$(nproc)"; \
    make install_sw; \
    /opt/openssl/bin/openssl version; \
    nm --defined-only /opt/openssl/lib/libssl.a \
        | grep -qE ' [TW] SSL_set_quic_tls_cbs$'; \
    rm -rf /tmp/openssl.tar.gz /opt/openssl-src

WORKDIR /src
RUN git init . \
 && git remote add origin "${LIBUVCPP_REPO}" \
 && git fetch --depth 1 origin "${LIBUVCPP_REF}" \
 && git checkout FETCH_HEAD

# `BUILD_SHARED_LIBS=OFF` makes every fetched dependency (libuv, llhttp, zlib,
# nghttp2, ngtcp2, nghttp3) a static library, and `OPENSSL_USE_STATIC_LIBS=ON`
# does the same for OpenSSL, so libpq is the only shared object the result
# needs beyond libc and libstdc++. `CMAKE_POSITION_INDEPENDENT_CODE=ON` is
# required, not defensive: BUILD_SHARED_LIBS=OFF also flips CMake's PIC default
# to OFF, and a non-PIC libuv/llhttp cannot be linked into libuvcpp.so.
#
# websocket, static files, h2 and h3 all come from these same flags -- there is
# no second build for the TLS legs.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/uvcpp \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DUVCPP_BUILD_LIBUV_FROM_SOURCE=ON \
        -DUVCPP_BUILD_SHARED=ON \
        -DUVCPP_BUILD_STATIC=OFF \
        -DUVCPP_BUILD_TESTS=OFF \
        -DUVCPP_BUILD_FUNCTIONAL=OFF \
        -DUVCPP_BUILD_EXAMPLES=OFF \
        -DUVCPP_BUILD_BENCH=OFF \
        -DUVCPP_BUILD_WEB=ON \
        -DUVCPP_BUILD_WEBAPP=ON \
        -DUVCPP_ENABLE_ZLIB=ON \
        -DUVCPP_ENABLE_BROTLI=ON \
        -DUVCPP_ENABLE_OPENSSL=ON \
        -DUVCPP_ENABLE_NGHTTP2=ON \
        -DUVCPP_ENABLE_QUIC=ON \
        -DUVCPP_ENABLE_HTTP3=ON \
        -DUVCPP_ENABLE_DB=ON \
        -DUVCPP_ENABLE_DB_PGSQL=ON \
        -DUVCPP_ENABLE_DB_MYSQL=OFF \
        -DUVCPP_ENABLE_DB_SQLITE=OFF \
        -DUVCPP_ENABLE_CAPI=OFF \
        -DUVCPP_ENABLE_WSDL=OFF \
        -DOPENSSL_USE_STATIC_LIBS=ON \
        -DOPENSSL_ROOT_DIR=/opt/openssl \
        -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON \
 && cmake --build build --parallel "$(nproc)" \
 && cmake --install build

# The entry itself. `-pthread` because libuv is inside the library and the
# static OpenSSL inside it uses thread-local storage.
COPY main.cc .
RUN g++ -std=c++11 -O2 -DNDEBUG -o /server main.cc \
        -I/opt/uvcpp/include \
        -L/opt/uvcpp/lib \
        -luvcpp -lpq \
        -Wl,-rpath,/opt/uvcpp/lib \
        -pthread

FROM ubuntu:24.04

# libstdc++ for the entry's own objects, libpq5 and libbrotli1 for the two shared
# libraries the build above could not absorb. `libbrotli1` is the second of those:
# unlike zlib and libuv, which the project fetches and links statically,
# `UVCPP_ENABLE_BROTLI` takes the system copy, so `libuvcpp.so` carries
# `libbrotlienc.so.1` and `libbrotlidec.so.1` in its `DT_NEEDED` and the server does
# not start without them. On noble they are both files of that one package -- the
# name is not `libbrotlienc1`, which exists on Debian but not here (`libbrotli-dev`
# itself depends on `libbrotli1`, so the build stage already pulls it in; the
# runtime stage needs it said out loud).
#
# apt pulls in libpq's transitive closure
# (libgssapi, libldap, libgnutls, libssl3t64, ...) on its own -- that OpenSSL
# 3.0 is libpq's, and it is a different copy from the 3.5 the library carries
# statically. Both live in the process; only libuvcpp.so's copy serves the h2
# and h3 listeners.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        libbrotli1 libpq5 libstdc++6 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/uvcpp/lib/libuvcpp.so /opt/uvcpp/lib/libuvcpp.so
COPY --from=build /server /server

# 8080 HTTP/1.1 + h2c, 8081 HTTPS/1.1, 8082 h2c, 8443 TCP+TLS for h2 and 8443
# UDP for h3. The benchmark reaches these through --network host, where EXPOSE is
# documentation; the validation harness publishes them with -p on a bridge
# network, and that is where the two 8443 entries stop being one line.
EXPOSE 8080 8081 8082 8443 8443/udp

CMD ["/server"]
