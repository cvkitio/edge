package agent

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/libemd"
)

// RecorderWorker handles clip recording for motion events.
type RecorderWorker struct {
	cfg        *Config
	cameras    map[string]*libemd.Camera // camera name -> camera handle
	camerasMux sync.RWMutex
	eventCh    <-chan libemd.Event
	ctx        chan struct{} // for shutdown
}

// NewRecorderWorker creates a new recorder worker.
func NewRecorderWorker(cfg *Config, eventCh <-chan libemd.Event) *RecorderWorker {
	return &RecorderWorker{
		cfg:     cfg,
		cameras: make(map[string]*libemd.Camera),
		eventCh: eventCh,
		ctx:     make(chan struct{}),
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
			// Only record motion events
			if evt.Type == libemd.EventMotion {
				go r.recordClip(evt)
			}
		}
	}
}

// recordClip records a clip for the given event.
func (r *RecorderWorker) recordClip(evt libemd.Event) {
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

	// Create clip request
	clipReq := &libemd.ClipRequest{
		Container:   r.cfg.Recording.Container,
		OutPath:     destPath,
		FsyncPolicy: fsyncPolicy,
	}

	// Record clip
	startTime := time.Now()
	hdr, err := cam.Record(fromPTS, toPTS, clipReq)
	duration := time.Since(startTime)

	if err != nil {
		log.Printf("RECORD: failed to record clip for %s event %s: %v",
			evt.CamName, evt.ID, err)
		return
	}

	// Log successful recording
	log.Printf("CLIP: created %s size=%dMB duration=%.1fs z=%.2f (recorded in %.1fs)",
		filepath.Base(destPath),
		hdr.SizeBytes/1024/1024,
		float64(hdr.DurationMS)/1000.0,
		parseZScore(evt.Reason),
		duration.Seconds())

	// TODO: Publish clip metadata to NATS/MQTT
	// TODO: Queue for S3 upload
}

// parseZScore extracts z-score from event reason string.
func parseZScore(reason string) float64 {
	var z float64
	fmt.Sscanf(reason, "z=%f", &z)
	return z
}
