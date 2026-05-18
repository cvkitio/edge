// Package metrics provides Prometheus metrics collection for emd-agent.
package metrics

import (
	"runtime"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
)

// Metrics holds all Prometheus metrics for the agent.
type Metrics struct {
	// Camera metrics
	CameraStatus     *prometheus.GaugeVec
	CameraConnected  prometheus.Gauge
	CameraTotal      prometheus.Gauge

	// Event metrics
	EventsTotal      *prometheus.CounterVec
	MotionEventsTotal prometheus.Counter

	// Recording metrics
	ClipsCreatedTotal *prometheus.CounterVec
	ClipBytesTotal    *prometheus.CounterVec
	ClipDuration      *prometheus.HistogramVec
	RecordingErrors   *prometheus.CounterVec

	// System metrics
	GoRoutines        prometheus.Gauge
	MemoryAllocBytes  prometheus.Gauge
	MemorySysBytes    prometheus.Gauge
	MemoryHeapBytes   prometheus.Gauge
	GCDuration        prometheus.Histogram

	// Disk metrics
	DiskUsageBytes    *prometheus.GaugeVec
	DiskClipsTotal    *prometheus.GaugeVec
	DiskCleanupRuns   prometheus.Counter
	DiskClipsDeleted  prometheus.Counter
}

// New creates and registers all Prometheus metrics.
func New() *Metrics {
	m := &Metrics{
		// Camera metrics
		CameraStatus: promauto.NewGaugeVec(
			prometheus.GaugeOpts{
				Name: "emd_camera_status",
				Help: "Camera connection status (1=connected, 0=disconnected)",
			},
			[]string{"camera"},
		),
		CameraConnected: promauto.NewGauge(
			prometheus.GaugeOpts{
				Name: "emd_cameras_connected",
				Help: "Number of cameras currently connected",
			},
		),
		CameraTotal: promauto.NewGauge(
			prometheus.GaugeOpts{
				Name: "emd_cameras_total",
				Help: "Total number of configured cameras",
			},
		),

		// Event metrics
		EventsTotal: promauto.NewCounterVec(
			prometheus.CounterOpts{
				Name: "emd_events_total",
				Help: "Total number of detection events by type",
			},
			[]string{"camera", "type"},
		),
		MotionEventsTotal: promauto.NewCounter(
			prometheus.CounterOpts{
				Name: "emd_motion_events_total",
				Help: "Total number of motion detection events",
			},
		),

		// Recording metrics
		ClipsCreatedTotal: promauto.NewCounterVec(
			prometheus.CounterOpts{
				Name: "emd_clips_created_total",
				Help: "Total number of clips created",
			},
			[]string{"camera"},
		),
		ClipBytesTotal: promauto.NewCounterVec(
			prometheus.CounterOpts{
				Name: "emd_clip_bytes_total",
				Help: "Total bytes written for clips",
			},
			[]string{"camera"},
		),
		ClipDuration: promauto.NewHistogramVec(
			prometheus.HistogramOpts{
				Name:    "emd_clip_duration_seconds",
				Help:    "Duration of recorded clips in seconds",
				Buckets: prometheus.LinearBuckets(5, 5, 10), // 5s to 50s in 5s steps
			},
			[]string{"camera"},
		),
		RecordingErrors: promauto.NewCounterVec(
			prometheus.CounterOpts{
				Name: "emd_recording_errors_total",
				Help: "Total number of recording errors",
			},
			[]string{"camera", "error"},
		),

		// System metrics
		GoRoutines: promauto.NewGauge(
			prometheus.GaugeOpts{
				Name: "emd_goroutines",
				Help: "Number of goroutines currently running",
			},
		),
		MemoryAllocBytes: promauto.NewGauge(
			prometheus.GaugeOpts{
				Name: "emd_memory_alloc_bytes",
				Help: "Bytes of allocated heap objects",
			},
		),
		MemorySysBytes: promauto.NewGauge(
			prometheus.GaugeOpts{
				Name: "emd_memory_sys_bytes",
				Help: "Total bytes obtained from OS",
			},
		),
		MemoryHeapBytes: promauto.NewGauge(
			prometheus.GaugeOpts{
				Name: "emd_memory_heap_bytes",
				Help: "Bytes in heap (live objects)",
			},
		),
		GCDuration: promauto.NewHistogram(
			prometheus.HistogramOpts{
				Name:    "emd_gc_duration_seconds",
				Help:    "GC pause duration in seconds",
				Buckets: prometheus.ExponentialBuckets(0.0001, 2, 12), // 0.1ms to ~400ms
			},
		),

		// Disk metrics
		DiskUsageBytes: promauto.NewGaugeVec(
			prometheus.GaugeOpts{
				Name: "emd_disk_usage_bytes",
				Help: "Disk usage in bytes by camera",
			},
			[]string{"camera"},
		),
		DiskClipsTotal: promauto.NewGaugeVec(
			prometheus.GaugeOpts{
				Name: "emd_disk_clips_total",
				Help: "Total number of clips on disk by camera",
			},
			[]string{"camera"},
		),
		DiskCleanupRuns: promauto.NewCounter(
			prometheus.CounterOpts{
				Name: "emd_disk_cleanup_runs_total",
				Help: "Total number of disk cleanup runs",
			},
		),
		DiskClipsDeleted: promauto.NewCounter(
			prometheus.CounterOpts{
				Name: "emd_disk_clips_deleted_total",
				Help: "Total number of clips deleted by cleanup",
			},
		),
	}

	// Start background collector for system metrics
	go m.collectSystemMetrics()

	return m
}

