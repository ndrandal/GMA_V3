# GMA_V3 production image — multi-stage build that compiles the C++
# `gma_server` against a known-good third-party set and ships the
# binary on a slim Debian runtime.
#
# Build context is THIS repo root (so the sibling .dockerignore applies and
# host build/ artifacts are excluded — ENC-790/H6):
#
#   cd <path-to>/gma_v3
#   docker build -t gma_v3:dev .
#
# Runtime image is `debian:stable-slim` (not scratch). The C++
# binary links against system libstdc++/libgcc; copying those out of
# the build image into scratch is fragile, so we accept the Debian base.
#
# Default ports:
#   4000 — embassy↔gma cloudchannel WebSocket
#   9001 — TCP ITCH feed ingest
#
# Configuration: bind-mount or COPY a `gma.conf` over /app/gma.conf
# in deployment. The default config bundled below points at the
# canonical local-dev ports.

FROM debian:stable-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ca-certificates \
    libboost-system-dev \
    libboost-thread-dev \
    libssl-dev \
    rapidjson-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the repo into the image. .dockerignore keeps build*/ (and its
# absolute-path CMakeCache.txt), .git and IDE cruft out of the context.
COPY . .

# Out-of-source build in a clean directory. Release uses a portable
# microarchitecture baseline by default (see GMA_NATIVE_ARCH / GMA_ARCH_BASELINE
# in CMakeLists.txt) so the shipped binary runs on any modern x86_64 host.
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build --target gma_server -j

# ─────────────────────────────────────────────────────────────────────

FROM debian:stable-slim

# Boost is statically linked into gma_server (Boost_USE_STATIC_LIBS ON), so the
# runtime needs no libboost*. We deliberately do NOT pin a boost soname here
# (the old libboost-system1.83.0 / libboost-thread1.83.0 pins broke whenever the
# base image's boost version drifted — ENC-806/L16). The only dynamic deps are
# libstdc++/libgcc (in the base) and OpenSSL (wss:// feed support).
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /build/gma_server /app/gma_server
COPY src/util/gma.conf /app/gma.conf

EXPOSE 4000 9001
STOPSIGNAL SIGTERM

# CLI: ./gma_server <wsPort> <conf> [feedPort]. Default conf binds
# 4000 and 9001 (see gma.conf). docker-compose can override the
# command if a different config is mounted.
ENTRYPOINT ["/app/gma_server", "4000", "/app/gma.conf"]
