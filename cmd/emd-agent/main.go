// emd-agent — Phase 2 Go supervisor for Edge Motion Detector
//
// This binary replaces the Phase 1 C supervisor with a Go outer that:
//  - Spawns camera workers via cgo into libemd
//  - Handles events via callbacks (C → Go)
//  - Drives recording, publishing, uploading via native Go
//
// For now (migration step 4 per phase2 spec §18), this is a minimal MVP
// that demonstrates the ABI boundary works. Full Phase 2 features (NATS,
// S3, outbox, rules, etc.) will be added incrementally.

package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net/http"
	_ "net/http/pprof"
	"os"
	"os/signal"
	"runtime"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus/promhttp"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/agent"
	"github.com/cvkitio/cvkit/edge/emd-agent/internal/api"
	"github.com/cvkitio/cvkit/edge/emd-agent/internal/health"
	"github.com/cvkitio/cvkit/edge/emd-agent/internal/libemd"
	"github.com/cvkitio/cvkit/edge/emd-agent/internal/metrics"
)

var (
	configPath = flag.String("config", "/etc/emd-agent/agent.toml", "path to config file")
	version    = flag.Bool("version", false, "print version and exit")
	pprofAddr  = flag.String("pprof", "localhost:6060", "pprof debug server address")
	metricsAddr = flag.String("metrics", ":9464", "metrics and health check server address")
	apiAddr    = flag.String("api", ":8080", "API server address")
)

func main() {
	flag.Parse()

	if *version {
		fmt.Printf("emd-agent %s\n", agent.Version)
		fmt.Printf("libemd: %s\n", libemd.BuildInfo())
		os.Exit(0)
	}

	log.SetFlags(log.LstdFlags | log.Lshortfile)
	log.Printf("emd-agent %s starting", agent.Version)

	// Verify ABI version
	abiVer := libemd.ABIVersion()
	log.Printf("libemd ABI version: %d.%d.%d",
		(abiVer>>16)&0xFF, (abiVer>>8)&0xFF, abiVer&0xFF)

	// For now, just run a single camera from the config
	// Full supervisor with multi-camera support will be added in next iteration
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Start pprof debug server
	go func() {
		log.Printf("pprof debug server listening on %s", *pprofAddr)
		if err := http.ListenAndServe(*pprofAddr, nil); err != nil {
			log.Printf("pprof server error: %v", err)
		}
	}()

	// Start memory stats logger
	go logMemoryStats(ctx)

	// Signal handling
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	go func() {
		sig := <-sigCh
		log.Printf("received signal %v, shutting down", sig)
		cancel()
	}()

	// Load configuration
	cfg, err := agent.LoadConfig(*configPath)
	if err != nil {
		log.Fatalf("load config: %v", err)
	}

	log.Printf("loaded %d cameras from config", len(cfg.Cameras))

	// Initialize Prometheus metrics
	promMetrics := metrics.New()
	promMetrics.CameraTotal.Set(float64(len(cfg.Cameras)))

	// Create supervisor
	supervisor := agent.NewSupervisor(cfg, promMetrics)

	// Start health check and metrics server (for Kubernetes probes and Prometheus)
	healthHandler := health.NewHandler()
	healthMux := http.NewServeMux()
	healthHandler.RegisterRoutes(healthMux)
	healthMux.Handle("/metrics", promhttp.Handler())

	go func() {
		log.Printf("health check and metrics server listening on %s", *metricsAddr)
		if err := http.ListenAndServe(*metricsAddr, healthMux); err != nil {
			log.Printf("health check and metrics server error: %v", err)
		}
	}()

	// Start API server
	apiHandler := api.NewHandler(supervisor)
	apiMux := http.NewServeMux()
	apiHandler.RegisterRoutes(apiMux)

	go func() {
		log.Printf("API server listening on %s", *apiAddr)
		if err := http.ListenAndServe(*apiAddr, apiMux); err != nil {
			log.Printf("API server error: %v", err)
		}
	}()

	// Run supervisor
	if err := supervisor.Start(ctx); err != nil {
		log.Fatalf("supervisor error: %v", err)
	}

	log.Printf("emd-agent stopped cleanly")
}

// logMemoryStats periodically logs memory statistics for debugging.
func logMemoryStats(ctx context.Context) {
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			var m runtime.MemStats
			runtime.ReadMemStats(&m)
			log.Printf("MEMORY: alloc=%dMB sys=%dMB heapAlloc=%dMB heapSys=%dMB numGC=%d goroutines=%d",
				m.Alloc/1024/1024,
				m.Sys/1024/1024,
				m.HeapAlloc/1024/1024,
				m.HeapSys/1024/1024,
				m.NumGC,
				runtime.NumGoroutine())
		}
	}
}
