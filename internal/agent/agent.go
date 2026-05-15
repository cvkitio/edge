// Package agent implements the Phase 2 Go supervisor.
//
// For now, this is a minimal MVP that runs a single camera from a hardcoded
// config to demonstrate the ABI boundary works. Full Phase 2 features will
// be added incrementally:
//  - TOML config parsing
//  - Multi-camera support
//  - NATS/MQTT publishing
//  - S3 upload
//  - Outbox
//  - Gate rules
//  - Control plane
//  - Metrics/traces

package agent

import (
	"context"
	"fmt"
	"log"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/libemd"
)

// Version is the agent version string.
const Version = "1.0.0-phase2-mvp"

// Run is the main supervisor entry point.
// For now, it just runs a single hardcoded camera to prove the ABI works.
func Run(ctx context.Context, configPath string) error {
	log.Printf("supervisor starting (config=%s)", configPath)

	// For MVP, use a hardcoded camera config
	// TODO: parse TOML config from configPath
	cfg := &libemd.CameraConfig{
		Name:            "test_camera",
		URL:             "rtsp://root:***REDACTED_PASSWORD***@10.45.81.7/axis-media/media.amp?videocodec=h264&resolution=1920x1080",
		CamID:           0,
		Transport:       0, // TCP
		CodecHint:       1, // H.264
		BufferSeconds:   15,
		PreRollSeconds:  6,
		PostRollSeconds: 10,
		ClipMaxSeconds:  120,
		MaxBitrateBPS:   8000000,
		MotionZHigh:     3.0,
		IntraRatioHigh:  2.5,
		OnThreshold:     2,
		OffThreshold:    15,
		GradualEnabled:  false,
	}

	// Create event and stats channels
	eventCh := make(chan libemd.Event, 100)
	statsCh := make(chan libemd.StatsSample, 100)

	// Spawn camera worker in a goroutine
	stopCh := make(chan struct{})
	errCh := make(chan error, 1)

	go func() {
		err := libemd.RunCameraWorker(cfg, eventCh, statsCh, stopCh)
		if err != nil {
			log.Printf("camera worker error: %v", err)
		}
		errCh <- err
	}()

	// Event processor (just logs for now)
	go func() {
		for {
			select {
			case evt := <-eventCh:
				log.Printf("EVENT: cam=%s type=%s reason=%s pts=%d",
					evt.CamName, evt.Type, evt.Reason, evt.StartedPTS)
				// TODO: gate rules, recorder driver, publisher, uploader
			case sample := <-statsCh:
				log.Printf("STATS: cam=%d bpf_ewma=%.1f fsm=%d rtsp=%d",
					sample.CamID, sample.BPFEwma, sample.FSMState, sample.RTSPState)
			case <-ctx.Done():
				return
			}
		}
	}()

	// Wait for shutdown signal or camera error
	select {
	case <-ctx.Done():
		log.Printf("supervisor shutting down")
		close(stopCh)
		// Wait for camera worker to exit
		<-errCh
		return nil
	case err := <-errCh:
		if err != nil {
			return fmt.Errorf("camera worker failed: %w", err)
		}
		return nil
	}
}
