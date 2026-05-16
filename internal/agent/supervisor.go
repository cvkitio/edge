package agent

import (
	"context"
	"log"
	"sync"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/libemd"
)

// Supervisor manages camera workers and the recorder.
type Supervisor struct {
	cfg      *Config
	recorder *RecorderWorker
	eventCh  chan libemd.Event
	statsCh  chan libemd.StatsSample
	stopCh   chan struct{}
	wg       sync.WaitGroup
}

// NewSupervisor creates a new supervisor.
func NewSupervisor(cfg *Config) *Supervisor {
	// Create global event and stats channels
	eventCh := make(chan libemd.Event, 1000)
	statsCh := make(chan libemd.StatsSample, 100)

	// Create recorder worker
	recorder := NewRecorderWorker(cfg, eventCh)

	return &Supervisor{
		cfg:      cfg,
		recorder: recorder,
		eventCh:  eventCh,
		statsCh:  statsCh,
		stopCh:   make(chan struct{}),
	}
}

// Start starts the supervisor and all camera workers.
func (s *Supervisor) Start(ctx context.Context) error {
	s.recorder.Start()

	// Spawn a worker for each camera
	camID := uint16(0)
	for name, camCfg := range s.cfg.Cameras {
		libCfg := camCfg.ToLibemdConfig(name, camID)
		camID++

		s.wg.Add(1)
		go func(cfg *libemd.CameraConfig, camName string) {
			defer s.wg.Done()

			// Open camera
			cam, err := libemd.OpenCamera(cfg, s.eventCh, s.statsCh)
			if err != nil {
				log.Printf("camera %s open error: %v", camName, err)
				return
			}
			defer cam.Close()

			// Register with recorder
			s.recorder.RegisterCamera(camName, cam)

			// Monitor stop channel
			go func() {
				<-s.stopCh
				cam.Stop()
			}()

			// Run camera worker
			if err := cam.Run(); err != nil {
				log.Printf("camera %s worker error: %v", camName, err)
			}
		}(libCfg, name)

		log.Printf("started camera worker: %s (cam_id=%d)", name, libCfg.CamID)
	}

	// Stats logger (events are handled by recorder)
	go func() {
		for {
			select {
			case evt := <-s.eventCh:
				// Log event (recorder will handle clip creation)
				log.Printf("EVENT: cam=%s type=%s reason=%s pts=%d",
					evt.CamName, evt.Type, evt.Reason, evt.StartedPTS)
			case sample := <-s.statsCh:
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
	close(s.stopCh)

	// Wait for all camera workers to exit
	s.wg.Wait()
	log.Printf("all camera workers stopped")

	s.recorder.Stop()

	return nil
}

// GetRecorder returns the recorder worker.
func (s *Supervisor) GetRecorder() *RecorderWorker {
	return s.recorder
}
