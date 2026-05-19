package libemd

/*
#include <stdlib.h>
#include <emd/agent_abi.h>

extern void goEventTrampoline(void*, emd_event_t*);
extern void goStatsTrampoline(void*, emd_stats_sample_t*);
*/
import "C"
import (
	"fmt"
	"runtime"
	"runtime/cgo"
	"unsafe"
)

// Camera represents a camera handle (emd_cam_t).
type Camera struct {
	handle *C.emd_cam_t
	eventCh chan<- Event
	statsCh chan<- StatsSample
	eventHandle cgo.Handle
	statsHandle cgo.Handle
}

// OpenCamera creates a new camera instance.
// The returned camera must be closed with Close() when done.
func OpenCamera(cfg *CameraConfig, eventCh chan<- Event, statsCh chan<- StatsSample) (*Camera, error) {
	cCfg := cfg.toCConfig()
	if cCfg == nil {
		return nil, fmt.Errorf("failed to allocate C config")
	}
	defer C.free(unsafe.Pointer(cCfg))

	var errbuf [256]C.char
	handle := C.emd_cam_open(cCfg, &errbuf[0], 256)
	if handle == nil {
		return nil, errorString("emd_cam_open", &errbuf[0])
	}

	cam := &Camera{
		handle:  handle,
		eventCh: eventCh,
		statsCh: statsCh,
	}

	// Register event callback if event channel provided
	if eventCh != nil {
		cam.eventHandle = cgo.NewHandle(eventCh)
		C.emd_cam_set_event_cb(handle,
			C.emd_event_cb_t(C.goEventTrampoline),
			unsafe.Pointer(&cam.eventHandle))
	}

	// Register stats callback if stats channel provided
	if statsCh != nil {
		cam.statsHandle = cgo.NewHandle(statsCh)
		// Report stats every 5 seconds assuming ~30fps → 150 frames
		C.emd_cam_set_stats_cb(handle,
			C.emd_stats_cb_t(C.goStatsTrampoline),
			unsafe.Pointer(&cam.statsHandle),
			150)
	}

	return cam, nil
}

// Run runs the camera ingest loop on the calling goroutine.
// This function blocks until Stop() is called or a fatal error occurs.
//
// IMPORTANT: The calling goroutine must be locked to an OS thread
// (via runtime.LockOSThread()) before calling this function.
func (c *Camera) Run() error {
	var errbuf [256]C.char
	ret := C.emd_cam_run(c.handle, &errbuf[0], 256)
	switch ret {
	case 0:
		return nil // stopped cleanly
	case -1:
		return errorString("emd_cam_run fatal error", &errbuf[0])
	case -2:
		return errorString("emd_cam_run config rejected", &errbuf[0])
	default:
		return fmt.Errorf("emd_cam_run returned %d", ret)
	}
}

// Stop signals the camera to stop.
// Thread-safe. Returns immediately; Run() will return shortly after.
func (c *Camera) Stop() {
	if c.handle != nil {
		C.emd_cam_stop(c.handle)
	}
}

// Close releases all resources associated with the camera.
// The camera must have been stopped (Run() returned) before calling this.
func (c *Camera) Close() {
	if c.handle != nil {
		C.emd_cam_close(c.handle)
		c.handle = nil
	}
	if c.eventHandle != 0 {
		c.eventHandle.Delete()
		c.eventHandle = 0
	}
	if c.statsHandle != 0 {
		c.statsHandle.Delete()
		c.statsHandle = 0
	}
}

// Record writes a clip from the camera's ring buffer.
// Returns the clip metadata on success.
func (c *Camera) Record(fromPTS, toPTS uint64, req *ClipRequest) (*ClipHeader, error) {
	if c.handle == nil {
		return nil, fmt.Errorf("camera is closed")
	}

	cReq := req.toCRequest()
	if cReq == nil {
		return nil, fmt.Errorf("failed to allocate C request")
	}
	defer freeClipRequest(cReq)

	var cHdr C.emd_clip_header_t
	var errbuf [256]C.char

	ret := C.emd_cam_record(c.handle,
		C.uint64_t(fromPTS), C.uint64_t(toPTS),
		cReq, &cHdr, &errbuf[0], 256)

	switch ret {
	case 0:
		hdr := fromCClipHeader(&cHdr)
		return &hdr, nil
	case -1:
		return nil, fmt.Errorf("no data in range: %s", C.GoString(&errbuf[0]))
	case -2:
		return nil, errorString("muxer error", &errbuf[0])
	default:
		return nil, fmt.Errorf("emd_cam_record returned %d", ret)
	}
}

