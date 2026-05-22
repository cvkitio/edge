# Edge Motion Detector — MB-Layer Parsing & Advanced Compressed-Domain Detection

**Codename:** `emd-mb-parse`
**Document status:** Draft v0.1
**Owner:** Andrew Sinclair
**Date:** 2026-05-20
**Audience:** Implementers, reviewers, QA
**Prerequisite:** `emd-spec.md` v1.0 (Phase 1 core), `emd-phase2-spec.md` v0.1 (Go outer)
**Tracks:** Phase 3 — detector enrichment

---

## 1. Purpose

The current `emd_inspector` (Phase 1 §7) operates on a single scalar per access unit (NAL byte count) plus a binary "intra slice present" flag, with detection thresholds tuned by autotune. This is robust as an alarm but is structurally incapable of:

- Localising motion within the frame (no spatial information).
- Distinguishing a frame-wide encoder hiccup (IR flicker, lighting cut, GOP refresh) from real scene activity.
- Reporting motion ROIs to downstream consumers (postprocess, ML inference, operator UI).
- Suppressing persistent local motion (waving trees, water surfaces, ceiling fans) without manual exclusion masks.

This spec defines a Phase 3 addition to `emd_inspector`: a per-macroblock parsing layer that extracts motion vectors, partition modes, residual presence, and per-MB QP from the H.264 bitstream, an enriched inspector that combines those signals into a multi-feature detection rule, and a spatial filter that produces MB-grid ROI hit-maps. All behaviour is gated behind feature flags so existing deployments continue to use the byte-count detector unchanged.

The single most important scoping fact: **≥90% of deployed feeds use H.264 High Profile with CABAC entropy coding**. CABAC parsing is therefore in Phase 1 of this track, not deferred behind CAVLC.

### 1.1 Goals

1. Extract per-MB motion vectors, MB/sub-MB partition modes, CBP (coded block pattern), `mb_qp_delta`, and skip/intra flags from H.264 streams using **CABAC** entropy decoding, without performing inverse transform, motion compensation, or deblocking.
2. Maintain the existing "no pixel decoding on the hot path" invariant (Phase 1 CLAUDE.md §6). Entropy-only MB-layer parsing is explicitly permitted.
3. Preserve the existing byte-count detector as the default and as a fail-open fallback. MB parsing is opt-in per camera and per build.
4. Extend `emd_inspector_input_t` with the new signals; extend `emd_inspector` with multi-signal combine rules; extend `emd_event_t` so autotune can search over the new threshold space.
5. Add a spatial filter that produces an MB-grid ROI hit-map per event for downstream consumers via the Phase 2 `emd_cam` ABI.
6. CPU budget: ≤30% of one vCPU per 1080p30 camera with all MB-parse features enabled on a Raspberry Pi 5–class core (Cortex-A76 @ 2.4 GHz). This is the design budget; see §10 for measurement.

### 1.2 Non-goals (this phase)

- H.265/HEVC MB-layer parity (deferred to a follow-up; the inspector signal layer is codec-agnostic so this is a parser-only follow-up).
- Interlaced / MBAFF / PAFF support. Streams with `mb_adaptive_frame_field_flag == 1` or `field_pic_flag == 1` shall be **detected at SPS/slice-header parse time** and the MB parser shall refuse to run on them, falling back to byte-count.
- Transform coefficient values. We parse coefficient *presence* (CBP, `coded_block_flag`) only. The bin cost of `significant_coeff_flag` / `last_significant_coeff_flag` / coefficient levels is the dominant CABAC cost and we explicitly avoid it.
- Tracking across frames. Sabirin & Kim's spatio-temporal graph approach [6] is in scope for a later phase that runs on the `postprocess` server tier, not on the edge inspector. This phase produces the per-frame attributed MB grid that such a tracker would consume.
- H.264 SVC / MVC. Single-layer Annex-B only.
- B-slice direct-mode inference table maintenance beyond what's required to advance the bitstream cursor. We compute MVs for inter MBs; direct-mode MVs are derived rather than coded and we accept either skipping them or storing them as "unknown" in v0.

### 1.3 Why CABAC first

Earlier draft plans put CABAC behind CAVLC ("scope cut to Baseline/Main first, add CABAC later"). Production reality overrides:

- Surveillance cameras shipped since ≈2015 default to High Profile @ L4.0/L4.1 with CABAC; only the cheapest end-of-life devices still emit Baseline.
- Bitrate savings from CABAC are the reason vendors enable it — the same parsing effort buys ≈30–60% more streams per ingest budget downstream.
- A CAVLC-only Phase 1 would force a second migration after deployment; the parsing surface is large enough that doing it twice is expensive.
- CABAC complexity is concentrated in the arithmetic decoder engine and context model tables. Both are bounded: ≈1000 lines of code and ≈460 contexts with documented initialization tables (H.264 spec §9.3, Tables 9-12 to 9-44). Skipping coefficient parsing removes the part that scales with frame energy.

CAVLC support is included in this phase as a parallel path with shared MB-layer syntax code; the entropy decode is the only branch.

---

## 2. References

Throughout this document, citations in `[N]` form refer to:

| Tag | Reference |
|---|---|
| **[1]** | RidgeRun. *H.264 Motion Vector Extractor*. developer.ridgerun.com/wiki/index.php/H.264_Motion_Vector_Extractor — GStreamer element design, MV-only output API patterns. |
| **[2]** | *See Without Decoding: Motion-Vector-Based Tracking in Compressed Video*. arXiv 2602.00153 — MV + residual feature aggregation for tracking; informs the Phase 3+ tracker layer (out of scope here but informs API). |
| **[3]** | You, Sabirin, Kim. *Real-time Detection and Tracking of Multiple Objects with Partial Decoding in H.264/AVC Bitstream Domain*. SPIE 7244, 2009 — PSMF (Probabilistic Spatio-temporal Macroblock Filter); informs §6 spatial filter design. |
| **[4]** | Laumer, Amon, Hutter, Kaup. *Moving object detection in the H.264/AVC compressed domain*. APSIPA Trans. SIP, vol. 5 e18, 2016 (doi 10.1017/ATSIP.2016.18) — syntax-only motion weights from partition modes + QP; informs §5.2 motion-weight scoring. |
| **[5]** | Szczerba, Forchhammer, Støttrup-Andersen, Eybye. *Fast Compressed Domain Motion Detection in H.264 Video Streams for Video Surveillance*. AVSS 2009 — MV-magnitude thresholding with spatial/temporal confidence; informs §5.3 activity-ratio and MV-p90 signals. |
| **[6]** | Sabirin, Kim. *Moving Object Detection and Tracking Using a Spatio-Temporal Graph in H.264/AVC Bitstreams*. IEEE TMM 14(3), 2012 — attributed graph with MV + non-zero residual presence; informs the `extract_cbp` requirement and §6 connected-component output. |
| **[7]** | LukasBommes/mv-extractor (MIT). github.com/LukasBommes/mv-extractor — reference implementation of MV-only output API; "decode-frames=False" mode validates the entropy-only design point. |
| **[8]** | carrardt/h264-tools. github.com/carrardt/h264-tools — minimal NAL-layer C parser; reference for the framing depth we already have. |
| **[S]** | ITU-T Rec. H.264 (08/2021) — Advanced video coding for generic audiovisual services. Authoritative source for syntax, semantics, and CABAC procedures cited by section number throughout this spec. |

References [1]–[8] are kept in the source tree under `docs/references/` as PDFs / link archives so they survive link rot; see §13.

---

## 3. Scope of CABAC support in v0

The H.264 CABAC specification (H.264 §9.3) is large. v0 of this parser implements exactly the subset needed to extract the MB grid described in §4 from the streams we expect to see in production. The strategy is to read every syntax element the spec requires us to read (the bitstream is sequential — we cannot skip bytes), but to **discard** any value we don't need.

### 3.1 In v0 — must parse, must store

- `mb_skip_flag` (P, B slices)
- `mb_type` (I, P, SP, B — all variants)
- `sub_mb_type` (P, B)
- `mvd_l0`, `mvd_l1` per MV component → reconstructed `mv_l0_x/y`, `mv_l1_x/y` via median predictor (H.264 §8.4.1.3)
- `coded_block_pattern` (luma 4-bit + chroma 2-bit)
- `mb_qp_delta`
- `transform_size_8x8_flag` (when present)

### 3.2 In v0 — must parse, may discard value (advance cursor only)

- `ref_idx_l0`, `ref_idx_l1` — we don't track reference lists in v0; values consumed but not stored. (Future tracker layer will need these; the API in §4 reserves space.)
- `prev_intra4x4_pred_mode_flag`, `rem_intra4x4_pred_mode`, `intra_chroma_pred_mode` — intra prediction modes; consumed but not stored.
- `coded_block_flag` per 4x4 / 8x8 block when CBP indicates non-zero coefficients — we store the **OR** across the MB as a single bit (`has_residual`), per [6]. Per-block flags are not retained in v0.
- `significant_coeff_flag`, `last_significant_coeff_flag`, coefficient levels — **these we do not parse at all**. Once we've consumed `coded_block_flag` and confirmed it's nonzero for some block, we use the H.264 termination rule (§9.3.3.1.1.9) to skip ahead to the next syntax element. *This is the key performance optimization.* See §3.4.

### 3.3 In v0 — refuse to parse, fall back

When any of the following is present in SPS / PPS / slice header, the MB parser shall return `EMD_MB_REFUSED` and the supervisor shall fall back to the byte-count detector for that slice:

- `frame_mbs_only_flag == 0` (interlaced support)
- `mb_adaptive_frame_field_flag == 1`
- `field_pic_flag == 1`
- `slice_type` indicates SP or SI (rare in surveillance; not worth the syntax cost in v0)
- `num_slice_groups_minus1 > 0` (FMO / arbitrary slice ordering)
- `redundant_pic_cnt_present_flag == 1` with a redundant slice
- SPS `profile_idc` ∈ {Scalable Baseline, Scalable High, Stereo High, Multiview High} — SVC/MVC variants

Refusal is per-slice; subsequent slices are retried. Refusal counters are exported as metrics (§9).

### 3.4 Coefficient-skip optimization

The CABAC residual_block procedure (H.264 §9.3.3.1.1.9) decodes `coded_block_flag`, then if nonzero, `significant_coeff_flag`/`last_significant_coeff_flag` pairs to identify which of 16 (or 64 for 8x8) positions have nonzero coefficients, then magnitude and sign for each. The magnitude/sign part dominates the bin count and is the heaviest CABAC cost for typical bitstreams (often 30–60% of all bins for inter frames at moderate QP).

We exploit two facts:

