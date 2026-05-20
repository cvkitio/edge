// Package libemd provides Go bindings to the libemd C library (Phase 2 ABI).
//
// This package wraps the C functions declared in emd/agent_abi.h and provides
// idiomatic Go types and error handling.

package libemd

/*
#cgo CFLAGS: -I${SRCDIR}/../../third_party/libemd/include
#cgo LDFLAGS: -L${SRCDIR}/../../third_party/libemd/lib -lemd -lm -lpthread
#include <stdlib.h>
#include <emd/agent_abi.h>

// Forward declarations for trampolines (defined in callbacks.go)
extern void goEventTrampoline(void *user_ctx, emd_event_t *evt);
extern void goStatsTrampoline(void *user_ctx, emd_stats_sample_t *sample);
*/
import "C"
import (
	"fmt"
	"time"
	"unsafe"
)

// ABIVersion returns the packed ABI version from libemd.
// Format: 0xMMmmpp (MAJOR.MINOR.PATCH)
func ABIVersion() uint32 {
	return uint32(C.emd_abi_version())
}

// BuildInfo returns the libemd build info string.
func BuildInfo() string {
	return C.GoString(C.emd_build_info())
}

// Event represents a detection event from the inspector.
type Event struct {
	ID          string
	CamID       uint16
	Type        EventType
	Reason      string
	StartedPTS    uint64 // 90 kHz
	StartedTime   time.Time
	StartedMonoNS uint64 // monotonic ns from C (emd_event_t.started_mono_ns)
	Codec         uint8  // 1=h264, 2=h265
	FPS         float64
	CamName     string
	PreRollPTS  uint64
	PostRollPTS uint64

	// Inspector signal snapshot — autotune grid search inputs.
	ZScore          float64
	IntraRatio      float64
	Bytes           uint64
	BPFSlow         float64
	BPFEwma         float64
	BPFVar          float64
	SinceKF         uint32
	FSMBefore       uint8
	FSMAfter        uint8
	TargetClassMask uint8
}

// EventType represents the type of detection event.
type EventType uint8

const (
	EventNone        EventType = 0
	EventMotion      EventType = 1
	EventSceneChange EventType = 2
	EventIDRBurst    EventType = 3
)

func (t EventType) String() string {
	switch t {
	case EventMotion:
		return "motion"
	case EventSceneChange:
		return "scene_change"
	case EventIDRBurst:
		return "idr_burst"
	default:
		return "none"
	}
}

// StatsSample represents a periodic statistics sample.
type StatsSample struct {
	CamID      uint16
	MonoNS     uint64
	BPFEwma    float64
	BPFSlow    float64
	FSMState   uint8
	RTSPState  uint8
}

// CameraConfig mirrors emd_camera_cfg_t from C.
type CameraConfig struct {
	Name               string
	URL                string
	CamID              uint16
	Transport          uint8 // 0=TCP, 1=UDP
	CodecHint          uint8 // 0=auto, 1=h264, 2=h265
	BufferSeconds      uint32
	PreRollSeconds     uint32
	PostRollSeconds    uint32
	ClipMaxSeconds     uint32
	MaxBitrateBPS      uint32
	MotionZHigh        float64
	IntraRatioHigh     float64
	OnThreshold        uint8
	OffThreshold       uint8
	GradualEnabled     bool
	MinBytesThreshold  uint32 // suppress events below this NAL byte count
}

// toCConfig converts a Go CameraConfig to C emd_camera_cfg_t.
func (cfg *CameraConfig) toCConfig() *C.emd_camera_cfg_t {
	// Use calloc so all fields (including string arrays) are zero-initialised.
	// This guarantees NUL termination even when Name/URL fill the buffer exactly.
	c := (*C.emd_camera_cfg_t)(C.calloc(1, C.sizeof_emd_camera_cfg_t))
	if c == nil {
		return nil
	}

	// Copy string fields — calloc guarantees a trailing NUL in all cases.
	nameBytes := []byte(cfg.Name)
	urlBytes := []byte(cfg.URL)

	// Manually copy bytes (strncpy equivalent), leaving last byte as NUL.
	nameMax := int(C.EMD_MAX_CAM_NAME_LEN) - 1
	for i := 0; i < len(nameBytes) && i < nameMax; i++ {
		c.name[i] = C.char(nameBytes[i])
	}
	urlMax := int(C.EMD_MAX_URL_LEN) - 1
	for i := 0; i < len(urlBytes) && i < urlMax; i++ {
		c.url[i] = C.char(urlBytes[i])
	}

	// Copy numeric fields
	c.cam_id = C.uint16_t(cfg.CamID)
	c.transport = C.emd_transport_t(cfg.Transport)
	c.codec_hint = C.emd_codec_hint_t(cfg.CodecHint)
	c.buffer_seconds = C.uint32_t(cfg.BufferSeconds)
	c.pre_roll_seconds = C.uint32_t(cfg.PreRollSeconds)
	c.post_roll_seconds = C.uint32_t(cfg.PostRollSeconds)
	c.clip_max_seconds = C.uint32_t(cfg.ClipMaxSeconds)
	c.max_bitrate_bps = C.uint32_t(cfg.MaxBitrateBPS)
	c.motion_z_high = C.double(cfg.MotionZHigh)
	c.intra_ratio_high = C.double(cfg.IntraRatioHigh)
	c.on_threshold = C.uint8_t(cfg.OnThreshold)
	c.off_threshold = C.uint8_t(cfg.OffThreshold)
	c.gradual_enabled = C.bool(cfg.GradualEnabled)
	c.min_bytes_threshold = C.uint32_t(cfg.MinBytesThreshold)

	return c
}

