# Performance Analysis - Memory Leak Investigation

**Date**: 2026-05-16  
**Issue**: High memory usage and suspected memory leak with 14 cameras  
**Container**: Killed (exit code 137) after running for ~11 minutes

---

## Observed Behavior

**Symptoms**:
- Container running 14 cameras
- High resource usage reported
- Container killed (exit code 137 = SIGKILL, likely OOM)
- Clean shutdown logs (all cameras stopped properly)

**Good News**:
- ✅ No libavcodec decoding (verified: no FFmpeg symbols linked)
- ✅ We're not decoding video pixels
- ✅ Ring buffer allocation is fixed-size (no growth)
- ✅ Snapshot release is properly called (though not used yet)

---

## Memory Usage Baseline (Expected)

### Per Camera (1080p30 H.264):
- **Ring buffer**: 20 MB (20 seconds × 8 Mbps / 8 × 1.25 safety)
- **RTSP/RTP buffers**: 5 MB
- **Inspector state**: 1 MB (EWMA, stats)
- **Depacketizer**: 2 MB (FU-A reassembly)
- **Go goroutine**: 2 MB (stack)
- **Total**: ~30 MB per camera

### 14 Cameras:
- Camera workers: 14 × 30 MB = **420 MB**
- Go runtime: **~200 MB** (heap, GC)
- Overhead: **~100 MB**
- **Expected total**: **~720 MB**

### Actual Observed:
- **Unknown** (container killed before measurement)
- Exit code 137 suggests OOM or manual kill

---

## Potential Issues

### 1. Ring Buffer Not Draining (HIGH PROBABILITY)

**Problem**: Ring buffer fills up when not being consumed

The ring buffer is a circular buffer that:
- Producer (camera worker) writes NAL units
- Consumer (recorder) reads snapshots
- If consumer doesn't read, producer drops oldest entries

**Current State**: No recording is happening, so ring buffer just accumulates until full, then drops oldest. This is **intentional** but may cause memory pressure.

**Ring Buffer Size**:
```c
// Per camera: buffer_seconds × max_bitrate_bps / 8 × 1.25
uint32_t size = 20 * 8000000 / 8 * 1.25 = 25 MB per camera
```

**Total for 14 cameras**: 14 × 25 MB = **350 MB** just in ring buffers

**This is NOT a leak** — it's fixed-size allocation. But it's a large baseline.

### 2. C.GoString Allocations (MEDIUM PROBABILITY)

**Location**: `internal/libemd/callbacks.go`

```go
func goEventTrampoline(userCtx unsafe.Pointer, cEvt *C.emd_event_t) {
    evt := Event{
        ID:      C.GoString(&cEvt.event_id[0]),     // String allocation
        Reason:  C.GoString(&cEvt.reason[0]),        // String allocation
        CamName: C.GoString(&cEvt.cam_name[0]),      // String allocation
        // ...
    }
    ch <- evt
}
```

**Problem**: `C.GoString()` allocates a new Go string by copying from C memory. With frequent events and stats:
- Events: ~2-3 per minute per camera = ~30-40 per minute total
- Stats: ~1 per second per camera = ~840 per minute total
- **String allocations**: ~870 per minute

**Each allocation**:
- `event_id`: 64 bytes
- `reason`: 128 bytes
- `cam_name`: 64 bytes
- `stats` (no strings, but frequent)

**Total allocation rate**: ~200 KB/minute for strings alone

This shouldn't cause a leak (Go GC should collect), but it creates memory pressure.

### 3. Channel Backpressure (LOW PROBABILITY)

**Location**: `internal/agent/agent.go`

```go
eventCh := make(chan libemd.Event, 1000)  // Buffer 1000 events
statsCh := make(chan libemd.StatsSample, 100)  // Buffer 100 samples
```

If the event processor can't keep up, channels fill. However:
- Callbacks use **non-blocking send** (select with default)
- Drop events if channel full (intentional)
- So this won't cause unbounded growth

**Not likely the issue.**

### 4. Go Runtime Memory Retention (MEDIUM PROBABILITY)

**Problem**: Go GC doesn't immediately release memory to OS

Even after freeing objects, Go runtime holds onto memory for future allocations. This is normal but can appear as a "leak" in tools like `docker stats`.