1. We only need `coded_block_flag` (presence). The H.264 spec guarantees that `coded_block_flag == 0` ends the block's contribution to the bitstream with no further bins. So when CBP indicates the block is coded but we don't care about the actual coefficients, we still must consume the `significant_coeff_flag` / `last_significant_coeff_flag` / coefficient bins to advance the CABAC state correctly — the arithmetic decoder is not byte-aligned and cannot skip arbitrary bins.
2. **Therefore we must implement the full residual_block_cabac procedure**, but optimize the path that decodes coefficient values: discard the values after reading them, hold no per-coefficient state, and do not write to a coefficient array. The decoder still runs; we just have ~10–20 lines of "consume and forget" instead of "consume and emit".

This is the dominant cost path. CPU budget validation (§10) verifies that even with full residual_block bin consumption, the parser fits the budget.

### 3.5 Out of v0 (deferred)

- 8x8 transform residual variants when `transform_8x8_mode_flag == 1` — supported in v0 because most High Profile encoders use it; cited here only to flag it as a known testing dimension.
- Adaptive scaling lists. We must read them in PPS if present (to advance the cursor) but never apply them.
- Cabac error concealment. If a parse error occurs mid-slice, the slice is marked corrupt, MB grid is invalidated for that AU, fail-open kicks in.

---

## 4. Module decomposition

### 4.1 New modules

| Module | Header | Purpose |
|---|---|---|
| `emd_cabac` | `emd/cabac.h` | Pure arithmetic decoder engine. No syntax knowledge. `DecodeDecision`, `DecodeBypass`, `DecodeTerminate`, renormalization, context model state (pStateIdx + valMPS pair, 1 byte each). Context init from `SliceQPY` + `cabac_init_idc` per H.264 §9.3.1.2 and Tables 9-12 to 9-23. |
| `emd_cabac_ctx_init` | `emd/cabac_ctx_init.h` | Auto-generated (committed) header containing the H.264 context model initialization tables (≈460 contexts × 3 init_idc values). Generator script in `scripts/gen_cabac_tables.py` consumes the spec PDF and emits C arrays; output is a static table and must be regenerated only when the spec edition changes. |
| `emd_h264_mb` | `emd/h264_mb.h` | H.264 MB-layer syntax decoder. Public entry point: `emd_h264_parse_slice_data(slice_ctx, mb_grid_out)`. Dispatches to CABAC or CAVLC based on PPS `entropy_coding_mode_flag`. |
| `emd_h264_mb_cabac` | (internal to `emd_h264_mb`) | CABAC-specific MB syntax binding: `mb_skip_flag`, `mb_type`, `sub_mb_type`, `mvd_lX`, `ref_idx_lX`, `mb_qp_delta`, `coded_block_pattern`, `coded_block_flag`, etc., each calling `emd_cabac` decisions with the right context index. |
| `emd_h264_mb_cavlc` | (internal to `emd_h264_mb`) | CAVLC-specific MB syntax binding for Baseline/Main streams. Re-uses the existing `emd_h264_parse` exp-Golomb reader. |
| `emd_h264_mv_predict` | `emd/h264_mv_predict.h` | MV median predictor per H.264 §8.4.1.3. Pure function over MB grid neighbour MVs. Independent of entropy mode. |
| `emd_mb_grid` | `emd/mb_grid.h` | Per-camera arena-backed grid of `emd_mb_record_t`. Allocated at camera start from SPS dimensions. Read/write API for the parser; read-only snapshot API for the inspector and downstream consumers. |
| `emd_spatial` | `emd/spatial.h` | MB-grid morphological filter, temporal accumulator, connected-component labelling. Operates on the binary "active MB" map produced by the inspector. |

### 4.2 Modified modules

| Module | Change |
|---|---|
| `emd_h264_parse` | Extend SPS parse to High Profile fields (`chroma_format_idc`, `bit_depth_luma_minus8`, `bit_depth_chroma_minus8`, `seq_scaling_matrix_present_flag` consumption, full timing/VUI). Extend PPS parse (`transform_8x8_mode_flag`, `chroma_qp_index_offset`, `weighted_pred_flag`, `weighted_bipred_idc`, scaling list consumption). Extend slice header parse to all fields needed up to `slice_data()`: `slice_qp_delta` (real value, not 0), `cabac_init_idc`, `direct_spatial_mv_pred_flag`, reordering, weighted pred tables, ref pic marking, slice group changes. |
| `emd_inspector` | New input fields (§5.1). New EWMA pairs for the new signals (§5.2). Multi-signal combine rule (§5.3). |
| `emd_supervisor` (Phase 1) / `emd_cam` (Phase 2) | Allocate MB grid arena at camera start. Branch on `mb_layer_parse` flag. Build `emd_inspector_input_t` from MB grid when available, fall back to byte-count fields otherwise. Forward MB-grid snapshot pointer + ROI hit-map into Phase 2 event callback. |
| `emd_event` | Extend `emd_event_t` with new signal scalars and ROI hit-map summary (centroid + bbox + MB count). |
| `emd_config` | New TOML keys (§7). New validation rules (e.g. `extract_mv` requires `mb_layer_parse`). |
| `emd_metrics` | New counters and histograms (§9). |

### 4.3 Unchanged modules

`emd_net`, `emd_rtsp`, `emd_rtp`, `emd_h264_depay`, `emd_h265_depay`, `emd_ringbuf`, `emd_recorder`, `emd_mux_mpegts`, `emd_mux_fmp4`, `emd_mqtt`, `emd_log` — no changes required.

---

## 5. Inspector signal model

### 5.1 New `emd_inspector_input_t` fields

