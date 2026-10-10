# libuvcpp entry for HttpArena.
#
# The build downloads the official v1.6.0 linux-x64 release package and
# compiles main.cc against it. That package is the artifact users actually get
# from the GitHub release, and its shared object already contains libuv,
# OpenSSL, nghttp2, ngtcp2 and zlib -- `readelf -d` on libuvcpp.so lists only
# libm, libc and the loader. So the runtime image needs no toolchain, no
# OpenSSL and no HTTP/2 or QUIC library: libc and libstdc++ are the whole
# dependency set, for the HTTP/3 listener included.
#
# The download is pinned by sha256 rather than by tag. A release tag can be
# moved and its assets re-uploaded, so the hash is the only part of this file
# that fixes which bytes get built.
#
# Both stages are ubuntu:24.04 (glibc 2.39; the release package's floor is
# 2.34), so the compiler that links /server and the libc that runs it are the
# same ones. Alpine is not an option: the package is glibc-linked.

FROM ubuntu:24.04 AS build

ARG LIBUVCPP_VERSION=1.6.0
ARG LIBUVCPP_SHA256=b64ae68d121b49543e7a37126bbf38adf95a00ac51bff463b11be2dc499a138b

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates curl unzip g++ \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
# --retry is for the download, not for the hash: a transient failure fetching
# from the release CDN would otherwise redden a build that has nothing to do
# with the entry. Nothing is trusted on the strength of the retry -- the hash
# below is what decides whether the bytes are the release's.
RUN curl -fsSL --retry 3 --retry-connrefused -o libuvcpp.zip \
        "https://github.com/Antruly/libuvcpp/releases/download/v${LIBUVCPP_VERSION}/libuvcpp-${LIBUVCPP_VERSION}-linux-x64.zip" \
 && echo "${LIBUVCPP_SHA256}  libuvcpp.zip" | sha256sum -c - \
 && unzip -q libuvcpp.zip -d /opt \
 && rm -f libuvcpp.zip

COPY main.cc .
RUN g++ -std=c++11 -O2 -DNDEBUG -o /server main.cc \
        -I/opt/libuvcpp-1.6.0-linux-x64/include \
        -L/opt/libuvcpp-1.6.0-linux-x64/lib \
        -luvcpp -Wl,-rpath,/opt/libuvcpp-1.6.0-linux-x64/lib -pthread

FROM ubuntu:24.04

# g++ links the C++ runtime dynamically, so the runtime layer needs it. (The
# library's own .so does not -- it carries its copy statically.)
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/libuvcpp-1.6.0-linux-x64/lib/libuvcpp.so \
                  /opt/libuvcpp-1.6.0-linux-x64/lib/libuvcpp.so
COPY --from=build /server /server

# 8080 HTTP/1.1, 8081 HTTPS/1.1, 8443 TCP+TLS for h2 and 8443 UDP for h3. The
# benchmark reaches these through --network host, where EXPOSE is documentation;
# the validation harness publishes them with -p on a bridge network, and that is
# where the two 8443 entries stop being one line.
EXPOSE 8080 8081 8443 8443/udp

CMD ["/server"]