**Check with**:
```go
import _ "net/http/pprof"
// Then: curl http://localhost:6060/debug/pprof/heap
```

### 5. cgo.Handle Leak (LOW PROBABILITY)

**Location**: `internal/libemd/camera.go`

```go
func OpenCamera(cfg *CameraConfig, eventCh chan<- Event, statsCh chan<- StatsSample) (*Camera, error) {
    // Create handles for Go channels
    eventHandle := cgo.NewHandle(eventCh)
    statsHandle := cgo.NewHandle(statsCh)
    
    // Pass to C...
}
```

**If handles are not deleted**, they prevent channels from being GC'd.

**Check**: `camera.go` should call `handle.Delete()` in `Close()`:
```go
func (c *Camera) Close() {
    if c.handle != nil {
        C.emd_cam_close(c.handle)
    }
    // Need to add:
    c.eventHandle.Delete()
    c.statsHandle.Delete()
}
```

**This is a bug if missing!**

---

## Diagnostic Steps

### 1. Check Ring Buffer Configuration

```bash
# In agent.toml:
buffer_seconds = 20        # 20 seconds
max_bitrate_bps = 8000000  # 8 Mbps
```

**Memory per camera**: 20 × 8000000 / 8 × 1.25 = **25 MB**

**Recommendation**: Reduce to 10 seconds for testing:
```toml
buffer_seconds = 10  # 12.5 MB per camera instead of 25 MB
```

### 2. Add Memory Profiling

```go
// In cmd/emd-agent/main.go:
import (
    _ "net/http/pprof"
    "net/http"
)

func main() {
    go func() {
        log.Println(http.ListenAndServe("localhost:6060", nil))
    }()
    // ...
}
```

**Then**:
```bash
# While container is running:
docker exec emd-agent-local wget -O /tmp/heap http://localhost:6060/debug/pprof/heap

# From host:
go tool pprof /tmp/heap
> top10
> list <function>
```

### 3. Check for Handle Leaks

**Verify in `internal/libemd/camera.go`**:

```go
type Camera struct {
    handle      *C.emd_cam_t
    eventHandle cgo.Handle  // ← Must be stored
    statsHandle cgo.Handle  // ← Must be stored
}

func (c *Camera) Close() {
    if c.handle != nil {
        C.emd_cam_close(c.handle)
        c.handle = nil
    }
    // Must delete handles:
    if c.eventHandle != 0 {
        c.eventHandle.Delete()
    }
    if c.statsHandle != 0 {
        c.statsHandle.Delete()
    }
}
```

**If handles are not deleted, this is a memory leak!**

### 4. Reduce String Allocations

**Optimization**: Avoid `C.GoString` in hot paths

```go
// Before (allocates every time):
evt := Event{
    CamName: C.GoString(&cEvt.cam_name[0]),
}

// After (reuse or avoid):
// Option 1: Just pass cam_id, look up name in Go map
evt := Event{
    CamID: uint16(cEvt.cam_id),
    // CamName populated by lookup
}

// Option 2: Use unsafe.String (Go 1.20+) with readonly C memory
evt := Event{
    CamName: unsafe.String(&cEvt.cam_name[0], C.strlen(&cEvt.cam_name[0])),
}
```

**Caution**: `unsafe.String` doesn't copy, so C memory must remain valid.

### 5. Monitor Container Memory

**Run with memory limit**:
```bash
docker run --rm --memory=2g --memory-swap=2g --name emd-agent-test \
    -v $(pwd)/k8s/agent.toml:/etc/emd-agent/agent.toml:ro \
    emd-agent:phase2-mvp

# Monitor from another terminal:
watch -n 1 'docker stats emd-agent-test --no-stream'
```

**Expected**:
- Initial: ~500-700 MB
- After 5 min: ~700-900 MB (Go heap growth)
- After 10 min: Stable ~800-1000 MB (GC steady state)

**If growing unbounded**: Memory leak

---

## Quick Fixes

### Fix 1: Reduce Ring Buffer Size (Immediate)

**Edit `k8s/agent.toml` or `k8s/agent.toml.example`**:
```toml
buffer_seconds = 10  # Was 20
```

