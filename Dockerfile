# GMA_V3 production image — multi-stage build that compiles the C++
# `gma_server` against a known-good third-party set and ships the
# binary on a slim Debian runtime.
#
# Build context must be the workspace root so this Dockerfile can see
# the GMA_V3 source tree:
#
#   cd <workspace-root>/
#   docker build -f GMA_V3/Dockerfile -t gma_v3:dev .
#
# Runtime image is `debian:stable-slim` (not scratch). The C++
# binary links against system libstdc++/libgcc; copying those out of
# the build image into scratch is fragile, so we accept the 80 MB
# Debian base.
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
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY GMA_V3/ GMA_V3/

WORKDIR /src/GMA_V3/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release \
 && cmake --build . --target gma_server -j

# ─────────────────────────────────────────────────────────────────────

FROM debian:stable-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libboost-system1.83.0 \
    libboost-thread1.83.0 \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/GMA_V3/build/gma_server /app/gma_server
COPY GMA_V3/src/util/gma.conf /app/gma.conf

EXPOSE 4000 9001
STOPSIGNAL SIGTERM

# CLI: ./gma_server <wsPort> <conf> [feedPort]. Default conf binds
# 4000 and 9001 (see gma.conf). docker-compose can override the
# command if a different config is mounted.
ENTRYPOINT ["/app/gma_server", "4000", "/app/gma.conf"]
