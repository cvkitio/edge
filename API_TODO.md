# API TODO - Inspector Configuration Runtime Updates

**Status**: C/Go bindings complete, HTTP API pending

---

## What's Implemented

### C Library (✅ Complete)

**Files**:
- `include/emd/agent_abi.h` - Added ABI functions:
  - `emd_cam_update_inspector_cfg()` - Update inspector config at runtime
  - `emd_cam_get_inspector_cfg()` - Get current inspector config
- `src/emd_cam.c` - Implementation with pthread mutex for thread-safety

**Changes**:
- Added `pthread_mutex_t cfg_mutex` to `emd_cam` struct
- Mutex initialized in `emd_cam_open()`, destroyed in `emd_cam_close()`
- Config updates are atomic (lock → memcpy → unlock)
- Validated parameters (no negative thresholds)

### Go Bindings (✅ Complete)

**Files**:
- `internal/libemd/camera.go` - Added methods:
  - `Camera.UpdateInspectorConfig(cfg *InspectorConfig) error`
  - `Camera.GetInspectorConfig() (*InspectorConfig, error)`
- `internal/libemd/bindings.go` - Added `InspectorConfig` struct with fields:
  - `MotionZHigh` - Z-score threshold for motion detection
  - `IntraRatioHigh` - Intra-ratio threshold  
  - `OnThreshold` - Consecutive frames above to trigger
  - `OffThreshold` - Consecutive frames below to return to idle
  - `BPFFloor` - Minimum denominator (prevent div/0)
  - `GradualEnabled` - Enable gradual scene change detection
  - `GradualThreshold` - Gradual change threshold
  - `GradualWindowFrames` - Gradual detection window

**Files**:
- `internal/agent/recorder.go` - Added methods:
  - `RecorderWorker.GetCamera(name) (*Camera, bool)` 
  - `RecorderWorker.UpdateInspectorConfig(camName, cfg) error`
  - `RecorderWorker.GetInspectorConfig(camName) (*InspectorConfig, error)`

- `internal/agent/supervisor.go` - New supervisor struct:
  - Refactored from `agent.Run()` to return struct
  - Exposes `GetRecorder()` to access cameras
  - Manages camera workers and event channels

---

## What's Pending

### HTTP API (❌ Not Started)

Need to create HTTP endpoints to expose inspector config updates remotely.

**Recommended approach**:

1. **Create new file**: `internal/api/handler.go`
   ```go
   package api
   
   import (
       "encoding/json"
       "net/http"
       "github.com/cvkitio/cvkit/edge/emd-agent/internal/agent"
       "github.com/cvkitio/cvkit/edge/emd-agent/internal/libemd"
   )
   
   type Handler struct {
       supervisor *agent.Supervisor
   }
   
   func NewHandler(supervisor *agent.Supervisor) *Handler {
       return &Handler{supervisor: supervisor}
   }
   
   // GET /api/cameras/:name/config
   func (h *Handler) GetInspectorConfig(w http.ResponseWriter, r *http.Request) {
       // Extract camera name from URL
       // Call supervisor.GetRecorder().GetInspectorConfig(name)
       // Return JSON
   }
   
   // PUT /api/cameras/:name/config
   func (h *Handler) UpdateInspectorConfig(w http.ResponseWriter, r *http.Request) {
       // Extract camera name from URL
       // Parse JSON body into InspectorConfig
       // Call supervisor.GetRecorder().UpdateInspectorConfig(name, cfg)
       // Return success/error
   }
   
   // GET /api/cameras
   func (h *Handler) ListCameras(w http.ResponseWriter, r *http.Request) {
       // Return list of camera names
   }
   
   func (h *Handler) RegisterRoutes(mux *http.ServeMux) {
       mux.HandleFunc("/api/cameras", h.ListCameras)
       mux.HandleFunc("/api/cameras/", h.routeCameraRequests)
   }
   ```

2. **Modify `cmd/emd-agent/main.go`**:
   ```go
   // Change agent.Run to return supervisor
   supervisor, err := agent.NewSupervisor(cfg)
   if err != nil {
       log.Fatalf("create supervisor: %v", err)
   }
   
   // Start API server
   apiHandler := api.NewHandler(supervisor)
   apiMux := http.NewServeMux()
   apiHandler.RegisterRoutes(apiMux)
   
   go func() {
       log.Printf("API server listening on %s", *apiAddr)
       if err := http.ListenAndServe(*apiAddr, apiMux); err != nil {
           log.Printf("API server error: %v", err)
       }
   }()
   
   // Run supervisor
   if err := supervisor.Start(ctx); err != nil {
       log.Fatalf("supervisor error: %v", err)
   }
   ```

3. **Add CLI flag**:
   ```go
   apiAddr = flag.String("api", ":8080", "API server address")
   ```

### Example API Usage

**Get current config**:
```bash
curl http://localhost:8080/api/cameras/axis_82_2/config
```

**Response**:
```json
{
  "motion_z_high": 3.0,
  "intra_ratio_high": 2.5,
  "on_threshold": 2,
  "off_threshold": 45,
  "bpf_floor": 100.0,
  "gradual_enabled": false,
  "gradual_threshold": 0.15,
  "gradual_window_frames": 900
}
```

**Update config**:
```bash
curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config \
  -H "Content-Type: application/json" \
  -d '{
    "motion_z_high": 4.5,
    "intra_ratio_high": 3.0,
    "on_threshold": 3,
    "off_threshold": 50
  }'
```

**Response**:
```json
{
  "success": true,
  "message": "Configuration updated"
}
```

---

## Testing

Once API is implemented:

```bash
# Start agent
./emd-agent --config config.toml --api :8080

# Test get config
curl http://localhost:8080/api/cameras/axis_82_2/config | jq

# Test update (make less sensitive)
curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config \
  -H "Content-Type: application/json" \
  -d '{"motion_z_high": 5.0, "on_threshold": 3}' | jq

# Verify change took effect
curl http://localhost:8080/api/cameras/axis_82_2/config | jq

# Watch logs to see if false positives reduced
docker logs -f emd-container | grep "axis_82_2"
```

---

## Benefits

1. **No restart required** - Adjust sensitivity on the fly
2. **Per-camera tuning** - Each camera can have different thresholds
3. **Remote management** - Adjust from anywhere via HTTP
4. **A/B testing** - Easily test different thresholds
5. **Production debugging** - Quickly reduce sensitivity if too many false positives

---

## Security Considerations

**TODO before production**:
- Add authentication (API key or JWT)
- Add rate limiting
- Add input validation (ranges: z-score 0-100, thresholds 1-255)
- Add audit logging
- Use HTTPS in production
- Consider read-only vs read-write endpoints

---

## Summary

**Status**: ~80% complete
- ✅ C implementation with thread-safe updates
- ✅ Go bindings with type-safe API
- ✅ RecorderWorker integration
- ❌ HTTP REST API (needs ~2-3 hours to complete)

**Next Steps**:
1. Create `internal/api/handler.go`
2. Add HTTP routes for GET/PUT `/api/cameras/:name/config`
3. Test with curl
4. Add to Docker image
5. Document API in OpenAPI/Swagger spec