**Impact**: 25 MB → 12.5 MB per camera = **175 MB savings** for 14 cameras

### Fix 2: Add Handle Cleanup (CRITICAL IF MISSING)

**Check `internal/libemd/camera.go`**:

Current `Camera` struct:
```go
type Camera struct {
    handle *C.emd_cam_t
}
```

**Should be**:
```go
type Camera struct {
    handle      *C.emd_cam_t
    eventHandle cgo.Handle
    statsHandle cgo.Handle
}
```

**In `OpenCamera`**:
```go
func OpenCamera(...) (*Camera, error) {
    eventHandle := cgo.NewHandle(eventCh)
    statsHandle := cgo.NewHandle(statsCh)
    
    cam := &Camera{
        handle:      cHandle,
        eventHandle: eventHandle,  // Store!
        statsHandle: statsHandle,  // Store!
    }
    return cam, nil
}
```

**In `Close`**:
```go
func (c *Camera) Close() {
    if c.handle != nil {
        C.emd_cam_close(c.handle)
        c.handle = nil
    }
    
    // Critical: Delete handles
    if c.eventHandle != 0 {
        c.eventHandle.Delete()
        c.eventHandle = 0
    }
    if c.statsHandle != 0 {
        c.statsHandle.Delete()
        c.statsHandle = 0
    }
}
```

**This is likely the leak!** cgo handles prevent GC of channels.

### Fix 3: Optimize String Allocations

**In `callbacks.go`**, reduce allocations:

```go
// For frequently called stats callback:
func goStatsTrampoline(userCtx unsafe.Pointer, cSample *C.emd_stats_sample_t) {
    // No strings in stats, but we can avoid other allocs
    
    sample := StatsSample{
        CamID:     uint16(cSample.cam_id),
        MonoNS:    uint64(cSample.mono_ns),
        BPFEwma:   float64(cSample.bpf_ewma),
        BPFSlow:   float64(cSample.bpf_slow),
        FSMState:  uint8(cSample.fsm_state),
        RTSPState: uint8(cSample.rtsp_state),
    }
    
    // Stats are frequent, make sure we're using non-blocking send
    select {
    case ch <- sample:
    default:
    }
}
```

**For events**, keep string copying but it's infrequent enough.

---

## Recommended Actions (Priority Order)

1. **CRITICAL**: Check and fix cgo.Handle cleanup in `Close()`
   - This is most likely the leak
   - If handles aren't deleted, channels can't be GC'd
   - Each handle holds a reference to a channel (1000-element buffer)

2. **HIGH**: Reduce ring buffer size for testing
   - Change `buffer_seconds = 10` in config
   - Saves 175 MB baseline
   - Test stability

3. **MEDIUM**: Add memory profiling endpoint
   - Add pprof import
   - Monitor heap growth
   - Identify allocation hotspots

4. **LOW**: Optimize string allocations
   - Only if profiling shows this is significant
   - Use unsafe.String for readonly C strings

---

## Testing Plan

1. **Fix handle cleanup**
2. **Rebuild and test**:
   ```bash
   docker build -t emd-agent:debug .
   docker run --rm --memory=2g --name emd-test \
       -v $(pwd)/k8s/agent.toml:/etc/emd-agent/agent.toml:ro \
       emd-agent:debug
   ```

3. **Monitor**:
   ```bash
   watch -n 5 'docker stats emd-test --no-stream'
   ```

4. **Expected behavior**:
   - Initial: ~600 MB
   - Steady state (after 10 min): ~700-900 MB
   - **No unbounded growth**

5. **If still leaking**:
   - Add pprof
   - Take heap snapshots every minute
   - Compare with `go tool pprof -base` to find growth

---

## Conclusion

**Most Likely Cause**: cgo.Handle leak (handles not deleted in Close)

**Evidence**:
- ✅ No video decoding
- ✅ Ring buffer is fixed-size
- ✅ Channels use non-blocking send
- ⚠️ Need to verify handle cleanup

**Fix**: Add `eventHandle.Delete()` and `statsHandle.Delete()` in `Camera.Close()`

**Expected Impact**: Complete fix (no more leak)

---

**Next Steps**: Review `camera.go`, add handle cleanup, rebuild, and retest.
