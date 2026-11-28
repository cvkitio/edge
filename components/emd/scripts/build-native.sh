#!/usr/bin/env bash
# Build libemd and emd-agent natively on the host, without Docker. Works on old toolchains too
# (Jetson Nano / JetPack 4: gcc 7.5, glibc 2.27), given CMake >= 3.22 and Go >= 1.23 on PATH.
#
#   bash scripts/build-native.sh [output binary]     (default: build/emd-agent)
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/emd-agent}
jobs=$(nproc 2>/dev/null || echo 2)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build build --parallel "$jobs"

# The Go bindings link third_party/libemd (see internal/libemd/bindings.go); the headers there are
# tracked copies of include/emd, refreshed here so they always match the library
mkdir -p third_party/libemd/lib third_party/libemd/include
cp build/libemd.a third_party/libemd/lib/
cp -r include/emd third_party/libemd/include/

CGO_ENABLED=1 go build -trimpath -ldflags "-s -w" -o "$out" ./cmd/emd-agent
"$out" -version