```c
typedef struct {
    /* existing fields preserved */
    uint64_t pts_90khz;
    uint64_t mono_ns;
    size_t   byte_count;
    bool     is_keyframe;
    bool     is_intra;
    double   intra_ratio_proxy;   /* deprecated semantics; see below */
    uint32_t mb_skip_run;
    int32_t  slice_qp_delta;

    /* new fields — populated when mb_grid_present == true */
    bool     mb_grid_present;
    const emd_mb_grid_snap_t *mb_grid;   /* read-only snapshot, lifetime = this call */

    /* derived per-AU scalars precomputed by supervisor for inspector convenience */
    double   intra_mb_ratio;       /* fraction of MBs decoded as intra */
    double   skip_mb_ratio;        /* fraction of MBs that are skip */
    double   activity_ratio;       /* fraction of non-skip MBs with |mv|>0 */
    double   mv_magnitude_p50;     /* median |mv| over non-skip MBs */
    double   mv_magnitude_p90;     /* 90th percentile |mv| over non-skip MBs */
    double   residual_ratio;       /* fraction of MBs with cbp != 0 */
    double   qp_mean;              /* mean of (slice_qp + mb_qp_delta) over MBs */
} emd_inspector_input_t;
```

`intra_ratio_proxy` retains its existing semantics (2.0 if AU has any intra slice, 0.5 otherwise) so existing camera configurations keep working until they migrate to `intra_mb_ratio`. The inspector will prefer `intra_mb_ratio` when `mb_grid_present` is true.

### 5.2 Per-signal EWMAs

Each of {`bytes_per_frame`, `intra_mb_ratio`, `activity_ratio`, `mv_magnitude_p90`, `residual_ratio`, `qp_mean`} gets a fast/slow EWMA pair, mirroring the existing `bpf_ewma` / `bpf_slow`. Welford variance is maintained for each. Total inspector state grows from ≈80 bytes to ≈320 bytes per camera, well within the per-camera arena.

The `intra_mb_ratio` signal is what Laumer et al. [4] call the "intra-coded MB density"; it spikes when motion exceeds the encoder's prediction budget. `activity_ratio` and `mv_magnitude_p90` are direct ports of Szczerba et al. [5]'s per-MB MV thresholding plus their p-percentile aggregation. `residual_ratio` is the cheapest proxy for Sabirin & Kim [6]'s "non-zero residual presence". `qp_mean` is included because a downward QP excursion (encoder lowering QP to absorb a bitrate spike) is corroboratory evidence; this is Laumer et al. [4]'s syntax-only motion weight in its purest form.

### 5.3 Combine rule

The existing rule (`z_bytes > 3.0 OR intra_ratio > 2.5 OR unexpected_idr`) becomes the v0 fallback. v1 introduces a configurable multi-signal vote:

```
signal_count = (z_bytes > thr_bytes)
             + (z_intra_mb > thr_intra)
             + (z_activity > thr_activity)
             + (z_mv_p90 > thr_mv)
             + (z_residual > thr_residual)
             + (z_qp < -thr_qp)        // downward
             + unexpected_idr

fire = signal_count >= combine_threshold
```

with `combine_threshold` configurable (default 2). Each individual signal can be disabled by setting its threshold to `+inf`, which is what feature flags expose (§7). The unexpected-IDR term remains as an unconditional disjunct because it's qualitatively different.

The intent: kill the dominant false-positive mode where a single signal spikes (e.g. IR-flicker burst inflates byte count without any MB activity) by requiring corroboration. Autotune already searches a per-camera threshold space and will pick this up.

### 5.4 Backward compatibility

A camera with `mb_layer_parse = false` is byte-equivalent to the existing detector. CI runs the existing IT-01…IT-18 acceptance suite with `mb_layer_parse = false` globally and these must continue to pass with zero behaviour change.

---

## 6. Spatial filter

The MB grid produced per AU is a `mb_width × mb_height` array of `emd_mb_record_t`. The spatial filter converts this into a binary "interesting MB" map per AU and aggregates across frames into a stable ROI hit-map.

### 6.1 Per-MB activity score

For each MB, compute a scalar `s ∈ [0, 1]` as a weighted combination of:

- `is_skip ? 0 : 1` (base activity)
- `min(|mv|, mv_clip) / mv_clip` (normalised MV magnitude, with `mv_clip` configurable, default 16 quarter-pel units)
- `is_intra ? intra_weight : 0` (intra MB bonus; intra MBs typically mean the encoder gave up predicting)
- `has_residual ? residual_weight : 0`

Weights are configurable (§7) but default to a Laumer [4]-style "syntax-only" weighting: skip=0, MV-derived 60%, intra 30%, residual 10%.

### 6.2 Temporal accumulator

Per camera, maintain a `uint8_t[mb_width × mb_height]` accumulator. Each AU:

```
acc[i] = clamp(acc[i] * decay + s[i] * gain, 0, 255)
```

with `decay ≈ 0.85` and `gain ≈ 64` as defaults; tunable. This is the PSMF [3] temporal accumulator in fixed-point integer form. The fixed-point arithmetic is critical for the CPU budget: ~8000 byte-ops per AU at 1080p is negligible.

### 6.3 Morphological cleanup

The thresholded binary map (`acc[i] > thr` where `thr` defaults to 128) is passed through a 3×3 morphological open (erosion then dilation) to drop isolated MBs. The implementation is bit-packed: the binary map is held as a packed `uint64_t[ceil(mb_width/64) × mb_height]` and erosion is a shift+AND across neighbours. For 1080p, the entire open operation is ≈2000 uint64 ops per frame — single-digit microseconds.

### 6.4 Connected components

Two-pass union-find labelling on the cleaned binary map produces connected components. Components below `min_blob_mbs` (default 8) are dropped. Surviving components are emitted with:

