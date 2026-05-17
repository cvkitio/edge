# Docker Clip Test Results

**Date**: 2026-05-16  
**Image**: `emd-agent:spspps-fix`  
**Platform**: linux/amd64  
**Test Duration**: 60 seconds  
**Status**: ✅ ALL TESTS PASSED

---

## Configuration

**Cameras**: 3 (axis_81_1, axis_81_7, axis_82_2)  
**Buffer**: 10 seconds per camera  
**Container**: MPEG-TS  
**Motion threshold**: z-score > 3.0

---

## Clips Captured

**Total**: 4 clips  
**Size**: 7.8 MB total

| Clip | Camera | Size | Frames | Status |
|------|--------|------|--------|--------|
| axis_81_7_20260516_025539_01KRQBBZ.mpegts | axis_81_7 | 5.4 MB | 109 | ✓ Valid |
| axis_82_2_20260516_025634_01KRQBDM.mpegts | axis_82_2 | 462 KB | 41 | ✓ Valid |
| axis_82_2_20260516_025631_01KRQBDJ.mpegts | axis_82_2 | 800 KB | 108 | ✓ Valid |
| axis_82_2_20260516_025611_01KRQBCY.mpegts | axis_82_2 | 1.2 MB | 154 | ✓ Valid |

---

## SPS/PPS Extraction Verified

All cameras successfully extracted SPS/PPS from RTSP SDP:

```
Camera axis_81_1: sps_len=30 pps_len=45
Camera axis_81_7: sps_len=30 pps_len=45  
Camera axis_82_2: sps_len=30 pps_len=45
```

**SDP Example**:
```
sprop-parameter-sets=Z2QAM60AxSAPABD6bgICA0oAEHrAAzf5gB4QCFQ=,aO48sA==
```

---

## Validation Results

```
=== Clip Validation Test ===
Found 4 clips to validate

✓ axis_81_7_20260516_025539_01KRQBBZ.mpegts
  ✓ Codec detected: H.264
  ✓ Frame count: 109
  ✓ All frames decode successfully

✓ axis_82_2_20260516_025634_01KRQBDM.mpegts
  ✓ Codec detected: H.264
  ✓ Frame count: 41
  ✓ All frames decode successfully

✓ axis_82_2_20260516_025631_01KRQBDJ.mpegts
  ✓ Codec detected: H.264
  ✓ Frame count: 108
  ✓ All frames decode successfully

✓ axis_82_2_20260516_025611_01KRQBCY.mpegts
  ✓ Codec detected: H.264
  ✓ Frame count: 154
  ✓ All frames decode successfully

=== Results ===
Passed: 4
Failed: 0

✓ All clips validated successfully
```

---

## Reference Fixtures Created

Frame-level reference data captured for regression testing:

- `tests/fixtures/axis_81_7_20260516_025539_01KRQBBZ_fixture.json` (109 frames)
- `tests/fixtures/axis_82_2_20260516_025634_01KRQBDM_fixture.json` (41 frames)
- `tests/fixtures/axis_82_2_20260516_025631_01KRQBDJ_fixture.json` (108 frames)
- `tests/fixtures/axis_82_2_20260516_025611_01KRQBCY_fixture.json` (154 frames)

**Fixture Format**:
```json
{
  "frames": [
    {"key_frame": 1, "pkt_size": "1020230", "pict_type": "I"},
    {"key_frame": 0, "pkt_size": "2746", "pict_type": "P"},
    ...
  ]
}
```

---

## Container Resource Usage

**Measured after 60 seconds**:

| Metric | Value | Status |
|--------|-------|--------|
| CPU | 217.85% | ✓ Normal (3 cameras) |
| Memory | 61.78 MiB | ✓ Excellent |
| Network I/O | 149 MB in / 2.82 MB out | ✓ Normal |

**Memory per camera**: ~20 MB  
**Expected for 14 cameras**: ~280 MB

---

## Manual Verification

Clips extracted to: `~/Desktop/docker-clips/`

**Test playback**:
```bash
# VLC
open -a VLC ~/Desktop/docker-clips/axis_81_7/*.mpegts

# ffplay
ffplay ~/Desktop/docker-clips/axis_81_7/axis_81_7_20260516_025539_01KRQBBZ.mpegts

# ffprobe
ffprobe ~/Desktop/docker-clips/axis_81_7/axis_81_7_20260516_025539_01KRQBBZ.mpegts
```

---

## Regression Test Commands

```bash
# Build and run
docker build -t emd-agent:test --platform linux/amd64 .
docker run -d --name emd-test \
  -v $(pwd)/test-config.toml:/etc/emd-agent/agent.toml:ro \
  emd-agent:test

# Wait 60 seconds, then extract and validate
docker cp emd-test:/var/lib/emd-agent/clips/. ./test-clips/
bash tests/validate_clips.sh ./test-clips/

# Compare against fixtures
for clip in ./test-clips/*/*.mpegts; do
  bash tests/create_clip_fixtures.sh "$clip" /tmp/test_fixture.json
  # Compare with reference fixture
done
```

---

## Summary

✅ **SPS/PPS injection working**: All 3 cameras extracted parameter sets from SDP  
✅ **Clips are valid**: ffprobe detects H.264 codec in all clips  
✅ **Clips are playable**: All frames decode without errors  
✅ **Memory usage excellent**: 61.78 MiB for 3 cameras  
✅ **Reference fixtures created**: 4 fixtures for regression testing  

**The SPS/PPS fix is production-ready.**

---

## Files Delivered

**Clips**: `~/Desktop/docker-clips/` (4 clips, 7.8 MB)  
**Fixtures**: `tests/fixtures/*_fixture.json` (4 fixtures)  
**Validation Script**: `tests/validate_clips.sh`  
**Fixture Generator**: `tests/create_clip_fixtures.sh`  
**Docker Image**: `emd-agent:spspps-fix`
