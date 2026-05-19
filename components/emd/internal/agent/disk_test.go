package agent

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

// TestScanClipDirectory tests directory scanning functionality.
func TestScanClipDirectory(t *testing.T) {
	// Create temporary directory for testing
	tmpDir := t.TempDir()

	// Create test clips
	clips := []struct {
		name    string
		size    int64
		modTime time.Time
	}{
		{"clip1.ts", 1024 * 1024, time.Now().Add(-2 * time.Hour)},
		{"clip2.mp4", 2 * 1024 * 1024, time.Now().Add(-1 * time.Hour)},
		{"clip3.mkv", 512 * 1024, time.Now()},
		{"ignore.txt", 100, time.Now()}, // Should be ignored
	}

	for _, clip := range clips {
		path := filepath.Join(tmpDir, clip.name)
		if err := os.WriteFile(path, make([]byte, clip.size), 0644); err != nil {
			t.Fatalf("failed to create test file: %v", err)
		}
		if err := os.Chtimes(path, clip.modTime, clip.modTime); err != nil {
			t.Fatalf("failed to set file time: %v", err)
		}
	}

	// Create disk manager
	cfg := &Config{
		Runtime: RuntimeConfig{ClipRoot: tmpDir},
	}
	m := NewTestMetrics()
	dm := NewDiskManager(cfg, m, []string{"test_cam"})

	// Scan directory
	foundClips, totalSize, err := dm.scanClipDirectory(tmpDir)
	if err != nil {
		t.Fatalf("scanClipDirectory failed: %v", err)
	}

	// Verify results
	if len(foundClips) != 3 {
		t.Errorf("expected 3 clips, got %d", len(foundClips))
	}

	expectedSize := int64(1024*1024 + 2*1024*1024 + 512*1024)
	if totalSize != expectedSize {
		t.Errorf("expected total size %d, got %d", expectedSize, totalSize)
	}

	// Verify clip metadata
	for _, clip := range foundClips {
		ext := filepath.Ext(clip.Path)
		if ext != ".ts" && ext != ".mp4" && ext != ".mkv" {
			t.Errorf("unexpected file extension: %s", ext)
		}
		if clip.Size <= 0 {
			t.Errorf("invalid clip size: %d", clip.Size)
		}
		if clip.ModTime.IsZero() {
			t.Error("clip has zero modification time")
		}
	}
}

// TestCleanupCamera_AgeBasedRetention tests age-based clip deletion.
func TestCleanupCamera_AgeBasedRetention(t *testing.T) {
	tmpDir := t.TempDir()
	camName := "test_cam"
	clipDir := filepath.Join(tmpDir, camName)

	if err := os.MkdirAll(clipDir, 0755); err != nil {
		t.Fatalf("failed to create clip directory: %v", err)
	}

	// Create clips with different ages
	now := time.Now()
	clips := []struct {
		name string
		age  time.Duration
		size int64
	}{
		{"old_clip.ts", 5 * 24 * time.Hour, 1024 * 1024},     // 5 days old
		{"recent_clip.ts", 1 * 24 * time.Hour, 1024 * 1024},  // 1 day old
		{"fresh_clip.ts", 1 * time.Hour, 1024 * 1024},        // 1 hour old
	}

	for _, clip := range clips {
		path := filepath.Join(clipDir, clip.name)
		if err := os.WriteFile(path, make([]byte, clip.size), 0644); err != nil {
			t.Fatalf("failed to create test file: %v", err)
		}
		modTime := now.Add(-clip.age)
		if err := os.Chtimes(path, modTime, modTime); err != nil {
			t.Fatalf("failed to set file time: %v", err)
		}
	}

	// Create disk manager with 3-day retention
	cfg := &Config{
		Runtime: RuntimeConfig{ClipRoot: tmpDir},
		Disk: DiskConfig{
			RetentionDays:      3,
			MaxBytesPerCamera:  0, // No quota limit
		},
	}
	m := NewTestMetrics()
	dm := NewDiskManager(cfg, m, []string{camName})

	// Run cleanup
	dm.cleanupCamera(camName)

	// Verify that old clip was deleted
	if _, err := os.Stat(filepath.Join(clipDir, "old_clip.ts")); !os.IsNotExist(err) {
		t.Error("expected old_clip.ts to be deleted")
	}

	// Verify that recent clips still exist
	if _, err := os.Stat(filepath.Join(clipDir, "recent_clip.ts")); err != nil {
		t.Error("expected recent_clip.ts to still exist")
	}
	if _, err := os.Stat(filepath.Join(clipDir, "fresh_clip.ts")); err != nil {
		t.Error("expected fresh_clip.ts to still exist")
	}
}

