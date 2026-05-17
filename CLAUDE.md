# CLAUDE.md — emd Codebase Guide

This file gives Claude Code (and human contributors) the context needed to work productively in this repository without re-reading the entire spec.

---

## Codebase Overview

- **Language:** Pure C11. No C++ anywhere in `src/` or `include/`. The ABI surface must stay C-compatible so plugins can be loaded with `dlopen`.
- **Modules:** 19 core modules (see table below), each with a narrow public header under `include/emd/`. Implementation files are in `src/`. Phase 2 adds the `emd_cam` module and `agent_abi.h` for Go integration.
- **Build system:** CMake ≥ 3.22, Ninja generator preferred. Out-of-tree builds only (`build/`). No in-tree `cmake .`.
- **Binary output:** `build/emd` — static-PIE, typically 3–5 MB stripped.
- **Test framework:** cmocka (Apache 2.0), vendored under `third_party/cmocka/`. Unit test sources live in `tests/`.
- **Third-party vendoring:** All deps live under `third_party/` with pinned versions. Never `apt-get install` a dep and assume it's available.

---

## Key Invariants (Do Not Violate)

1. **No `pthread_mutex_lock` on the hot path.** Camera worker threads (`camera_worker` in `src/emd_supervisor.c`) must not acquire any contended mutex. Locks are permitted only in supervisor/config code and in the recorder and notifier threads outside the per-frame loop.

2. **C11 `_Atomic` with explicit memory orders.** Use `memory_order_acquire` / `memory_order_release` at handoff points. Use `memory_order_relaxed` only where justified by a code comment explaining why relaxed is safe. Never use the default `memory_order_seq_cst` implicitly — be explicit.

3. **Zero-copy from depacketizer into ring buffer.** The RTP payload is written directly into the ring buffer backing store (`emd_ringbuf_reserve` → memcpy → `emd_ringbuf_commit`). No per-NAL malloc. No intermediate copy. See `push_nal_to_ring` in `src/emd_supervisor.c`.

4. **Per-camera arena allocator.** Each camera's ring buffer is backed by a contiguous arena sized at startup (`buffer_seconds × max_bitrate_bps / 8 × 1.25`). NAL records index into this arena. Do not call `malloc` per NAL unit on the hot path.

5. **Clips are written atomically.** Always: write to `.part` file → `fsync` (per policy) → `rename` to final path. The MQTT notifier must only publish after the rename succeeds. Never publish a path that may not yet exist.

6. **No pixel decoding on the hot path.** The inspector (`emd_inspector`) works on NAL headers, slice types, and byte-count statistics only. Calling any decode function (libavcodec, etc.) from the camera worker loop is forbidden.

7. **MQTT failure must not block clips.** If the MQTT broker is unreachable, the notifier queues events up to `notifier.queue_max` and drops oldest — it never stalls the recorder or the worker. See §4.4 of the spec.

---

## Coding Rules

These are enforced by CI and will cause build failures if violated:

- **Banned functions:** `gets`, `strcpy`, `sprintf`. Use `fgets`/`strncpy`/`snprintf`. CI runs a grep check.
- **Compiler flags (all must pass cleanly):**
  - `-Wall -Wextra -Wpedantic`
  - `-Wconversion -Wshadow -Wformat=2`
  - `-Werror`
  - Vendored third-party code under `third_party/` is built with `-w` (warnings suppressed) — do not apply this to `src/`.
- **No VLAs.** `-Wvla` is implicitly covered by `-Wpedantic` in C11 mode.
- **All public functions must have declarations in the corresponding `include/emd/*.h` header.** No `extern` declarations in `.c` files that reference another module's internals.
- **Integer types:** prefer `uint32_t`/`uint64_t`/`int32_t` from `<stdint.h>`. Avoid bare `int` for sizes and counts where the range matters.

---

## Module Map

| Module | Header | Key Types |
|---|---|---|
| `emd_config` | `emd/config.h` | `emd_config_t`, `emd_camera_cfg_t`, `emd_container_t`, `emd_fsync_policy_t` |
| `emd_log` | `emd/log.h` | `emd_log_level_t`, `EMD_LOGI/W/E/F` macros |
| `emd_metrics` | `emd/metrics.h` | `emd_counter_t`, `emd_gauge_t`, `emd_histogram_t` |
| `emd_net` | `emd/net.h` | `emd_tcp_connect`, `emd_epoll_*` |
| `emd_rtsp` | `emd/rtsp.h` | `emd_rtsp_client_t`, `emd_rtsp_tick` |
| `emd_rtp` | `emd/rtp.h` | `emd_rtp_pkt_t`, `emd_rtp_parse` |
| `emd_h264_depay` | `emd/h264_depay.h` | `emd_h264_depay_t`, `emd_h264_nal_cb_t` |
| `emd_h265_depay` | `emd/h265_depay.h` | `emd_h265_depay_t`, `emd_h265_nal_cb_t` |
| `emd_h264_parse` | `emd/h264_parse.h` | `emd_h264_sps_t`, `emd_h264_slice_hdr_t` |
| `emd_h265_parse` | `emd/h265_parse.h` | `emd_h265_sps_t`, `emd_h265_slice_hdr_t` |
| `emd_inspector` | `emd/inspector.h` | `emd_inspector_state_t`, `emd_inspector_cfg_t`, `emd_inspector_process` |
| `emd_ringbuf` | `emd/ringbuf.h` | `emd_ringbuf_t`, `emd_nal_record_t`, `emd_ringbuf_snap_t` |
| `emd_event` | `emd/event.h` | `emd_event_t`, `emd_event_bus_t`, `emd_event_bus_push/pop` |
| `emd_recorder` | `emd/recorder.h` | `emd_recorder_pool_t`, `emd_mux_backend_t`, `emd_clip_header_t` |
| `emd_mux_mpegts` | (internal) | MPEG-TS muxer implementation (used by recorder) |
| `emd_mux_fmp4` | (internal) | fMP4 muxer implementation (used by recorder) |
| `emd_mqtt` | `emd/mqtt.h` | `emd_mqtt_client_t`, `emd_mqtt_cfg_t`, `emd_mqtt_publish_str` |
| `emd_supervisor` | `emd/supervisor.h` | `emd_supervisor_run`, `emd_sdnotify` |
| `emd_cam` | `emd/agent_abi.h` | Phase 2: `emd_cam_t`, `emd_cam_open`, `emd_cam_update_inspector_cfg` |

