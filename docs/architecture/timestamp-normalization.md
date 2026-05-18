# MPEG-TS Timestamp Normalization

## Issue
Security camera RTSP streams use absolute PTS (Presentation Time Stamps) that represent the actual camera's clock time. For example, a camera might output PTS values starting at `20056881978` (90kHz units), representing thousands of seconds since the camera booted or its epoch.

HLS.js and many browser-based media players cannot handle MPEG-TS files with such large timestamp values. They expect timestamps to start near zero for proper playback.

## Solution Implemented
The MPEG-TS muxer (`src/emd_mux_mpegts.c`) now normalizes timestamps:

1. **Capture First PTS**: On the first frame written to a clip, we record `first_pts`
2. **Normalize All Timestamps**: All subsequent PTS and DTS values have `first_pts` subtracted:
   ```c
   uint64_t normalized_pts = pts - first_pts;
   uint64_t normalized_dts = dts - first_pts;
   ```
3. **Result**: Clips now start with timestamps near zero (typically 0-2 seconds)

## Code Changes
```c
typedef struct {
    // ... existing fields
    uint64_t first_pts;      /* First PTS for timestamp normalization */
    bool     pts_initialized;
} mpegts_ctx_t;
```

In `mpegts_write_nal()`:
```c
if (!ctx->pts_initialized) {
    ctx->first_pts = pts;
    ctx->pts_initialized = true;
}

uint64_t normalized_pts = pts - ctx->first_pts;
uint64_t normalized_dts = dts - ctx->first_pts;
```

## Important Consequences

### ⚠️ Loss of Original Timing Information
By normalizing timestamps, we **lose the original camera timestamp** that indicates when the event actually occurred in camera time. This has implications:

1. **No absolute time reference**: Cannot determine actual wall-clock time from PTS alone
2. **Cross-camera sync**: Cannot synchronize clips from multiple cameras using PTS
3. **Forensic analysis**: Cannot correlate with external events using timestamps

### Mitigation Strategies

To preserve original timing information, consider:

1. **Filename Timestamps**: The clip filename already includes ISO8601 timestamp:
   ```
   axis_81_8_20260518_005840_01KRW9F7.mpegts
              ^^^^^^^^ ^^^^^^
              YYYYMMDD HHMMSS
   ```

2. **Metadata Sidecar**: Store original timing in a separate file:
   ```json
   {
     "clip": "axis_81_8_20260518_005840_01KRW9F7.mpegts",
     "first_pts_90khz": 1805119378,
     "camera_epoch": "2026-05-18T00:58:40Z",
     "duration_sec": 16.0
   }
   ```

3. **Extended Filename**: Encode first_pts in the filename:
   ```
   axis_81_8_20260518_005840_pts1805119378.mpegts
   ```

4. **MPEG-TS Private Data**: Use MPEG-TS private stream to carry original timestamps
   - Add a private PID (e.g., 0x102) with original timing metadata
   - Write once per clip with original PTS, camera ID, etc.

5. **Database/Index**: Maintain external index:
   ```sql
   CREATE TABLE clip_timing (
       clip_id TEXT PRIMARY KEY,
       camera_id TEXT,
       recording_start_utc TIMESTAMP,
       first_pts_90khz BIGINT,
       duration_sec REAL
   );
   ```

## Recommendations

For the current MVP phase, the **filename timestamp** is sufficient. For production:

1. **Add metadata sidecar files** (.json) alongside each .mpegts clip
2. **Preserve in MQTT notifications** - include original PTS in event payloads
3. **Index in database** - if/when adding clip management features

## Testing

Verify normalized timestamps:
```bash
ffprobe clip.mpegts 2>&1 | grep start_time
# Should show: start_time=0.000000 or start_time=1.400000 (small values)
```

Compare with unnormalized:
```bash
# Original camera clip (won't play in browser)
start_time=20056.881978

# Normalized clip (plays in browser via HLS.js)
start_time=1.400000
```

## References
- ISO 13818-1 (MPEG-TS specification)
- HLS RFC 8216
- hls.js documentation: https://github.com/video-dev/hls.js/
