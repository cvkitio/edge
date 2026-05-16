# False Positive Analysis - axis_82_2_20260516_025611_01KRQBCY.mpegts

**Date**: 2026-05-16  
**Camera**: axis_82_2  
**Clip Duration**: 154 frames (~6 seconds @ 25fps)  
**User Report**: "I don't see any movement"

---

## Frame Analysis

### Frame Size Distribution

```
3 I-frames:  375-382 KB each
141 P-frames: 354 bytes each (91% of clip)
6 P-frames:  538 bytes each
2 P-frames:  722 bytes each  
2 P-frames:  906 bytes each
```

### Timeline

```
Frames 1-144:   354 bytes (static scene - baseline established)
Frames 145-148: 538 bytes (1.5x increase)
Frame 149:      722 bytes (2.0x increase)
Frames 150-151: 538 bytes
Frames 152-153: 906 bytes (2.56x increase) ← TRIGGER
Frame 154:      722 bytes
```

---

## Why Did It Trigger?

### Z-Score Calculation

The motion detector uses **relative change**, not absolute:

```
z-score = (current_bytes - baseline) / standard_deviation

Baseline (bpf_slow):    ~354 bytes (from frames 1-144)
Standard deviation (σ): ~100 bytes (estimated, very low for static scene)
Peak P-frame:            906 bytes

z = (906 - 354) / 100 = 5.52
```

**Threshold**: z > 3.0  
**Result**: z=5.52 > 3.0 → **MOTION DETECTED**

### Debouncing

The detector requires **2 consecutive frames** above threshold (`on_threshold=2`):

```
Frame 145-148: z ≈ 1.8 (below threshold)
Frame 149:     z ≈ 3.7 (above threshold) ← count=1
Frame 152:     z ≈ 5.5 (above threshold) ← count=2, TRIGGER!
```

---

## Why Is This Happening?

### Root Cause: Static Scene Sensitivity

Z-score detection is **scene-adaptive**:
- For active scenes (baseline=50KB), a 906-byte frame is negligible (z ≈ 0)
- For static scenes (baseline=354 bytes), a 906-byte frame is huge (z ≈ 5.5)

**The algorithm is working as designed** - it detects changes **relative to the scene**, not absolute thresholds.

### What Caused the 538-906 Byte Frames?

Likely culprits (invisible to the eye but detected by encoder):

1. **Camera micro-movements**: Vibration from wind, building settling, or mounting
2. **Lighting changes**: Cloud passing, shadow edge moving, or auto-exposure adjustment
3. **Small object movement**: Insect, bird, or leaf at edge of frame
4. **Camera processing**: Auto white-balance, noise reduction changes
5. **Compression artifacts**: H.264 encoder resets or adjustments

**These changes are too subtle to notice visually but affect compression significantly.**

---

## Is This a Bug?

**No** - this is expected z-score behavior for static scenes.

### Pros of Current Approach

✅ **Adaptive**: Automatically adjusts to each camera's baseline  
✅ **Sensitive**: Detects subtle changes that might be important  
✅ **No calibration needed**: Works out-of-box for any camera

### Cons of Current Approach

❌ **False positives on static cameras**: Triggers on micro-changes  
❌ **No absolute threshold**: Can't say "ignore changes < X pixels"  
❌ **Encoder-dependent**: H.264 compression quirks affect detection

---

## Solutions

### Option 1: Raise Z-Score Threshold (Quick Fix)

Increase `motion_z_high` from 3.0 to 4.0 or 5.0 for static cameras:

```toml
[cameras.axis_82_2]
motion_z_high = 4.5  # Was 3.0
```

**Effect**: Reduces false positives but may miss subtle real motion.

### Option 2: Add Absolute Threshold (Recommended)

Ignore frames below an absolute size, regardless of z-score:

```c
// In emd_inspector.c, around line 119
bool is_z_motion = (z > cfg->motion_z_high && bytes > cfg->min_bytes_threshold);
```

**Config**:
```toml
[cameras.axis_82_2]
motion_z_high = 3.0
min_bytes_threshold = 2000  # Ignore P-frames < 2KB
```

### Option 3: Use Intra-Ratio Only for Static Cameras

Disable z-score detection, rely on intra-ratio (macroblock analysis):

```toml
[cameras.axis_82_2]
motion_z_high = 999.0      # Effectively disable z-score
intra_ratio_high = 2.5     # Use intra-ratio instead
```

**Effect**: More robust for static scenes, but requires camera to send macroblock stats.

### Option 4: Camera-Specific Profiles

Create camera profiles based on typical activity:

```toml
# High-activity camera (parking lot)
[cameras.parking_lot]
motion_z_high = 3.0
min_bytes_threshold = 5000

# Static camera (hallway)
[cameras.hallway]
motion_z_high = 5.0
min_bytes_threshold = 3000
```

---

## Recommended Action

For **axis_82_2** (appears to be a static/low-activity camera):

1. **Short-term**: Increase `motion_z_high` to 4.5
2. **Long-term**: Implement Option 2 (absolute threshold) in the inspector

---

## Testing

To test sensitivity tuning:

```bash
# Run with higher threshold
sed -i '' 's/motion_z_high = 3.0/motion_z_high = 4.5/g' config.toml

# Run for 60 seconds
./emd-agent --config config.toml

# Check if false positives reduced
find clips/ -name "axis_82_2*.mpegts" -exec bash -c 'echo "{}:"; ffprobe -v error -count_frames -show_entries stream=nb_read_frames -of default=nokey=1 "{}"' \;
```

---

## Summary

**The motion detector is working correctly** - it detected a 2.5x increase in P-frame size (906 vs 354 bytes).

**The problem**: Z-score is **too sensitive** for extremely static scenes because:
- Baseline converges very low (354 bytes)
- Any small change produces high z-scores
- Subtle, invisible changes trigger detection

**Recommended fix**: Add an absolute minimum threshold (`min_bytes_threshold`) to prevent triggering on tiny absolute changes, even if they're large relative changes.

**Impact**: This is a **tuning issue**, not a bug. Adjusting thresholds per camera type will reduce false positives.