- Bounding box in MB units (`mb_x_min, mb_y_min, mb_x_max, mb_y_max`)
- Centroid in MB units
- MB count
- Mean MV vector across the component

This is the per-event ROI hit-map. Up to 8 components per event in v0; if more, keep the top 8 by MB count. The hit-map is part of `emd_event_t` (§4.2).

### 6.5 Event firing with spatial info

When `spatial.enabled == true` and `mb_layer_parse == true`, the inspector's `fire` condition is augmented:

```
fire = combine_rule_fire AND (largest_component_size >= min_blob_mbs)
```

i.e., we additionally require at least one non-trivial connected component. This is what eliminates "everything moved a tiny bit" false positives (panning false-alarm, encoder hiccup, single-MB MV outlier).

The connected-component requirement can be relaxed for `unexpected_idr` events (they're qualitatively spatial-information-poor by construction) — these still fire even with no component, so we don't drop genuine recovery-from-loss events.

---

## 7. Feature flag scheme

All MB-parse features are gated by both build-time and runtime flags. The build-time flag exists so resource-constrained deployments can ship a smaller binary without the parser code at all.

### 7.1 Build-time (CMake)

Added to `components/emd/CMakeLists.txt`:

```cmake
option(EMD_ENABLE_MB_PARSE "Compile MB-layer parser, CABAC engine, spatial filter" ON)
option(EMD_ENABLE_MB_PARSE_H265 "Compile H.265 MB-layer parser (future)" OFF)

if(EMD_ENABLE_MB_PARSE)
    target_compile_definitions(emd PRIVATE EMD_HAVE_MB_PARSE=1)
    target_sources(emd PRIVATE
        src/emd_cabac.c
        src/emd_h264_mb.c
        src/emd_h264_mb_cabac.c
        src/emd_h264_mb_cavlc.c
        src/emd_h264_mv_predict.c
        src/emd_mb_grid.c
        src/emd_spatial.c)
endif()
```

When `EMD_ENABLE_MB_PARSE` is OFF, none of the new symbols exist; the supervisor's MB-parse branch compiles to a no-op via `#ifdef EMD_HAVE_MB_PARSE`. All runtime config keys under the `parser` and `spatial` sections are then ignored with a warning at config-load time.

The default CI matrix builds both ON and OFF and runs the IT-01…IT-18 suite on each.

### 7.2 Runtime (TOML, per camera)

```toml
[cameras.cam_north_front.parser]
mb_layer_parse       = true       # master toggle, default: false
codec_h264_cabac     = true       # default: true if mb_layer_parse
codec_h264_cavlc     = true       # default: true if mb_layer_parse
extract_mv           = true       # default: true if mb_layer_parse
extract_cbp          = true       # default: true if mb_layer_parse
extract_qp           = true       # default: true if mb_layer_parse
max_resolution_mbs   = 8160       # 1080p; refuse to parse larger frames in v0
fail_open            = true       # on parse error, fall back to byte-count for that AU
parser_tier          = "v0"       # named tier so future "v1_with_8x8_intra" etc. can ship

[cameras.cam_north_front.inspector]
# multi-signal combine
combine_threshold        = 2       # default: 1 (i.e. preserve OR semantics)
signal_bytes_z           = true    # default: true
signal_intra_mb_ratio    = true    # default: true if mb_layer_parse
signal_activity_ratio    = true    # default: false (opt-in; needs autotune)
signal_mv_p90            = false   # default: false
signal_residual_ratio    = false   # default: false
signal_qp_drop           = false   # default: false

thr_bytes_z              = 3.0
thr_intra_mb_z           = 3.0
thr_activity_z           = 3.0
thr_mv_p90_z             = 3.0
thr_residual_z           = 3.0
thr_qp_drop              = 6.0     # |delta QP| not z-score

[cameras.cam_north_front.spatial]
enabled              = false      # default: false
min_blob_mbs         = 8
morph_open_3x3       = true
temporal_decay       = 0.85
temporal_gain        = 64
intra_weight         = 0.3
residual_weight      = 0.1
mv_clip_qpel         = 16
```

### 7.3 Default profiles

To avoid burdening operators with the flag matrix, ship three named profiles selectable via `[cameras.X] profile = "..."`:

| Profile | `mb_layer_parse` | `combine_threshold` | Spatial filter | Signals enabled |
|---|---|---|---|---|
| `byte_count` (default) | false | 1 | off | bytes_z + intra_ratio (legacy) |
| `mb_basic` | true | 2 | off | bytes_z + intra_mb_ratio + activity_ratio |
| `mb_full` | true | 2 | on | bytes_z + intra_mb_ratio + activity_ratio + mv_p90 + residual_ratio |

Explicit per-key overrides in TOML override the profile selection.

### 7.4 Compatibility matrix and validation

At config load time, `emd_config` validates:

- `extract_mv` requires `mb_layer_parse`
- `extract_cbp` requires `mb_layer_parse`
- `signal_activity_ratio` / `signal_mv_p90` / `signal_residual_ratio` require `mb_layer_parse`
- `spatial.enabled` requires `mb_layer_parse` (since the input is the MB grid)
- `combine_threshold` ≤ number of enabled signals + 1 (the unexpected-IDR disjunct)
- `max_resolution_mbs` must accommodate the largest stream dimensions seen in SPS, else parser refuses

Invalid combinations are a hard error at startup, not silent degradation.

---

## 8. Detailed task breakdown

The following is a sequenced, time-estimated task list. Estimates assume one full-time C engineer with H.264 spec familiarity; calendar time will differ.

### 8.1 Task list

| # | Task | Files | Effort | Depends on |
|---|---|---|---|---|
| T1 | Extend SPS / PPS / slice-header parse to all High Profile fields | `emd_h264_parse.{c,h}` | 3 d | — |
| T2 | Arithmetic decoder engine (`emd_cabac`) | `emd_cabac.{c,h}` | 5 d | — |
| T3 | CABAC context init tables (generator + committed header) | `scripts/gen_cabac_tables.py`, `emd_cabac_ctx_init.h` | 3 d | T2 |
| T4 | `emd_mb_grid` arena allocator + record format | `emd_mb_grid.{c,h}` | 2 d | — |
| T5 | MV median predictor | `emd_h264_mv_predict.{c,h}` | 2 d | T4 |
| T6 | MB-layer CABAC syntax decode (skip, type, MV, CBP, QP) | `emd_h264_mb_cabac.c` | 7 d | T2, T3, T4, T5 |
| T7 | MB-layer CAVLC syntax decode (parity for Baseline/Main) | `emd_h264_mb_cavlc.c` | 4 d | T4, T5 |
| T8 | MB-layer dispatcher and refusal logic | `emd_h264_mb.{c,h}` | 1 d | T6, T7 |
| T9 | Golden-output testing vs ffmpeg `-export_mvs` | `tests/test_mb_golden.c`, fixtures | 5 d | T8 |
| T10 | Inspector enrichment: new EWMAs, multi-signal combine | `emd_inspector.{c,h}` | 3 d | T8 |
| T11 | Spatial filter: temporal accumulator, morph, components | `emd_spatial.{c,h}` | 3 d | T8 |
| T12 | Supervisor wiring + arena allocation + fail-open path | `emd_supervisor.c`, `emd_cam.c` | 3 d | T8, T10 |
| T13 | Config schema, validation, profile presets | `emd_config.{c,h}`, sample TOMLs | 2 d | T10, T11 |
| T14 | Metrics, event schema extension | `emd_metrics.{c,h}`, `emd_event.{c,h}` | 2 d | T10 |
| T15 | CMake build flag + dual CI matrix | `CMakeLists.txt`, `.github/workflows/` | 1 d | — |
| T16 | Performance budget verification + tuning | (across modules) | 5 d | T12, T14 |
| T17 | Fuzz harness over malformed bitstreams | `tests/fuzz_cabac.c` | 3 d | T2 |
| T18 | Sanitizer matrix (ASan/UBSan/TSan/MSan) green | (build configs) | 2 d | T16 |
| T19 | Phase 2 ABI extension to forward MB grid + ROI to Go | `agent_abi.h`, `emd_cam.c` | 3 d | T12 |
| T20 | Documentation update (this spec + CLAUDE.md + README) | docs/ | 1 d | all |
|   | **Total** | | **≈58 d** | |

Critical path: T1 → T2 → T3 → T6 → T8 → T9 → T16 → release. ≈30 working days end-to-end if other tasks run in parallel.

### 8.2 Milestones

- **M1 (week 2):** T1, T2, T4 done. CABAC engine passes round-trip unit tests (encode then decode a known bitstring).
- **M2 (week 4):** T3, T5, T6 done. CABAC engine parses real slices and emits an MB grid.
- **M3 (week 6):** T7, T8, T9 done. Golden output matches ffmpeg on the fixture corpus.
- **M4 (week 8):** T10, T11, T12, T13, T14 done. End-to-end MB-parse detector running on a test stream behind feature flags.
- **M5 (week 10):** T15, T16, T17, T18 done. Performance budget validated, sanitizers green, fuzz harness running in CI.
- **M6 (week 11):** T19, T20 done. Phase 2 Go agent forwards ROI hit-map.

### 8.3 Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| CABAC context init off-by-one (silent corruption) | high | high | Golden-output test (T9) against ffmpeg as canonical reference; mismatch fails CI. |
| CPU budget exceeded at 1080p30 | medium | high | Early benchmarking (T16); coefficient-skip optimization (§3.4); per-camera opt-in keeps blast radius small. |
| Malformed bitstream crashes parser | medium | high | Fuzz harness (T17); ASan in CI; fail-open semantics for all parse errors. |
| Encoder uses syntax we refused (FMO etc.) | low | low | Refusal counter (§9); falls back to byte-count detector; alerts operator. |
| Spec table transcription errors | medium | medium | Auto-generate tables from spec source where possible (T3); manual table review by second engineer. |
| ffmpeg's `-export_mvs` is itself incorrect | low | medium | Cross-check on at least one stream with a second reference (JM reference decoder, [8] tools). |
| MBAFF stream slips through profile check | low | medium | Explicit refusal at SPS parse; e2e test with a known MBAFF fixture. |

---

## 9. Metrics

New Prometheus metrics added under existing `/metrics` endpoint:

- `emd_mb_parse_au_total{cam,result}` — counter. `result ∈ {ok, refused_profile, refused_resolution, error, disabled}`.
- `emd_mb_parse_ns{cam}` — histogram. Per-AU parse time in nanoseconds. Buckets: 10us, 50us, 100us, 500us, 1ms, 5ms, 10ms.
- `emd_mb_parse_bins_total{cam}` — counter. Total CABAC bins decoded.
- `emd_inspector_signal_fire_total{cam,signal}` — counter. `signal ∈ {bytes_z, intra_mb_z, activity_z, mv_p90_z, residual_z, qp_drop, unexpected_idr}`.
- `emd_inspector_combine_fire_total{cam}` — counter. Times the multi-signal combine threshold was met.
- `emd_spatial_components_per_event{cam}` — histogram. Number of connected components per fired event. Buckets: 0, 1, 2, 4, 8, 16+.
- `emd_spatial_largest_blob_mbs{cam}` — histogram. Size in MBs of the largest blob per event.

The existing `emd_event_*` metrics keep their semantics; they continue to count events as published, regardless of which detector path fired.

---

## 10. Performance budget

### 10.1 Target

≤30% of one Cortex-A76 vCPU at 2.4 GHz per 1080p30 camera with `mb_full` profile (all parse + spatial features enabled). At 30% × 2.4 GHz = 720 Mcycle/s, divided across 30 fps = 24 Mcycle/frame. A 1080p frame is 8160 MBs, so the per-MB budget is ≈2940 cycles ≈1.2µs.

### 10.2 Estimated cost per MB

The dominant cost components, in approximate per-MB cycles for a typical inter MB at moderate QP:

| Component | Cycles | Notes |
|---|---|---|
| CABAC bins (≈80 bins/MB average, ≈25 cycles/bin) | ≈2000 | The bulk; see §3.4 for skipping coefficient values. Coefficient bins are still consumed, just not stored. |
| MV predictor | ≈100 | Median of 3 candidates from neighbour MVs. |
| Grid write | ≈50 | One `emd_mb_record_t` (≈48 bytes) per MB. |
| QP tracking | ≈20 | Single add + clamp. |
| Per-MB activity score | ≈40 | Used by spatial filter; bypassable if spatial off. |
| Subtotal | **≈2210** | Within budget. |

Intra MBs are ≈30% cheaper (no MVs); skip MBs are dramatically cheaper (≈100 cycles total). The 2210 estimate is for the dominant inter case; weighted average across a typical frame is ≈1500 cycles/MB, leaving substantial headroom.

### 10.3 Per-frame overhead

| Component | Cycles per frame | Notes |
|---|---|---|
| Slice header parse | ≈5000 | One-time per slice. |
| Inspector EWMA updates (6 signals × ~50 cycles) | ≈300 | Once per AU. |
| Spatial: per-MB activity (8160 MBs × 40 cy) | ≈330k | Already counted above. |
| Spatial: temporal accumulator (8160 byte-ops) | ≈25k | Trivial. |
| Spatial: morph 3x3 (≈2000 uint64 ops × 4 cy) | ≈8k | Bit-packed open. |
| Spatial: connected components (worst case) | ≈100k | Two-pass union-find on 8160 nodes. |
| Frame overhead total | ≈140k | ≈0.6% of frame budget. |

### 10.4 Measurement plan (T16)

- Microbenchmark `emd_cabac` over a corpus of known bin streams. Target: ≥40 Mbin/s on the reference CPU. Failure: optimize hot loop (renormalization + range LUT — see [7]'s FFmpeg integration for proven patterns).
- Per-AU parse-time histogram from `emd_mb_parse_ns` over the fixture corpus and a 1-hour real-camera capture. Validate p99 ≤ 8 ms (24% of a 33 ms frame budget).
- Spin up 4 concurrent 1080p30 cameras on a Raspberry Pi 5 and verify aggregate CPU < 100%.

If targets are missed, mitigations in priority order: (1) SIMD-optimize the bypass-bin path (≈30% of bins are bypass); (2) batch-process skip-MB runs without per-MB context switching; (3) defer `qp_mean` computation outside the parse loop; (4) consider Neon intrinsics for the morph filter.

---

## 11. Testing strategy

### 11.1 Unit tests (cmocka, per-module)

- `test_cabac_engine.c` — encode → decode round-trip of bit sequences; specific binarization patterns from H.264 spec examples; renormalization edge cases (range underflow, offset overflow).
- `test_cabac_ctx_init.c` — hash-check of the context init tables against a committed reference hash; catches accidental table modification.
- `test_h264_mv_predict.c` — H.264 spec examples for median predictor across MB neighbour configurations (left edge, top edge, top-right unavailable, etc.).
- `test_mb_grid.c` — arena allocation, snapshot consistency, capacity exhaustion.
- `test_spatial.c` — temporal accumulator monotonicity, morph open correctness on hand-coded patterns, connected-component labelling on known shapes.

### 11.2 Golden-output integration tests (T9)

A corpus of ≥20 short clips covers:

- High Profile + CABAC, P slices, transform 8x8 off
- High Profile + CABAC, P + B slices, transform 8x8 on
- Main Profile + CABAC, P slices
- Baseline + CAVLC, P slices
- Constrained Baseline + CAVLC
- IDR-heavy streams (low GOP size)
- Long-GOP streams (90+ frames)
- Various resolutions: 720p, 1080p, 1440p, 4K (4K should refuse if `max_resolution_mbs` lower)
- One MBAFF stream (must refuse)
- One field-coded stream (must refuse)
- Two known-tricky encoders (e.g. one Hikvision, one Dahua, one Axis if available)

For each clip:

```
ffmpeg -flags2 +export_mvs -i clip.h264 -vf 'codecview=mv=pf+bf+bb' -f rawvideo /dev/null \
    2> ffmpeg_mv_dump.txt
emd_mb_dump clip.h264 > emd_mv_dump.txt
diff_mv_dumps ffmpeg_mv_dump.txt emd_mv_dump.txt --tolerance 0
```

Tolerance is zero for MV values (they're integers in quarter-pel units; exact match required). MB type comparison is tolerant of equivalent partition encodings (e.g. P_L0_16x16 vs P_Skip with the same effective MV are equivalent for our purposes). Documented in the diff tool.

### 11.3 End-to-end test extension

`scripts/e2e_test.sh` already generates fixture streams and runs the binary. Extend with a `--mb-parse` mode that enables the `mb_full` profile and adds an `--expect-roi-hits` assertion: the synthetic motion fixture should produce a connected component overlapping the known motion region.

### 11.4 Fuzz testing (T17)

`tests/fuzz_cabac.c` is a libFuzzer / AFL harness:

```c
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    emd_h264_param_cache_t cache;
    emd_h264_param_cache_init(&cache);
    /* prepend a known-good SPS/PPS so the fuzzer focuses on slice data */
    inject_known_sps_pps(&cache);
    emd_mb_grid_t grid;
    emd_mb_grid_init(&grid, /* dims from injected SPS */);
    (void)emd_h264_parse_slice_data(data, size, &cache, &grid);
    emd_mb_grid_free(&grid);
    return 0;
}
```

Must not crash, not read out-of-bounds (ASan), not produce uninitialised reads (MSan), not race (TSan). Run continuously in CI for at least 24 CPU-hours before a v1 tag.

### 11.5 Sanitizer matrix

The existing CLAUDE.md ASan / UBSan / TSan / MSan builds are extended with a `-DEMD_ENABLE_MB_PARSE=ON` variant and a `=OFF` variant. Both must pass `ctest` for every PR.

### 11.6 Acceptance test scenarios (IT-19…IT-25)

New scenarios extending the Phase 1 IT-01…IT-18 suite:

- **IT-19** Existing `byte_count` profile produces identical events to Phase 1 baseline (regression guard).
- **IT-20** `mb_basic` profile on a known-good stream produces an event with `activity_ratio > 0.3` for a synthetic moving-object fixture.
- **IT-21** `mb_full` profile produces a ROI hit-map whose largest component overlaps the synthetic motion region by ≥50% of MBs.
- **IT-22** IR-flicker fixture (bytes spike, no MV activity) fires the byte_count detector but does **not** fire `mb_full`. Demonstrates false-positive suppression.
- **IT-23** MBAFF fixture: parser refuses, byte-count detector continues, `emd_mb_parse_au_total{result="refused_profile"}` increments.
- **IT-24** CABAC vs CAVLC fixtures produce equivalent MV grids (modulo encoder differences) for the same source content.
- **IT-25** Fuzz-derived corpus replays without crash or grid corruption.

---

## 12. Rollout plan

### 12.1 Versions

- **v0.3.0** — Phase 3 alpha. `EMD_ENABLE_MB_PARSE` defaults ON; all per-camera flags default to `byte_count` profile. Opt-in early adopters.
- **v0.4.0** — Phase 3 beta. Default profile becomes `mb_basic` for newly added cameras; existing cameras retain `byte_count`. Autotune learns thresholds for the new signals on opted-in cameras.
- **v0.5.0** — Phase 3 GA. Documentation, deployment guide, migration tooling for existing cameras.
- **v1.0.0** — Default profile becomes `mb_full` for new cameras. Existing cameras migrated on next config reload.

Each step gated on the autotune component demonstrating non-regression of precision/recall on the canonical evaluation set.

### 12.2 Migration

For existing camera configs, no action required: omitting the new TOML sections preserves `byte_count` profile. An optional `migrate-cameras.sh` script bumps a TOML file's cameras to `mb_basic`.

### 12.3 Feature flag retirement

`EMD_ENABLE_MB_PARSE` remains a build-time toggle indefinitely so resource-constrained ports can disable it. The runtime `mb_layer_parse = false` remains supported indefinitely because it's the path used by streams the parser refuses (MBAFF etc.); we can't remove it without removing fail-open.

---

## 13. Document & reference hygiene

The eight papers and code repositories in §2 are downloaded as PDFs / repo snapshots and committed to `docs/references/` so they survive link rot and so the codebase remains reviewable offline. The `docs/references/README.md` indexes them with the same `[1]`–`[8]` tags used throughout this spec.

The `gen_cabac_tables.py` script in `scripts/` is the single source of truth for the CABAC initialization tables; it consumes the spec PDF page numbers as input. Re-running it requires the H.264 spec PDF in `third_party/h264_spec/` (not committed; licensed; engineers fetch from ITU). The generated `emd_cabac_ctx_init.h` is committed.

This spec itself is referenced from `CLAUDE.md` (top-level "What's new in Phase 3" section to be added on first PR landing).

---

## 14. Open questions for review

1. Should `mb_full` be the default profile for `v1.0.0`, or do we want to keep `mb_basic` as default and require explicit opt-in to spatial filtering? The trade-off is CPU budget on the smallest deployment hardware.
2. The 2210-cycle/MB estimate in §10 is preliminary. Should we require a working microbenchmark of `emd_cabac` before committing to T6 effort? (Recommended: yes — schedule a 2-day prototype before M2.)
3. ffmpeg's MV export format has changed across versions. Which ffmpeg version do we pin as the golden reference? Proposal: ffmpeg 6.1 LTS.
4. Phase 2's `emd_cam` ABI passes the MB grid snapshot to Go via cgo. Should the Go side get a zero-copy view (lifetime contract: valid for callback duration) or a copy? The zero-copy path is faster but harder to get right; proposal: zero-copy with explicit lifetime tag, copy on the Go side only if the Go consumer needs it.
5. Should we expose per-MB grid data via a metrics endpoint for debugging? Probably yes via a debug-only path, but rate-limited; this is the kind of feature that's irreplaceable when debugging in production.

These are tracked as comments and resolved before v0.4.0.

---

**End of document.**