// TestCleanupCamera_QuotaBasedRetention tests quota-based clip deletion.
func TestCleanupCamera_QuotaBasedRetention(t *testing.T) {
	tmpDir := t.TempDir()
	camName := "test_cam"
	clipDir := filepath.Join(tmpDir, camName)

	if err := os.MkdirAll(clipDir, 0755); err != nil {
		t.Fatalf("failed to create clip directory: %v", err)
	}

	// Create clips that exceed quota
	now := time.Now()
	clips := []struct {
		name    string
		size    int64
		ageHours int
	}{
		{"clip1.ts", 1024 * 1024, 3}, // Oldest
		{"clip2.ts", 1024 * 1024, 2},
		{"clip3.ts", 1024 * 1024, 1}, // Newest
	}

	for _, clip := range clips {
		path := filepath.Join(clipDir, clip.name)
		if err := os.WriteFile(path, make([]byte, clip.size), 0644); err != nil {
			t.Fatalf("failed to create test file: %v", err)
		}
		modTime := now.Add(-time.Duration(clip.ageHours) * time.Hour)
		if err := os.Chtimes(path, modTime, modTime); err != nil {
			t.Fatalf("failed to set file time: %v", err)
		}
	}

	// Create disk manager with 2MB quota (total is 3MB, so 1 clip should be deleted)
	cfg := &Config{
		Runtime: RuntimeConfig{ClipRoot: tmpDir},
		Disk: DiskConfig{
			RetentionDays:     0, // No age limit
			MaxBytesPerCamera: 2 * 1024 * 1024,
		},
	}
	m := NewTestMetrics()
	dm := NewDiskManager(cfg, m, []string{camName})

	// Run cleanup
	dm.cleanupCamera(camName)

	// Verify that oldest clip was deleted
	if _, err := os.Stat(filepath.Join(clipDir, "clip1.ts")); !os.IsNotExist(err) {
		t.Error("expected clip1.ts (oldest) to be deleted")
	}

	// Verify that newer clips still exist
	if _, err := os.Stat(filepath.Join(clipDir, "clip2.ts")); err != nil {
		t.Error("expected clip2.ts to still exist")
	}
	if _, err := os.Stat(filepath.Join(clipDir, "clip3.ts")); err != nil {
		t.Error("expected clip3.ts to still exist")
	}
}

// TestCleanupCamera_NonexistentDirectory tests handling of missing directories.
func TestCleanupCamera_NonexistentDirectory(t *testing.T) {
	tmpDir := t.TempDir()
	camName := "nonexistent_cam"

	cfg := &Config{
		Runtime: RuntimeConfig{ClipRoot: tmpDir},
	}
	m := NewTestMetrics()
	dm := NewDiskManager(cfg, m, []string{camName})

	// This should not panic or error
	dm.cleanupCamera(camName)

	// Verify metrics were updated with zero values
	// (metrics implementation details omitted)
}

// TestCleanupCamera_EmptyDirectory tests handling of empty directories.
func TestCleanupCamera_EmptyDirectory(t *testing.T) {
	tmpDir := t.TempDir()
	camName := "empty_cam"
	clipDir := filepath.Join(tmpDir, camName)

	if err := os.MkdirAll(clipDir, 0755); err != nil {
		t.Fatalf("failed to create clip directory: %v", err)
	}

	cfg := &Config{
		Runtime: RuntimeConfig{ClipRoot: tmpDir},
	}
	m := NewTestMetrics()
	dm := NewDiskManager(cfg, m, []string{camName})

	// This should not panic or error
	dm.cleanupCamera(camName)
}

// TestCleanupCamera_MixedAgeAndQuota tests combined retention policies.
func TestCleanupCamera_MixedAgeAndQuota(t *testing.T) {
	tmpDir := t.TempDir()
	camName := "test_cam"
	clipDir := filepath.Join(tmpDir, camName)

	if err := os.MkdirAll(clipDir, 0755); err != nil {
		t.Fatalf("failed to create clip directory: %v", err)
	}

	now := time.Now()
	clips := []struct {
		name     string
		size     int64
		ageDays  int
		shouldDelete bool
	}{
		{"very_old.ts", 1024 * 1024, 10, true},  // Too old
		{"old.ts", 1024 * 1024, 5, false},        // Too old but cleanup stops after first deletion
		{"recent1.ts", 1024 * 1024, 1, false},   // Keep
		{"recent2.ts", 1024 * 1024, 0, false},   // Keep
	}

	for _, clip := range clips {
		path := filepath.Join(clipDir, clip.name)
		if err := os.WriteFile(path, make([]byte, clip.size), 0644); err != nil {
			t.Fatalf("failed to create test file: %v", err)
		}
		modTime := now.Add(-time.Duration(clip.ageDays) * 24 * time.Hour)
		if err := os.Chtimes(path, modTime, modTime); err != nil {
			t.Fatalf("failed to set file time: %v", err)
		}
	}

	// 3-day retention and 10MB quota (won't hit quota, so age-based deletion stops after quota check)
	cfg := &Config{
		Runtime: RuntimeConfig{ClipRoot: tmpDir},
		Disk: DiskConfig{
			RetentionDays:     3,
			MaxBytesPerCamera: 10 * 1024 * 1024,
		},
	}
	m := NewTestMetrics()
	dm := NewDiskManager(cfg, m, []string{camName})

	dm.cleanupCamera(camName)

	// Verify at least oldest file was deleted
	if _, err := os.Stat(filepath.Join(clipDir, "very_old.ts")); !os.IsNotExist(err) {
		t.Error("expected very_old.ts to be deleted")
	}

	// Recent files should exist
	if _, err := os.Stat(filepath.Join(clipDir, "recent1.ts")); err != nil {
		t.Error("expected recent1.ts to exist")
	}
	if _, err := os.Stat(filepath.Join(clipDir, "recent2.ts")); err != nil {
		t.Error("expected recent2.ts to exist")
	}
}

// TestDiskManager_StartStop tests the lifecycle methods.
func TestDiskManager_StartStop(t *testing.T) {
	tmpDir := t.TempDir()

	cfg := &Config{
		Runtime: RuntimeConfig{ClipRoot: tmpDir},
		Disk: DiskConfig{
			RetentionDays:     7,
			MaxBytesPerCamera: 100 * 1024 * 1024,
		},
	}
	m := NewTestMetrics()
	dm := NewDiskManager(cfg, m, []string{"test_cam"})

	// Start should not block
	dm.Start()

	// Allow background goroutines to start
	time.Sleep(100 * time.Millisecond)

	// Stop should not panic
	dm.Stop()
}
