// Package agent implements the Phase 2 Go supervisor.

package agent

import (
	"context"
	"fmt"
	"log"
	"sync"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/libemd"
)

// Version is the agent version string.
const Version = "1.0.0-phase2-mvp"

// Run is the main supervisor entry point.
// Loads config from TOML and runs all configured cameras.
func Run(ctx context.Context, configPath string) error {
	log.Printf("supervisor starting (config=%s)", configPath)

	// Load configuration
	cfg, err := LoadConfig(configPath)
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}

	log.Printf("loaded %d cameras from config", len(cfg.Cameras))

	// Create global event and stats channels
	eventCh := make(chan libemd.Event, 1000)  // Buffer for all cameras
	statsCh := make(chan libemd.StatsSample, 100)

	// Track camera workers
	var wg sync.WaitGroup
	stopCh := make(chan struct{})

	// Spawn a worker for each camera
	camID := uint16(0)
	for name, camCfg := range cfg.Cameras {
		libCfg := camCfg.ToLibemdConfig(name, camID)
		camID++

		wg.Add(1)
		go func(cfg *libemd.CameraConfig) {
			defer wg.Done()
			err := libemd.RunCameraWorker(cfg, eventCh, statsCh, stopCh)
			if err != nil {
				log.Printf("camera %s worker error: %v", cfg.Name, err)
			}
		}(libCfg)

		log.Printf("started camera worker: %s (cam_id=%d)", name, libCfg.CamID)
	}

	// Event processor
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

	// Wait for shutdown signal
	<-ctx.Done()
	log.Printf("supervisor shutting down")
	close(stopCh)

	// Wait for all camera workers to exit
	wg.Wait()
	log.Printf("all camera workers stopped")

	return nil
}
