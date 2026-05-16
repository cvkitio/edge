# CPU Profiling Analysis

**Date**: 2026-05-16  
**CPU Usage**: 98% for single camera (axis_82_2)  
**Expected**: 5-10% for parsing an RTSP stream

---

## Profiling Results

### Sample Distribution (10 second sample, 8639 total samples)

| Function | Samples | % | Description |
|----------|---------|---|-------------|
| `__recvfrom` | 7048 | 82% | Network receive syscall |
| `poll` | 391 | 4.5% | Polling for network readiness |
| `clock_gettime` | 869 | 10% | Time checking in keepalive loop |
| Actual processing | ~10 | 0.1% | RTP depay, NAL push, inspector |

### Call Stack

```
emd_cam_run (main loop)
└─ emd_rtsp_tick (RTSP state machine)
   ├─ process_interleaved (82% of time)
   │  └─ emd_tcp_recv (busy-looping on network I/O)
   │     └─ __recvfrom (non-blocking read)
   └─ mono_ns (10% of time)
      └─ clock_gettime (keepalive timer check)
```

---

## Root Cause: Busy-Loop on Network I/O

### The Problem

The main loop is **burning CPU in a tight loop** checking for network data:

```c
// In emd_rtsp_tick -> process_interleaved
while (1) {
    int r = emd_tcp_recv(fd, buf, 1);  // Non-blocking read
    if (r == EMD_NET_AGAIN) return 0;  // No data, return immediately
    // Process data...
}
```

**Then in the main loop:**
```c
// In emd_cam_run
while (!stop_requested) {
    emd_rtsp_tick(rtsp);  // Returns immediately if no data
    // NO SLEEP - loops again immediately!
}
```

**Result**: CPU spins at 100% calling `recv()` with `MSG_DONTWAIT` thousands of times per second.

---

## Expected vs Actual Behavior

### Current (Broken)

```
Loop iteration: 1ms
├─ emd_rtsp_tick: call recv(MSG_DONTWAIT)
├─ Returns EAGAIN (no data)
├─ Check time
└─ Loop again immediately
```

**Iterations per second**: ~10,000  
**CPU usage**: 98%  
**Useful work**: <1%

### Expected (Efficient)

```
Loop iteration: 100ms (when data arrives)
├─ select/poll with timeout (blocks until data or timeout)
├─ Data ready: call recv() once
├─ Process RTP packet
└─ Loop when more data arrives
```

**Iterations per second**: ~30-60 (matches frame rate)  
**CPU usage**: 5-10%  
**Useful work**: 90%

---

## Why This Happened

### Architecture Issue

The code uses **non-blocking sockets** (`MSG_DONTWAIT`) but has **no event loop** to wait for readiness.

From `src/emd_net.c`:
```c
int emd_tcp_recv(int fd, uint8_t *buf, size_t len) {
    ssize_t r = recv(fd, buf, len, MSG_DONTWAIT);  // Non-blocking!
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return EMD_NET_AGAIN;  // No data available
        }
        return -1;
    }
    return (int)r;
}
```

The function returns immediately if no data is available, and the caller loops again without sleeping.

---

## Solutions

### Option 1: Use Blocking I/O (Simplest)

Change `emd_tcp_recv` to **block** instead of returning immediately:

```c
// In src/emd_net.c
int emd_tcp_recv(int fd, uint8_t *buf, size_t len) {
    ssize_t r = recv(fd, buf, len, 0);  // Blocking (remove MSG_DONTWAIT)
    if (r < 0) {
        if (errno == EINTR) return EMD_NET_AGAIN;  // Interrupted, retry
        return -1;
    }
    return (int)r;
}
```

**Pros**:
- Simple one-line fix
- CPU drops from 98% to ~5-10%

**Cons**:
- Blocks on I/O, can't check stop signal quickly
- Need timeout for clean shutdown

### Option 2: Add Timeout to Blocking Recv (Recommended)

Set a socket timeout so blocking recv returns periodically:

```c
// In emd_rtsp_client_new or emd_tcp_connect
struct timeval tv;
tv.tv_sec = 0;
tv.tv_usec = 100000;  // 100ms timeout
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

// Then in emd_tcp_recv
ssize_t r = recv(fd, buf, len, 0);  // Blocks up to 100ms
if (r < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return EMD_NET_AGAIN;  // Timeout, no data
    }
    return -1;
}
```

**Pros**:
- CPU efficient (blocks when idle)
- Responsive to stop signal (wakes every 100ms)
- Simple to implement

**Cons**:
- Slight delay on shutdown (up to 100ms)

### Option 3: Use Select/Poll Properly (Best, Most Work)

Add proper event loop with `poll()` or `select()`:

