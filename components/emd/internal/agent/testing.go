package agent

import (
	"github.com/cvkitio/cvkit/edge/emd-agent/internal/metrics"
	"github.com/prometheus/client_golang/prometheus"
)

// NewTestMetrics creates a metrics instance for testing.
// Uses a custom registry to avoid conflicts with global Prometheus metrics.
func NewTestMetrics() *metrics.Metrics {
	// Create a custom registry for this test
	reg := prometheus.NewRegistry()
	_ = reg // Unused but avoids import warning

	m := &metrics.Metrics{
		DiskUsageBytes: prometheus.NewGaugeVec(
			prometheus.GaugeOpts{
				Name: "test_disk_usage_bytes",
				Help: "Test disk usage",
			},
			[]string{"camera"},
		),
		DiskClipsTotal: prometheus.NewGaugeVec(
			prometheus.GaugeOpts{
				Name: "test_disk_clips_total",
				Help: "Test clips total",
			},
			[]string{"camera"},
		),
		DiskCleanupRuns: prometheus.NewCounter(
			prometheus.CounterOpts{
				Name: "test_disk_cleanup_runs",
				Help: "Test cleanup runs",
			},
		),
		DiskClipsDeleted: prometheus.NewCounter(
			prometheus.CounterOpts{
				Name: "test_disk_clips_deleted",
				Help: "Test clips deleted",
			},
		),
	}

	return m
}
