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

# The library source, pinned to the v1.6.1 release revision. By commit rather
# than by tag: a tag can be moved and the bytes under it re-uploaded, so a SHA
# is the only thing that fixes which revision gets built. `3fdd6208` is what
# `v1.6.1` resolves to, and it is the revision the Release workflow builds the
# six published packages from -- the only thing in it past the version bump is
# the two READMEs' download tables, so the sources are the ones the fixes
# describe.
#
# Three of the four fixes in 1.6.1 are load-bearing for the profiles this entry
# subscribes to, and each was measured against the 1.6.0 release package before
# being taken:
#
#   `fix(quic): 流关闭时把额度还给对端` -- without it a QUIC connection stops
#     being served after 100 requests (the server never re-sends MAX_STREAMS).
#     h2load opens one connection for the whole baseline-h3 run, so the run was
#     collapsing partway through.
#   `fix(webapp): 在途上限不再套在 h2 上` -- the default `max_pipelined_requests`
#     of 8 was applied to h2 streams as well as to h1 pipelining, so a compliant
#     client was answered with `503` + `connection: close` once 8 responses were
#     in flight. Measured locally against the 1.6.0 package at the static-h2
#     shape: 578,096 of 742,441 requests answered 503 (78%); after the fix, 49
#     to 289 of ~163,000 (~0.1%). This is the one that moves the score.
#   `perf(web): 压缩不再每请求新建一个 deflate 内核` -- +2.4% on json-comp
#     (non-overlapping, 5 alternating rounds each way).
#
# The fourth, `fix(http2): WINDOW_UPDATE 不再算进控制帧预算`, is a real bug (a
# 32 frame/s bucket counted WINDOW_UPDATE, capping a connection at ~1 MiB/s when
# the peer honours its own flow control) but it does **not** show up here:
# h2load raises its flow-control window to 2^30-1 and never sends a
# WINDOW_UPDATE, so the bucket is unreachable from these profiles. It is in the
# pin because it is fixed, not because these numbers depend on it.
ARG LIBUVCPP_REPO=https://github.com/Antruly/libuvcpp.git
ARG LIBUVCPP_REF=3fdd620825bc95d7ae04b8fba276f74162ba4f45

ARG OPENSSL_VERSION=3.5.0

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates cmake curl g++ git libpq-dev ninja-build \
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

# libstdc++ for the entry's own objects, libpq5 for the one shared library the
# build above could not absorb. apt pulls in libpq's transitive closure
# (libgssapi, libldap, libgnutls, libssl3t64, ...) on its own -- that OpenSSL
# 3.0 is libpq's, and it is a different copy from the 3.5 the library carries
# statically. Both live in the process; only libuvcpp.so's copy serves the h2
# and h3 listeners.
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 libpq5 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/uvcpp/lib/libuvcpp.so /opt/uvcpp/lib/libuvcpp.so
COPY --from=build /server /server

# 8080 HTTP/1.1, 8081 HTTPS/1.1, 8443 TCP+TLS for h2 and 8443 UDP for h3. The
# benchmark reaches these through --network host, where EXPOSE is documentation;
# the validation harness publishes them with -p on a bridge network, and that is
# where the two 8443 entries stop being one line.
EXPOSE 8080 8081 8443 8443/udp

CMD ["/server"]
