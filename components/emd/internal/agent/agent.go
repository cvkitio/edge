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

	// Create recorder worker (no eventlog in the legacy Run path).
	recorder := NewRecorderWorker(cfg, eventCh, nil)
	recorder.Start()
	defer recorder.Stop()

	// Track camera workers
	var wg sync.WaitGroup
	stopCh := make(chan struct{})

	// Spawn a worker for each camera
	camID := uint16(0)
	for name, camCfg := range cfg.Cameras {
		libCfg := camCfg.ToLibemdConfig(name, camID)
		camID++

		wg.Add(1)
		go func(cfg *libemd.CameraConfig, camName string) {
			defer wg.Done()

			// Open camera
			cam, err := libemd.OpenCamera(cfg, eventCh, statsCh)
			if err != nil {
				log.Printf("camera %s open error: %v", camName, err)
				return
			}
			defer cam.Close()

			// Register with recorder
			recorder.RegisterCamera(camName, cam)

			// Monitor stop channel
			go func() {
				<-stopCh
				cam.Stop()
			}()

			// Run camera worker
			if err := cam.Run(); err != nil {
				log.Printf("camera %s worker error: %v", camName, err)
			}
		}(libCfg, name)

		log.Printf("started camera worker: %s (cam_id=%d)", name, libCfg.CamID)
	}

	// Stats logger — recorder owns eventCh exclusively.
	go func() {
		for {
			select {
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