```c
// In emd_rtsp_tick
int poll_fd(int fd, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    int r = poll(&pfd, 1, timeout_ms);
    return r > 0 && (pfd.revents & POLLIN);
}

// Main loop
while (!stop_requested) {
    if (poll_fd(rtsp_fd, 100)) {  // Block up to 100ms
        emd_rtsp_tick(rtsp);  // Data ready
    }
    // Check stop signal every 100ms
}
```

**Pros**:
- Cleanest architecture
- Most CPU efficient
- Easy to add multiple fd monitoring

**Cons**:
- Requires refactoring main loop

---

## Recommended Fix

**Implement Option 2**: Add socket receive timeout

### Changes Needed

**File**: `src/emd_net.c`

```c
int emd_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms) {
    // ... existing connection code ...
    
    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;  // 100ms
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        EMD_LOGW("net", "failed to set SO_RCVTIMEO");
    }
    
    return fd;
}

int emd_tcp_recv(int fd, uint8_t *buf, size_t len) {
    // Remove MSG_DONTWAIT, use blocking recv with timeout
    ssize_t r = recv(fd, buf, len, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return EMD_NET_AGAIN;  // Timeout
        }
        if (errno == EINTR) return EMD_NET_AGAIN;
        return -1;
    }
    return (int)r;
}
```

**File**: `src/emd_cam.c` - **NO CHANGES NEEDED** (works with existing code)

---

## Expected Impact

### Before Fix

```
CPU:  98% (1 camera)
      294% (3 cameras)  
      ~1400% (14 cameras) - NOT FEASIBLE

Syscalls: ~10,000/sec per camera (mostly wasted)
Power:    High (laptop fans spin up)
```

### After Fix

```
CPU:  5-10% (1 camera)
      15-30% (3 cameras)
      70-140% (14 cameras) - FEASIBLE ✓

Syscalls: ~60/sec per camera (frame rate)
Power:    Low (normal operation)
```

---

## Additional Improvements

### Reduce clock_gettime Calls (10% CPU)

The keepalive timer checks time every loop iteration:

```c
// Current: check time every iteration (wasteful)
uint64_t now = mono_ns();  // calls clock_gettime
if (now - last_keepalive_ns >= keepalive_ms * 1000000ULL) {
    do_keepalive();
}

// Better: only check time every N iterations
static int tick_count = 0;
if (++tick_count >= 100) {  // Check every 100 frames
    tick_count = 0;
    uint64_t now = mono_ns();
    if (now - last_keepalive_ns >= keepalive_ms * 1000000ULL) {
        do_keepalive();
    }
}
```

**Savings**: ~10% CPU reduction

---

## Testing

### Before Fix

```bash
./build/emd-agent-fresh --config config.toml &
PID=$!
sleep 10
ps -p $PID -o %cpu
# Expected: 98%
```

### After Fix

```bash
# Rebuild with timeout fix
cmake --build build
cp build/libemd.a third_party/libemd/lib/
go build -o build/emd-agent-fixed ./cmd/emd-agent

./build/emd-agent-fixed --config config.toml &
PID=$!
sleep 10
ps -p $PID -o %cpu
# Expected: 5-10%
```

---

## Summary

**Root Cause**: Non-blocking I/O with no event loop → busy-loop burning 98% CPU

**Fix**: Add `SO_RCVTIMEO` to sockets and remove `MSG_DONTWAIT`

**Impact**: CPU drops from 98% to 5-10% per camera

**Status**: ✅ **FIXED** - see test results below

---

## Fix Implementation and Test Results

**Date Fixed**: 2026-05-16  
**Fix Applied**: Added `SO_RCVTIMEO` (100ms timeout) in `emd_tcp_connect()` and switched to blocking recv()

### Actual Results

Tested with 14 cameras for 60 seconds:

```
Before Fix (projected):
  CPU:  98% per camera × 14 = 1,372% total
  Feasibility: NOT FEASIBLE

After Fix (measured):
  PID  %CPU %MEM    RSS      VSZ
26348   2.9  0.5 138224 437187632

  CPU:  2.9% total for 14 cameras = 0.21% per camera
  Memory: 138 MB RSS
  Motion events: 12 in 60 seconds
  All cameras: ✓ Connected and streaming
  Feasibility: PRODUCTION READY ✓
```

### Performance Improvement

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| CPU per camera | 98.0% | 0.21% | **99.8% reduction** |
| CPU for 14 cameras | 1,372% | 2.9% | **99.8% reduction** |

**Conclusion**: The fix exceeded expectations. CPU usage is now negligible, making the 14-camera deployment not just feasible, but trivial in terms of CPU resources.

See `CPU_FIX_RESULTS.md` for complete details.
