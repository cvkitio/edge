// Package nats provides NATS JetStream wiring and canonical event schemas
// for the autotune component. The Event type mirrors the JSONL schema defined
// in cvkit/edge/docs/emd-tuning-reporting-spec.md (W4).
package nats

import "time"

// Event is a raw motion event published by the edge agent on
// events.raw.<site>.<camera>. It is the source record for all downstream
// processing: post-processor confirmation, S3 upload, and tuner input.
type Event struct {
	EventID         string    `json:"event_id"`
	Site            string    `json:"site"`
	Camera          string    `json:"camera"`
	CamID           uint16    `json:"cam_id"`
	Ts              time.Time `json:"ts"`
	TsMonoNS        uint64    `json:"ts_mono_ns"`
	Type            string    `json:"type"` // motion | scene_change | idr_burst
	ZScore          float64   `json:"z_score"`
	IntraRatio      float64   `json:"intra_ratio"`
	Bytes           uint64    `json:"bytes"`
	BPFSlow         float64   `json:"bpf_slow"`
	BPFEwma         float64   `json:"bpf_ewma"`
	BPFVar          float64   `json:"bpf_var"`
	SinceKF         uint32    `json:"since_kf"`
	Reason          string    `json:"reason"`
	FSMBefore       uint8     `json:"fsm_before"`
	FSMAfter        uint8     `json:"fsm_after"`
	PTSStart        uint64    `json:"pts_start"`
	PTSEnd          uint64    `json:"pts_end"`
	Codec           string    `json:"codec"`
	FPS             float64   `json:"fps"`
	ClipID          string    `json:"clip_id"`
	ClipURL         string    `json:"clip_url,omitempty"`
	ThumbsURL       string    `json:"thumbs_url,omitempty"`
	TargetClassMask uint8     `json:"target_class_mask"`
	AgentVersion    string    `json:"agent_version"`
}

// Label is the result of the post-processor's vision gate for a single event.
type Label struct {
	Classes    []string     `json:"classes"`    // e.g. ["person", "car"]
	MaxConf    float64      `json:"max_conf"`
	Model      string       `json:"model"` // e.g. "yolov8n@int8"
	Frames     []FrameLabel `json:"frames"`
}

// FrameLabel holds per-decoded-frame detection results.
type FrameLabel struct {
	OffsetMS   uint32      `json:"offset_ms"`
	Detections []Detection `json:"detections"`
	SSIM       float64     `json:"ssim,omitempty"`
	BGSubArea  float64     `json:"bgsub_area,omitempty"` // 0..1, fraction of frame
}

// Detection is one object detected in a frame.
type Detection struct {
	Class      string     `json:"class"`
	Confidence float64    `json:"confidence"`
	BBox       [4]float64 `json:"bbox"` // x1,y1,x2,y2 normalised 0..1
}

// ConfirmedEvent is published to events.confirmed.<site>.<camera>.
type ConfirmedEvent struct {
	Event
	Label       Label  `json:"label"`
	S3URL       string `json:"s3_url,omitempty"`
	ConfirmedBy string `json:"confirmed_by"` // postproc instance id, or "deadline"
}

// SuppressedEvent is published to events.suppressed.<site>.<camera>.
type SuppressedEvent struct {
	Event
	Label          *Label `json:"label,omitempty"`
	SuppressReason string `json:"suppress_reason"` // ssim_above | no_target_class | bgsub_low
	SuppressedBy   string `json:"suppressed_by"`
}

// UploadedClip is published to clips.uploaded.<site>.<camera>.
type UploadedClip struct {
	EventID  string    `json:"event_id"`
	Site     string    `json:"site"`
	Camera   string    `json:"camera"`
	S3URL    string    `json:"s3_url"`
	S3Key    string    `json:"s3_key"`
	SizeBytes uint64   `json:"size_bytes"`
	UploadedAt time.Time `json:"uploaded_at"`
}

// FeedbackEvent is published to events.feedback.<site>.<camera> by operators.
type FeedbackEvent struct {
	EventID   string    `json:"event_id"`
	Site      string    `json:"site"`
	Camera    string    `json:"camera"`
	Ts        time.Time `json:"ts"`
	Label     string    `json:"label"` // "tp" | "fp" | "fn"
	Note      string    `json:"note,omitempty"`
	Operator  string    `json:"operator,omitempty"`
}

// ConfigHistory is published to config.history.<site>.<camera> by the tuner.
type ConfigHistory struct {
	TunerRunID  string    `json:"tuner_run_id"`
	Site        string    `json:"site"`
	Camera      string    `json:"camera"`
	AppliedAt   time.Time `json:"applied_at"`
	PreviousCfg ConfigSnapshot `json:"previous_cfg"`
	NewCfg      ConfigSnapshot `json:"new_cfg"`
	PlanSummary PlanSummary    `json:"plan_summary"`
}

// ConfigSnapshot is a serialisable snapshot of the inspector config.
type ConfigSnapshot struct {
	MotionZHigh         float64 `json:"motion_z_high"`
	IntraRatioHigh      float64 `json:"intra_ratio_high"`
	OnThreshold         uint8   `json:"on_threshold"`
	OffThreshold        uint8   `json:"off_threshold"`
	MinBytesThreshold   uint32  `json:"min_bytes_threshold"`
	BPFRelativeFloor    float64 `json:"bpf_relative_floor"`
	ZHighWarmup         float64 `json:"z_high_warmup"`
	ZHighWarmupFrames   uint16  `json:"z_high_warmup_frames"`
	TargetClassMask     uint8   `json:"target_class_mask"`
}

// PlanSummary is the high-level outcome of one tuner run.
type PlanSummary struct {
	FPReductionPct  float64 `json:"fp_reduction_pct"`
	TPLossPct       float64 `json:"tp_loss_pct"`
	EventsEvaluated int     `json:"events_evaluated"`
	Objective       string  `json:"objective"` // e.g. "f0.5"
}

// TargetClass bitmask constants, matching EMD_TARGET_* in inspector.h.
const (
	TargetPerson  uint8 = 1 << 0
	TargetVehicle uint8 = 1 << 1
	TargetAnimal  uint8 = 1 << 2
	TargetOther   uint8 = 1 << 7
)

// ClassesFromMask returns the string names for a target class bitmask.
func ClassesFromMask(mask uint8) []string {
	var out []string
	if mask&TargetPerson != 0 {
		out = append(out, "person")
	}
	if mask&TargetVehicle != 0 {
		out = append(out, "vehicle")
	}
	if mask&TargetAnimal != 0 {
		out = append(out, "animal")
	}
	if mask&TargetOther != 0 {
		out = append(out, "other")
	}
	return out
}

// MaskFromClasses converts a slice of class names to a bitmask.
func MaskFromClasses(classes []string) uint8 {
	var mask uint8
	for _, c := range classes {
		switch c {
		case "person":
			mask |= TargetPerson
		case "vehicle":
			mask |= TargetVehicle
		case "animal":
			mask |= TargetAnimal
		case "other":
			mask |= TargetOther
		}
	}
	return mask
}
