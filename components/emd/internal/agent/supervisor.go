package agent

import (
	"context"
	"log"
	"path/filepath"
	"sync"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/eventlog"
	"github.com/cvkitio/cvkit/edge/emd-agent/internal/libemd"
	"github.com/cvkitio/cvkit/edge/emd-agent/internal/metrics"
	natspub "github.com/cvkitio/cvkit/edge/emd-agent/internal/nats"
)

// Supervisor manages camera workers and the recorder.
type Supervisor struct {
	cfg         *Config
	recorder    *RecorderWorker
	diskMgr     *DiskManager
	eventLog    *eventlog.Writer
	publisher   *natspub.Publisher
	eventCh     chan libemd.Event
	statsCh     chan libemd.StatsSample
	stopCh      chan struct{}
	wg          sync.WaitGroup
	healthMu    sync.RWMutex
	cameraReady map[string]bool
}

// eventLogRoot returns the effective eventlog directory, defaulting to a
// sibling of the clip root when not explicitly configured.
func eventLogRoot(cfg *Config) string {
	if cfg.Runtime.EventLogRoot != "" {
		return cfg.Runtime.EventLogRoot
	}
	if cfg.Runtime.ClipRoot != "" {
		return filepath.Join(filepath.Dir(cfg.Runtime.ClipRoot), "eventlog")
	}
	return "/var/lib/emd/eventlog"
}

// NewSupervisor creates a new supervisor.
func NewSupervisor(cfg *Config, m *metrics.Metrics) *Supervisor {
	// Create global event and stats channels.
	eventCh := make(chan libemd.Event, 1000)
	statsCh := make(chan libemd.StatsSample, 100)

	// Open the per-camera JSONL event log.
	evLog, err := eventlog.New(eventLogRoot(cfg), eventlog.Config{})
	if err != nil {
		log.Printf("warning: could not open event log: %v", err)
	}

	// Connect to NATS if configured (optional — nil if URL is empty).
	pub, err := natspub.New(cfg.NATS.URL)
	if err != nil {
		log.Printf("warning: NATS connect failed: %v (publishing disabled)", err)
		pub = nil
	} else if pub != nil {
		log.Printf("NATS: connected to %s", cfg.NATS.URL)
	}

	// Create recorder worker — it is the single consumer of eventCh.
	recorder := NewRecorderWorker(cfg, eventCh, evLog, pub)

	// Get camera names for disk manager.
	cameraNames := make([]string, 0, len(cfg.Cameras))
	for name := range cfg.Cameras {
		cameraNames = append(cameraNames, name)
	}

	// Create disk manager.
	diskMgr := NewDiskManager(cfg, m, cameraNames)

	return &Supervisor{
		cfg:         cfg,
		recorder:    recorder,
		diskMgr:     diskMgr,
		eventLog:    evLog,
		publisher:   pub,
		eventCh:     eventCh,
		statsCh:     statsCh,
		stopCh:      make(chan struct{}),
		cameraReady: make(map[string]bool),
	}
}

// Start starts the supervisor and all camera workers.
func (s *Supervisor) Start(ctx context.Context) error {
	s.recorder.Start()
	s.diskMgr.Start()

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

	// Stats logger — recorder owns the event channel, we only watch stats here.
	go func() {
		for {
			select {
			case sample := <-s.statsCh:
				log.Printf("STATS: cam=%d bpf_ewma=%.1f fsm=%d rtsp=%d",
					sample.CamID, sample.BPFEwma, sample.FSMState, sample.RTSPState)
			case <-ctx.Done():
				return
			}
		}
	}()

	// Wait for shutdown signal.
	<-ctx.Done()
	log.Printf("supervisor shutting down")
	close(s.stopCh)

	// Wait for all camera workers to exit.
	s.wg.Wait()
	log.Printf("all camera workers stopped")

	s.recorder.Stop()
	s.diskMgr.Stop()

	if s.publisher != nil {
		s.publisher.Close()
	}

	if s.eventLog != nil {
		if err := s.eventLog.Close(); err != nil {
			log.Printf("eventlog close: %v", err)
		}
	}

	return nil
}

// GetRecorder returns the recorder worker.
func (s *Supervisor) GetRecorder() *RecorderWorker {
	return s.recorder
}

// GetEventLogRoot returns the root directory used for per-camera JSONL event logs.
func (s *Supervisor) GetEventLogRoot() string {
	return eventLogRoot(s.cfg)
}

// GetCameraNames returns a list of all camera names.
func (s *Supervisor) GetCameraNames() []string {
	names := make([]string, 0, len(s.cfg.Cameras))
	for name := range s.cfg.Cameras {
		names = append(names, name)
	}
	return names
}

// SetCameraReady marks a camera as ready (connected and streaming).
func (s *Supervisor) SetCameraReady(name string, ready bool) {
	s.healthMu.Lock()
	defer s.healthMu.Unlock()
	s.cameraReady[name] = ready
}

// GetCameraStatus returns the ready status of all cameras.
func (s *Supervisor) GetCameraStatus() map[string]bool {
	s.healthMu.RLock()
	defer s.healthMu.RUnlock()
	status := make(map[string]bool, len(s.cameraReady))
	for name, ready := range s.cameraReady {
		status[name] = ready
	}
	return status
}

// AnyReadyFunc returns a readiness check function for health checks.
func (s *Supervisor) AnyReadyFunc() func() bool {
	return func() bool {
		s.healthMu.RLock()
		defer s.healthMu.RUnlock()
		for _, ready := range s.cameraReady {
			if ready {
				return true
			}
		}
		return false
	}
}
