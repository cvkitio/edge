package agent

import (
	"log"
	"os"
	"path/filepath"
	"sort"
	"time"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/metrics"
)

// ClipInfo holds metadata about a clip file.
type ClipInfo struct {
	Path    string
	Size    int64
	ModTime time.Time
}

// DiskManager manages disk usage and clip retention.
type DiskManager struct {
	cfg        *Config
	metrics    *metrics.Metrics
	cameraList []string
	stopCh     chan struct{}
}

// NewDiskManager creates a new disk manager.
func NewDiskManager(cfg *Config, metrics *metrics.Metrics, cameraList []string) *DiskManager {
	return &DiskManager{
		cfg:        cfg,
		metrics:    metrics,
		cameraList: cameraList,
		stopCh:     make(chan struct{}),
	}
}

// Start starts the disk manager background worker.
func (d *DiskManager) Start() {
	// Run initial cleanup
	go d.runCleanup()

	// Start periodic cleanup (every 5 minutes)
	go d.periodicCleanup()
}

// Stop stops the disk manager.
func (d *DiskManager) Stop() {
	close(d.stopCh)
}

// periodicCleanup runs cleanup periodically.
func (d *DiskManager) periodicCleanup() {
	ticker := time.NewTicker(5 * time.Minute)
	defer ticker.Stop()

	for {
		select {
		case <-d.stopCh:
			return
		case <-ticker.C:
			d.runCleanup()
		}
	}
}

// runCleanup performs cleanup for all cameras.
func (d *DiskManager) runCleanup() {
	log.Printf("DISK: starting cleanup cycle")
	d.metrics.DiskCleanupRuns.Inc()

	for _, camName := range d.cameraList {
		d.cleanupCamera(camName)
	}
}

// cleanupCamera performs cleanup for a single camera.
func (d *DiskManager) cleanupCamera(camName string) {
	clipDir := filepath.Join(d.cfg.Runtime.ClipRoot, camName)

	// Check if directory exists
	if _, err := os.Stat(clipDir); os.IsNotExist(err) {
		// No clips yet for this camera
		d.metrics.UpdateDiskMetrics(camName, 0, 0)
		return
	}

	// Scan directory for clips
	clips, totalSize, err := d.scanClipDirectory(clipDir)
	if err != nil {
		log.Printf("DISK: error scanning %s: %v", clipDir, err)
		return
	}

	// Update metrics before cleanup
	d.metrics.UpdateDiskMetrics(camName, uint64(totalSize), len(clips))

	if len(clips) == 0 {
		return
	}

	// Sort clips by modification time (oldest first)
	sort.Slice(clips, func(i, j int) bool {
		return clips[i].ModTime.Before(clips[j].ModTime)
	})

	// Calculate retention cutoff time
	var retentionCutoff time.Time
	if d.cfg.Disk.RetentionDays > 0 {
		retentionCutoff = time.Now().Add(-time.Duration(d.cfg.Disk.RetentionDays) * 24 * time.Hour)
	}

	// Delete clips that violate retention policy
	deleted := 0
	for _, clip := range clips {
		shouldDelete := false
		reason := ""

		// Check age-based retention
		if d.cfg.Disk.RetentionDays > 0 && clip.ModTime.Before(retentionCutoff) {
			shouldDelete = true
			reason = "age"
		}

		// Check size-based retention (delete oldest if over limit)
		if d.cfg.Disk.MaxBytesPerCamera > 0 && uint64(totalSize) > d.cfg.Disk.MaxBytesPerCamera {
			shouldDelete = true
			if reason == "" {
				reason = "quota"
			} else {
				reason = "age+quota"
			}
		}

		if shouldDelete {
			if err := os.Remove(clip.Path); err != nil {
				log.Printf("DISK: failed to delete %s: %v", clip.Path, err)
			} else {
				log.Printf("DISK: deleted %s (%.1fMB, age=%.1fd, reason=%s)",
					filepath.Base(clip.Path),
					float64(clip.Size)/1024/1024,
					time.Since(clip.ModTime).Hours()/24,
					reason)
				totalSize -= clip.Size
				deleted++
				d.metrics.DiskClipsDeleted.Inc()
			}
		}

		// Stop if we're now under quota
		if d.cfg.Disk.MaxBytesPerCamera > 0 && uint64(totalSize) <= d.cfg.Disk.MaxBytesPerCamera {
			break
		}
	}

	// Update metrics after cleanup
	remainingClips := len(clips) - deleted
	d.metrics.UpdateDiskMetrics(camName, uint64(totalSize), remainingClips)

	if deleted > 0 {
		log.Printf("DISK: cleanup complete for %s: deleted=%d remaining=%d size=%.1fMB",
			camName, deleted, remainingClips, float64(totalSize)/1024/1024)
	}
}

// scanClipDirectory scans a directory and returns clip metadata.
func (d *DiskManager) scanClipDirectory(dir string) ([]ClipInfo, int64, error) {
	var clips []ClipInfo
	var totalSize int64

	err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}

		// Skip directories
		if info.IsDir() {
			return nil
		}

		// Only consider clip files (common extensions)
		ext := filepath.Ext(path)
		if ext == ".ts" || ext == ".mp4" || ext == ".mkv" {
			clips = append(clips, ClipInfo{
				Path:    path,
				Size:    info.Size(),
				ModTime: info.ModTime(),
			})
			totalSize += info.Size()
		}

		return nil
	})

	return clips, totalSize, err
}