// ClipHeader represents metadata about a recorded clip.
type ClipHeader struct {
	EventID     string
	CamIDStr    string
	Container   string
	Codec       string
	Path        string
	SizeBytes   uint64
	DurationMS  uint64
	PreRollMS   uint64
	PostRollMS  uint64
	SHA256      string
}

// ZPoint is one entry in a per-frame z-score timeline.
// OffsetMS is milliseconds from the clip start; ZScore is the inspector value.
type ZPoint struct {
	OffsetMS uint32
	ZScore   float32
}

// ClipRequest represents a clip recording request.
type ClipRequest struct {
	OutPath     string
	Container   string // "mpegts" or "fmp4"
	FsyncPolicy uint8  // 0=on_close, 1=always, 2=never
	// ZBufSize is the number of z-score timeline slots to allocate.
	// Pass 0 to skip timeline extraction. Typical: pre+post roll frames (e.g. 300).
	ZBufSize int
}

func (req *ClipRequest) toCRequest() *C.emd_clip_request_t {
	c := (*C.emd_clip_request_t)(C.calloc(1, C.sizeof_emd_clip_request_t))
	if c == nil {
		return nil
	}

	c.out_path = C.CString(req.OutPath)
	c.container = C.CString(req.Container)
	c.fsync_policy = C.emd_fsync_policy_t(req.FsyncPolicy)

	if req.ZBufSize > 0 {
		c.z_buf = (*C.emd_z_point_t)(C.calloc(C.size_t(req.ZBufSize), C.sizeof_emd_z_point_t))
		c.z_buf_count = C.uint32_t(req.ZBufSize)
		// z_out_count: allocate a single uint32_t that C will write into
		c.z_out_count = (*C.uint32_t)(C.calloc(1, C.size_t(unsafe.Sizeof(C.uint32_t(0)))))
	}

	return c
}

// extractZPoints reads the z-score timeline from a completed clip request.
// Must be called before freeClipRequest.
func extractZPoints(c *C.emd_clip_request_t) []ZPoint {
	if c.z_buf == nil || c.z_out_count == nil {
		return nil
	}
	n := int(*c.z_out_count)
	if n <= 0 {
		return nil
	}
	pts := make([]ZPoint, n)
	base := uintptr(unsafe.Pointer(c.z_buf))
	for i := 0; i < n; i++ {
		p := (*C.emd_z_point_t)(unsafe.Pointer(base + uintptr(i)*C.sizeof_emd_z_point_t))
		pts[i] = ZPoint{
			OffsetMS: uint32(p.offset_ms),
			ZScore:   float32(p.z_score),
		}
	}
	return pts
}

func freeClipRequest(c *C.emd_clip_request_t) {
	if c == nil {
		return
	}
	C.free(unsafe.Pointer(c.out_path))
	C.free(unsafe.Pointer(c.container))
	if c.z_buf != nil {
		C.free(unsafe.Pointer(c.z_buf))
	}
	if c.z_out_count != nil {
		C.free(unsafe.Pointer(c.z_out_count))
	}
	C.free(unsafe.Pointer(c))
}

// fromCClipHeader converts C emd_clip_header_t to Go ClipHeader.
func fromCClipHeader(c *C.emd_clip_header_t) ClipHeader {
	return ClipHeader{
		EventID:    C.GoString(&c.event_id[0]),
		CamIDStr:   C.GoString(&c.cam_id_str[0]),
		Container:  C.GoString(&c.container[0]),
		Codec:      C.GoString(&c.codec[0]),
		Path:       C.GoString(&c.path[0]),
		SizeBytes:  uint64(c.size_bytes),
		DurationMS: uint64(c.duration_ms),
		PreRollMS:  uint64(c.pre_roll_ms),
		PostRollMS: uint64(c.post_roll_ms),
		SHA256:     C.GoString(&c.sha256[0]),
	}
}

// errorString helper for building error messages with C errbuf.
func errorString(prefix string, cErrbuf *C.char) error {
	return fmt.Errorf("%s: %s", prefix, C.GoString(cErrbuf))
}