// collectSystemMetrics periodically collects Go runtime metrics.
func (m *Metrics) collectSystemMetrics() {
	ticker := time.NewTicker(15 * time.Second)
	defer ticker.Stop()

	for range ticker.C {
		var stats runtime.MemStats
		runtime.ReadMemStats(&stats)

		m.GoRoutines.Set(float64(runtime.NumGoroutine()))
		m.MemoryAllocBytes.Set(float64(stats.Alloc))
		m.MemorySysBytes.Set(float64(stats.Sys))
		m.MemoryHeapBytes.Set(float64(stats.HeapAlloc))

		// Record GC pause times
		if stats.NumGC > 0 {
			// Get the most recent GC pause
			pause := stats.PauseNs[(stats.NumGC+255)%256]
			m.GCDuration.Observe(float64(pause) / 1e9) // Convert ns to seconds
		}
	}
}

// RecordEvent records a detection event.
func (m *Metrics) RecordEvent(camera string, eventType string) {
	m.EventsTotal.WithLabelValues(camera, eventType).Inc()
	if eventType == "motion" {
		m.MotionEventsTotal.Inc()
	}
}

// RecordClip records a clip creation.
func (m *Metrics) RecordClip(camera string, bytes uint64, durationSeconds float64) {
	m.ClipsCreatedTotal.WithLabelValues(camera).Inc()
	m.ClipBytesTotal.WithLabelValues(camera).Add(float64(bytes))
	m.ClipDuration.WithLabelValues(camera).Observe(durationSeconds)
}

// RecordRecordingError records a recording error.
func (m *Metrics) RecordRecordingError(camera string, errorType string) {
	m.RecordingErrors.WithLabelValues(camera, errorType).Inc()
}

// UpdateCameraStatus updates camera connection status.
func (m *Metrics) UpdateCameraStatus(camera string, connected bool) {
	if connected {
		m.CameraStatus.WithLabelValues(camera).Set(1)
	} else {
		m.CameraStatus.WithLabelValues(camera).Set(0)
	}
}

// UpdateDiskMetrics updates disk usage metrics.
func (m *Metrics) UpdateDiskMetrics(camera string, bytes uint64, clipCount int) {
	m.DiskUsageBytes.WithLabelValues(camera).Set(float64(bytes))
	m.DiskClipsTotal.WithLabelValues(camera).Set(float64(clipCount))
}