---

## How to Add a New Camera Module

The camera pipeline is driven by `camera_worker` in `src/emd_supervisor.c`. To add support for a new codec or transport:

1. **Create a depacketizer** (`src/emd_<codec>_depay.c`, header `include/emd/<codec>_depay.h`) following the pattern of `emd_h264_depay`. It must expose a NAL callback type `emd_<codec>_nal_cb_t` with signature `(const uint8_t *nal, size_t len, bool marker, uint32_t pts, void *userdata)`.

2. **Create a NAL parser** (`src/emd_<codec>_parse.c`) to identify NAL types (keyframe, param set, etc.) and extract the fields the inspector needs.

3. **Wire the codec into `camera_worker`:** add an init branch for the new codec in the `ctx.codec` switch, register a NAL callback that calls `push_nal_to_ring` with the correct `flags` bitmask (`EMD_NAL_KEYFRAME`, `EMD_NAL_PARAMSET`).

4. **Add unit tests** in `tests/test_<codec>_depay.c` and `tests/test_<codec>_parse.c` following the pattern of the H.264/H.265 test files.

5. **Register the test** in `tests/CMakeLists.txt` using the `emd_add_test` macro.

---

## How to Run Tests

### Unit tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
cd build && ctest --output-on-failure
```

### End-to-end test (requires Python 3 and ffmpeg)

```sh
bash scripts/e2e_test.sh
```

Or directly:

```sh
python3 tests/integration/e2e_test.py \
    --binary build/emd \
    --fixtures-dir tests/fixtures/streams \
    --scenario motion \
    --container mpegts
```

### Sanitizers

ASan + UBSan:

```sh
cmake -S . -B build-asan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan --parallel
cd build-asan && ctest --output-on-failure
```

TSan (thread sanitizer — catches data races):

```sh
cmake -S . -B build-tsan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=thread"
cmake --build build-tsan --parallel
cd build-tsan && ctest --output-on-failure
```

MSan (memory sanitizer — Clang only):

```sh
cmake -S . -B build-msan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_C_FLAGS="-fsanitize=memory -fno-omit-frame-pointer"
cmake --build build-msan --parallel
cd build-msan && ctest --output-on-failure
```

### Coverage report (gcovr)

```sh
cmake -S . -B build-cov \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="--coverage"
cmake --build build-cov --parallel
cd build-cov && ctest
gcovr --root .. --exclude '../tests/' --exclude '../third_party/' \
      --html-details coverage.html
```

---

## Third-Party Dependencies

| Library | License | Location | Notes |
|---|---|---|---|
| `tomlc99` | MIT | `third_party/tomlc99/` | Single `.c` + `.h` pair; built as `tomlc99` static lib |
| `mqtt-c` (LiamBindle) | MIT | `third_party/mqtt-c/` | `mqtt.c` + `mqtt_pal.c`; built as `mqtt_c` static lib |
| `cmocka` | Apache 2.0 | `third_party/cmocka/` | Test-only; system installation preferred, vendored stub fallback |

### Adding a new vendored dependency

1. Copy the source into `third_party/<name>/` with a pinned version tag in a `third_party/<name>/VERSION` file.
2. Add a `add_library` target in the root `CMakeLists.txt` with `target_compile_options(... PRIVATE -w)` to suppress vendored-code warnings.
3. Add the dependency to this table.
4. Update the SBOM (CycloneDX) generation step in CI.

---

## License Policy

- **No GPL** components, statically or dynamically linked in the production binary.
- **No shelling out to the `ffmpeg` CLI** from the production binary. Any FFmpeg usage must be in-process via libavcodec/libavformat, isolated behind the `emd_decoder_libav` plugin (dynamic link only, LGPL build of FFmpeg with `--disable-gpl --disable-nonfree`).
- **The `ffmpeg` CLI is acceptable in test scripts only** (e.g., `scripts/generate_fixtures.sh`) because test tooling is not shipped in the production binary.
- **LGPL dynamic-link** is acceptable for optional plugins (`emd_mux_libav`, `emd_decoder_libav`). Static link of LGPL is not acceptable (relinkability requirement conflicts with static-PIE).
- CI runs a license audit step that greps the shipped binary for GPL strings; this check must pass before any release tag.
