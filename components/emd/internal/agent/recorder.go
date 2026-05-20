package agent

import (
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/eventlog"
	"github.com/cvkitio/cvkit/edge/emd-agent/internal/libemd"
	natspub "github.com/cvkitio/cvkit/edge/emd-agent/internal/nats"
)

// ClipMeta is written as a JSON sidecar alongside each recorded clip.
// It captures the trigger statistics and timing so the UI can show where
// the motion spike occurs in the clip timeline.
type ClipMeta struct {
	EventID         string             `json:"event_id"`
	Camera          string             `json:"camera"`
	Site            string             `json:"site"`
	TS              time.Time          `json:"ts"`
	ZScore          float64            `json:"z_score"`
	BPFSlow         float64            `json:"bpf_slow"`
	BPFEwma         float64            `json:"bpf_ewma"`
	BPFVar          float64            `json:"bpf_var"`
	IntraRatio      float64            `json:"intra_ratio"`
	Bytes           uint64             `json:"bytes"`
	Reason          string             `json:"reason"`
	PreRollSeconds  uint32             `json:"pre_roll_seconds"`
	PostRollSeconds uint32             `json:"post_roll_seconds"`
	TriggerOffsetMS uint32  `json:"trigger_offset_ms"` // = pre_roll_seconds * 1000
	ClipDurationMS  uint64  `json:"clip_duration_ms"`
	ClipURL         string  `json:"clip_url,omitempty"`
	ZTimelineURL    string  `json:"z_timeline_url,omitempty"`
	Codec           string  `json:"codec"`
	FPS             float64 `json:"fps"`
}

// clipMetaPath returns the sidecar path for a clip file.
func clipMetaPath(clipFilePath string) string {
	return clipFilePath + ".meta.json"
}

// RecorderWorker handles clip recording for motion events.
type RecorderWorker struct {
	cfg        *Config
	cameras    map[string]*libemd.Camera // camera name -> camera handle
	camerasMux sync.RWMutex
	eventCh    <-chan libemd.Event
	eventLog   *eventlog.Writer
	publisher  *natspub.Publisher // nil = NATS disabled
	ctx        chan struct{}       // for shutdown
}

// NewRecorderWorker creates a new recorder worker.
func NewRecorderWorker(cfg *Config, eventCh <-chan libemd.Event, evLog *eventlog.Writer, pub *natspub.Publisher) *RecorderWorker {
	return &RecorderWorker{
		cfg:       cfg,
		cameras:   make(map[string]*libemd.Camera),
		eventCh:   eventCh,
		eventLog:  evLog,
		publisher: pub,
		ctx:       make(chan struct{}),
	}
}

// RegisterCamera registers a camera for recording.
func (r *RecorderWorker) RegisterCamera(name string, cam *libemd.Camera) {
	r.camerasMux.Lock()
	defer r.camerasMux.Unlock()
	r.cameras[name] = cam
}

// Start starts the recorder worker.
func (r *RecorderWorker) Start() {
	// Ensure clip directories exist
	if err := os.MkdirAll(r.cfg.Runtime.ClipRoot, 0755); err != nil {
		log.Printf("warning: failed to create clip root: %v", err)
	}
	if err := os.MkdirAll(r.cfg.Runtime.InflightRoot, 0755); err != nil {
		log.Printf("warning: failed to create inflight root: %v", err)
	}

	go r.processEvents()
}

// Stop stops the recorder worker.
func (r *RecorderWorker) Stop() {
	close(r.ctx)
}

// processEvents processes incoming motion events and triggers recording.
func (r *RecorderWorker) processEvents() {
	for {
		select {
		case <-r.ctx:
			return
		case evt := <-r.eventCh:
			log.Printf("EVENT: cam=%s type=%s reason=%s z=%.2f bpf_slow=%.0f bytes=%d",
				evt.CamName, evt.Type, evt.Reason, evt.ZScore, evt.BPFSlow, evt.Bytes)

			// Write to event log regardless of event type.
			if r.eventLog != nil {
				logEvt := toLogEvent(evt, r.cfg.Agent.InstanceID)
				if err := r.eventLog.Append(logEvt); err != nil {
					log.Printf("eventlog: append failed for %s: %v", evt.CamName, err)
				}
			}

			// Only record motion events.
			if evt.Type == libemd.EventMotion {
				go r.recordClip(evt)
			}
		}
	}
}