// RunCameraWorker is a helper that locks the goroutine to an OS thread,
// opens the camera, runs it, and cleans up on exit.
//
// This is the typical entry point for a camera worker goroutine.
func RunCameraWorker(cfg *CameraConfig, eventCh chan<- Event, statsCh chan<- StatsSample, stopCh <-chan struct{}) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	cam, err := OpenCamera(cfg, eventCh, statsCh)
	if err != nil {
		return fmt.Errorf("open camera: %w", err)
	}
	defer cam.Close()

	// Monitor stop channel in a separate goroutine
	go func() {
		<-stopCh
		cam.Stop()
	}()

	return cam.Run()
}

// InspectorConfig represents the motion detection configuration parameters.
type InspectorConfig struct {
	MotionZHigh          float64
	IntraRatioHigh       float64
	OnThreshold          uint8
	OffThreshold         uint8
	BPFFloor             float64
	MinBytesThreshold    uint32
	ConfiguredPeriodicKF bool
	GradualEnabled       bool
	GradualThreshold     float64
	GradualWindowFrames  uint32
}

// UpdateInspectorConfig updates the inspector configuration for a running camera.
// Changes take effect immediately (next frame processed).
// Thread-safe.
func (c *Camera) UpdateInspectorConfig(cfg *InspectorConfig) error {
	if c.handle == nil {
		return fmt.Errorf("camera is closed")
	}

	cCfg := C.emd_inspector_cfg_t{
		motion_z_high:           C.double(cfg.MotionZHigh),
		intra_ratio_high:        C.double(cfg.IntraRatioHigh),
		on_threshold:            C.uint8_t(cfg.OnThreshold),
		off_threshold:           C.uint8_t(cfg.OffThreshold),
		bpf_floor:               C.double(cfg.BPFFloor),
		min_bytes_threshold:     C.uint32_t(cfg.MinBytesThreshold),
		configured_periodic_kf:  C.bool(cfg.ConfiguredPeriodicKF),
		gradual_enabled:         C.bool(cfg.GradualEnabled),
		gradual_threshold:       C.double(cfg.GradualThreshold),
		gradual_window_frames:   C.uint32_t(cfg.GradualWindowFrames),
	}

	ret := C.emd_cam_update_inspector_cfg(c.handle, &cCfg)
	if ret != 0 {
		return fmt.Errorf("invalid configuration parameters")
	}

	return nil
}

// GetInspectorConfig retrieves the current inspector configuration.
// Thread-safe.
func (c *Camera) GetInspectorConfig() (*InspectorConfig, error) {
	if c.handle == nil {
		return nil, fmt.Errorf("camera is closed")
	}

	var cCfg C.emd_inspector_cfg_t
	ret := C.emd_cam_get_inspector_cfg(c.handle, &cCfg)
	if ret != 0 {
		return nil, fmt.Errorf("failed to get config")
	}

	cfg := &InspectorConfig{
		MotionZHigh:          float64(cCfg.motion_z_high),
		IntraRatioHigh:       float64(cCfg.intra_ratio_high),
		OnThreshold:          uint8(cCfg.on_threshold),
		OffThreshold:         uint8(cCfg.off_threshold),
		BPFFloor:             float64(cCfg.bpf_floor),
		MinBytesThreshold:    uint32(cCfg.min_bytes_threshold),
		ConfiguredPeriodicKF: bool(cCfg.configured_periodic_kf),
		GradualEnabled:       bool(cCfg.gradual_enabled),
		GradualThreshold:     float64(cCfg.gradual_threshold),
		GradualWindowFrames:  uint32(cCfg.gradual_window_frames),
	}

	return cfg, nil
}
