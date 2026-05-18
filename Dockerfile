# Multi-stage Dockerfile for emd-agent (x86_64)
#
# Stage 1: Build C library (libemd)
# Stage 2: Build Go agent
# Stage 3: Runtime image

# Stage 1: Build libemd
FROM debian:bookworm-slim AS libemd-builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy C source
COPY include/ ./include/
COPY src/ ./src/
COPY third_party/ ./third_party/
COPY CMakeLists.txt ./

# Build libemd (with relaxed warnings for Docker build)
RUN cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-O2 -D_POSIX_C_SOURCE=200809L -w" \
    -DBUILD_TESTS=OFF \
    && cmake --build build --parallel \
    && mkdir -p /output/lib /output/include \
    && cp build/libemd.a /output/lib/ \
    && cp -r include/emd /output/include/

# Stage 2: Build Go agent
FROM golang:1.23-bookworm AS go-builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy libemd artifacts from stage 1
COPY --from=libemd-builder /output /build/third_party/libemd

# Copy Go source
COPY go.mod go.sum ./
COPY cmd/ ./cmd/
COPY internal/ ./internal/

# Build Go agent (static binary where possible)
RUN CGO_ENABLED=1 go build \
    -tags 'netgo,osusergo' \
    -ldflags '-s -w -extldflags "-static-libgcc"' \
    -o /output/emd-agent \
    ./cmd/emd-agent

# Stage 3: Runtime image
FROM debian:bookworm-slim

# Install runtime dependencies (only libc, libm, libpthread)
RUN apt-get update && apt-get install -y \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -u 1000 -m -s /bin/false emd

# Copy binary
COPY --from=go-builder /output/emd-agent /usr/local/bin/emd-agent

# Create directories
RUN mkdir -p /var/lib/emd-agent/clips \
    /var/lib/emd-agent/inflight \
    /etc/emd-agent \
    && chown -R emd:emd /var/lib/emd-agent

# Volumes for persistence
VOLUME ["/var/lib/emd-agent", "/etc/emd-agent"]

# Run as non-root
USER emd

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s \
    CMD pgrep emd-agent || exit 1

ENTRYPOINT ["/usr/local/bin/emd-agent"]
CMD ["--config", "/etc/emd-agent/agent.toml"]