// toLogEvent converts a libemd.Event to an eventlog.Event.
func toLogEvent(evt libemd.Event, instanceID string) eventlog.Event {
	codecStr := "h264"
	if evt.Codec == 2 {
		codecStr = "h265"
	}
	return eventlog.Event{
		EventID:         evt.ID,
		Site:            instanceID,
		Camera:          evt.CamName,
		CamID:           evt.CamID,
		TS:              evt.StartedTime,
		TsMonoNs:        evt.StartedMonoNS,
		Type:            evt.Type.String(),
		ZScore:          evt.ZScore,
		IntraRatio:      evt.IntraRatio,
		Bytes:           evt.Bytes,
		BPFSlow:         evt.BPFSlow,
		BPFEwma:         evt.BPFEwma,
		BPFVar:          evt.BPFVar,
		SinceKF:         evt.SinceKF,
		FSMBefore:       evt.FSMBefore,
		FSMAfter:        evt.FSMAfter,
		Reason:          evt.Reason,
		PTSStart:        evt.PreRollPTS,
		PTSEnd:          evt.PostRollPTS,
		Codec:           codecStr,
		FPS:             evt.FPS,
		TargetClassMask: evt.TargetClassMask,
		AgentVersion:    Version,
	}
}

// recordClip records a clip for the given event.
func (r *RecorderWorker) recordClip(evt libemd.Event) {
	// Wait for post-roll data to accumulate in the ring buffer before recording.
	// The C library sets PostRollPTS = event_pts + post_roll_seconds*90kHz at the
	// moment the event fires, so without this sleep the ring buffer won't yet contain
	// the post-roll frames and the clip would be truncated to just pre-roll + event.
	if camCfg, ok := r.cfg.Cameras[evt.CamName]; ok && camCfg.PostRollSeconds > 0 {
		time.Sleep(time.Duration(camCfg.PostRollSeconds) * time.Second)
	}

	r.camerasMux.RLock()
	cam, ok := r.cameras[evt.CamName]
	r.camerasMux.RUnlock()

	if !ok {
		log.Printf("RECORD: camera %s not found for event %s", evt.CamName, evt.ID)
		return
	}

	// Calculate PTS window (pre-roll + post-roll)
	// Note: evt.PreRollPTS and evt.PostRollPTS are already calculated by C library
	fromPTS := evt.PreRollPTS
	toPTS := evt.PostRollPTS

	// Generate clip filename
	timestamp := time.Now().Format("20060102_150405")
	filename := fmt.Sprintf("%s_%s_%s.%s",
		evt.CamName,
		timestamp,
		evt.ID[:8], // First 8 chars of event ID
		r.cfg.Recording.Container)

	destPath := filepath.Join(r.cfg.Runtime.ClipRoot, evt.CamName, filename)

	// Ensure camera subdirectory exists
	if err := os.MkdirAll(filepath.Dir(destPath), 0755); err != nil {
		log.Printf("RECORD: failed to create camera directory: %v", err)
		return
	}

	// Convert fsync policy string to uint8
	fsyncPolicy := uint8(0) // on_close
	switch r.cfg.Recording.FsyncPolicy {
	case "always":
		fsyncPolicy = 1
	case "never":
		fsyncPolicy = 2
	}

	// Estimate z-buf capacity: pre+post roll at 30fps + 20% headroom
	preRollSec := uint32(0)
	postRollSec := uint32(0)
	if camCfg, ok := r.cfg.Cameras[evt.CamName]; ok {
		preRollSec = camCfg.PreRollSeconds
		postRollSec = camCfg.PostRollSeconds
	}
	zBufSize := int((preRollSec+postRollSec)*30) + 60

	// Create clip request
	clipReq := &libemd.ClipRequest{
		Container:   r.cfg.Recording.Container,
		OutPath:     destPath,
		FsyncPolicy: fsyncPolicy,
		ZBufSize:    zBufSize,
	}

	// Record clip
	startTime := time.Now()
	hdr, zTimeline, err := cam.Record(fromPTS, toPTS, clipReq)
	duration := time.Since(startTime)

	if err != nil {
		log.Printf("RECORD: failed to record clip for %s event %s: %v",
			evt.CamName, evt.ID, err)
		return
	}

	// Log successful recording
	log.Printf("CLIP: created %s size=%dMB duration=%.1fs z=%.2f (recorded in %.1fs, z_points=%d)",
		filepath.Base(destPath),
		hdr.SizeBytes/1024/1024,
		float64(hdr.DurationMS)/1000.0,
		parseZScore(evt.Reason),
		duration.Seconds(),
		len(zTimeline))

	// Resolve pre/post roll from per-camera config.
	codecStr := "h264"
	if evt.Codec == 2 {
		codecStr = "h265"
	}

	// Build clip and timeline URLs (empty if public_url not configured).
	base := filepath.Base(destPath)
	ext := filepath.Ext(base)
	baseNoExt := base[:len(base)-len(ext)]
	clipURL := ""
	zTimelineURL := ""
	if r.cfg.Runtime.PublicURL != "" {
		clipURL = r.cfg.Runtime.PublicURL + "/api/clips/" + evt.CamName + "/" + baseNoExt + ".m3u8"
		zTimelineURL = r.cfg.Runtime.PublicURL + "/api/clips/" + evt.CamName + "/" + baseNoExt + ".z.json"
	}

	// Write z-score timeline as a separate .z.json file so consumers can
	// fetch it on demand without bloating the NATS event payload.
	zJSONPath := filepath.Join(filepath.Dir(destPath), baseNoExt+".z.json")
	if len(zTimeline) > 0 {
		if zb, err := json.Marshal(zTimeline); err == nil {
			_ = os.WriteFile(zJSONPath, zb, 0640)
		}
	}

	// Write clip metadata sidecar.
	meta := ClipMeta{
		EventID:         evt.ID,
		Camera:          evt.CamName,
		Site:            r.cfg.Agent.InstanceID,
		TS:              evt.StartedTime,
		ZScore:          evt.ZScore,
		BPFSlow:         evt.BPFSlow,
		BPFEwma:         evt.BPFEwma,
		BPFVar:          evt.BPFVar,
		IntraRatio:      evt.IntraRatio,
		Bytes:           evt.Bytes,
		Reason:          evt.Reason,
		PreRollSeconds:  preRollSec,
		PostRollSeconds: postRollSec,
		TriggerOffsetMS: preRollSec * 1000,
		ClipDurationMS:  hdr.DurationMS,
		ClipURL:         clipURL,
		ZTimelineURL:    zTimelineURL,
		Codec:           codecStr,
		FPS:             evt.FPS,
	}
	if mb, err := json.Marshal(meta); err == nil {
		_ = os.WriteFile(clipMetaPath(destPath), mb, 0640)
	}

	// Publish to NATS JetStream (non-blocking on failure).
	if r.publisher != nil {
		natsEvt := natspub.Event{
			EventID:         evt.ID,
			Site:            r.cfg.Agent.InstanceID,
			Camera:          evt.CamName,
			CamID:           evt.CamID,
			Ts:              evt.StartedTime,
			TsMonoNS:        evt.StartedMonoNS,
			Type:            evt.Type.String(),
			ZScore:          evt.ZScore,
			IntraRatio:      evt.IntraRatio,
			Bytes:           evt.Bytes,
			BPFSlow:         evt.BPFSlow,
			BPFEwma:         evt.BPFEwma,
			BPFVar:          evt.BPFVar,
			SinceKF:         evt.SinceKF,
			Reason:          evt.Reason,
			FSMBefore:       evt.FSMBefore,
			FSMAfter:        evt.FSMAfter,
			PTSStart:        evt.PreRollPTS,
			PTSEnd:          evt.PostRollPTS,
			Codec:           meta.Codec,
			FPS:             evt.FPS,
			ClipID:          evt.ID,
			ClipURL:         clipURL,
			ZTimelineURL:    zTimelineURL,
			TriggerOffsetMS: meta.TriggerOffsetMS,
			TargetClassMask: evt.TargetClassMask,
			AgentVersion:    Version,
		}
		if err := r.publisher.Publish(natsEvt); err != nil {
			log.Printf("NATS: publish failed for %s event %s: %v", evt.CamName, evt.ID, err)
		} else {
			log.Printf("NATS: published event %s for %s z_timeline_url=%s", evt.ID[:8], evt.CamName, zTimelineURL)
		}
	}

	// TODO: Queue for S3 upload
}

// parseZScore extracts z-score from event reason string.
func parseZScore(reason string) float64 {
	var z float64
	fmt.Sscanf(reason, "z=%f", &z)
	return z
}

// GetCamera returns a camera handle by name.
func (r *RecorderWorker) GetCamera(name string) (*libemd.Camera, bool) {
	r.camerasMux.RLock()
	defer r.camerasMux.RUnlock()
	cam, ok := r.cameras[name]
	return cam, ok
}

// UpdateInspectorConfig updates the inspector configuration for a camera.
func (r *RecorderWorker) UpdateInspectorConfig(camName string, cfg *libemd.InspectorConfig) error {
	cam, ok := r.GetCamera(camName)
	if !ok {
		return fmt.Errorf("camera %s not found", camName)
	}
	return cam.UpdateInspectorConfig(cfg)
}

// GetInspectorConfig gets the current inspector configuration for a camera.
func (r *RecorderWorker) GetInspectorConfig(camName string) (*libemd.InspectorConfig, error) {
	cam, ok := r.GetCamera(camName)
	if !ok {
		return nil, fmt.Errorf("camera %s not found", camName)
	}
	return cam.GetInspectorConfig()
}
