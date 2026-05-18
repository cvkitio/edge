package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// TestHandleClipsList tests the clips listing endpoint.
func TestHandleClipsList(t *testing.T) {
	// Create temporary clip directory
	tmpDir := t.TempDir()

	// Create test clip files
	createTestClip(t, tmpDir, "camera1", "clip1.ts", 1024*1024)
	createTestClip(t, tmpDir, "camera1", "clip2.ts", 2*1024*1024)
	createTestClip(t, tmpDir, "camera2", "clip3.mp4", 512*1024)

	handler := &Handler{
		supervisor: nil, // Not needed for clip listing
		clipRoot:   tmpDir,
	}

	tests := []struct {
		name        string
		query       string
		wantStatus  int
		wantClips   int
		wantCamera  string
	}{
		{
			name:       "list all clips",
			query:      "",
			wantStatus: http.StatusOK,
			wantClips:  3,
		},
		{
			name:       "filter by camera1",
			query:      "?camera=camera1",
			wantStatus: http.StatusOK,
			wantClips:  2,
			wantCamera: "camera1",
		},
		{
			name:       "filter by camera2",
			query:      "?camera=camera2",
			wantStatus: http.StatusOK,
			wantClips:  1,
			wantCamera: "camera2",
		},
		{
			name:       "filter by nonexistent camera",
			query:      "?camera=nonexistent",
			wantStatus: http.StatusOK,
			wantClips:  0,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, "/api/clips"+tt.query, nil)
			w := httptest.NewRecorder()

			handler.handleClipsList(w, req)

			resp := w.Result()
			if resp.StatusCode != tt.wantStatus {
				t.Errorf("expected status %d, got %d", tt.wantStatus, resp.StatusCode)
			}

			var clipsList ClipsListResponse
			if err := json.NewDecoder(resp.Body).Decode(&clipsList); err != nil {
				t.Fatalf("failed to decode response: %v", err)
			}

			if len(clipsList.Clips) != tt.wantClips {
				t.Errorf("expected %d clips, got %d", tt.wantClips, len(clipsList.Clips))
			}

			if clipsList.Total != tt.wantClips {
				t.Errorf("expected total=%d, got %d", tt.wantClips, clipsList.Total)
			}

			// Verify camera filter worked
			if tt.wantCamera != "" {
				for _, clip := range clipsList.Clips {
					if clip.Camera != tt.wantCamera {
						t.Errorf("expected camera=%s, got %s", tt.wantCamera, clip.Camera)
					}
				}
			}

			// Verify clip metadata
			for _, clip := range clipsList.Clips {
				if clip.Size == 0 {
					t.Error("clip size should not be zero")
				}
				if clip.URL == "" {
					t.Error("clip URL should not be empty")
				}
				if clip.Filename == "" {
					t.Error("clip filename should not be empty")
				}
			}
		})
	}
}

// TestHandleClipFile tests the clip file serving endpoint.
func TestHandleClipFile(t *testing.T) {
	tmpDir := t.TempDir()

	// Create test files
	createTestClip(t, tmpDir, "camera1", "test.ts", 1024)
	createTestClip(t, tmpDir, "camera1", "test.mp4", 2048)

	handler := &Handler{
		supervisor: nil,
		clipRoot:   tmpDir,
	}

	tests := []struct {
		name           string
		path           string
		wantStatus     int
		wantType       string
		checkSize      bool
	}{
		{
			name:       "serve ts file",
			path:       "/api/clips/camera1/test.ts",
			wantStatus: http.StatusOK,
			wantType:   "video/mp2t",
			checkSize:  true,
		},
		{
			name:       "serve mp4 file",
			path:       "/api/clips/camera1/test.mp4",
			wantStatus: http.StatusOK,
			wantType:   "video/mp4",
			checkSize:  true,
		},
		{
			name:       "file not found",
			path:       "/api/clips/camera1/nonexistent.ts",
			wantStatus: http.StatusNotFound,
		},
		{
			name:       "invalid path",
			path:       "/api/clips/invalid",
			wantStatus: http.StatusBadRequest,
		},
		{
			name:       "path traversal attempt",
			path:       "/api/clips/../../../etc/passwd",
			wantStatus: http.StatusBadRequest,
		},
		{
			name:       "path traversal in filename",
			path:       "/api/clips/camera1/../../../etc/passwd",
			wantStatus: http.StatusBadRequest,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, tt.path, nil)
			w := httptest.NewRecorder()

			handler.handleClipFile(w, req)

			resp := w.Result()
			if resp.StatusCode != tt.wantStatus {
				t.Errorf("expected status %d, got %d", tt.wantStatus, resp.StatusCode)
			}

			if tt.wantType != "" {
				contentType := resp.Header.Get("Content-Type")
				if contentType != tt.wantType {
					t.Errorf("expected Content-Type %s, got %s", tt.wantType, contentType)
				}
			}

			if tt.wantStatus == http.StatusOK {
				// Verify headers for streaming
				if acceptRanges := resp.Header.Get("Accept-Ranges"); acceptRanges != "bytes" {
					t.Errorf("expected Accept-Ranges: bytes, got %s", acceptRanges)
				}
				if cacheControl := resp.Header.Get("Cache-Control"); cacheControl == "" {
					t.Error("expected Cache-Control header to be set")
				}
			}

			if tt.checkSize && resp.StatusCode == http.StatusOK {
				if resp.Body == nil {
					t.Error("expected response body, got nil")
				}
			}
		})
	}
}

// TestHandleClipFileHead tests HEAD requests for clip files.
func TestHandleClipFileHead(t *testing.T) {
	tmpDir := t.TempDir()
	createTestClip(t, tmpDir, "camera1", "test.ts", 1024)

	handler := &Handler{
		supervisor: nil,
		clipRoot:   tmpDir,
	}

	req := httptest.NewRequest(http.MethodHead, "/api/clips/camera1/test.ts", nil)
	w := httptest.NewRecorder()

	handler.handleClipFile(w, req)

	resp := w.Result()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("expected status 200, got %d", resp.StatusCode)
	}

	// HEAD should return headers but no body
	if w.Body.Len() > 0 {
		t.Error("HEAD request should not return body")
	}
}

// TestHandleWebUI tests the web UI endpoint.
func TestHandleWebUI(t *testing.T) {
	handler := &Handler{
		supervisor: nil,
		clipRoot:   t.TempDir(),
	}

	tests := []struct {
		name       string
		path       string
		wantStatus int
		checkHTML  bool
	}{
		{
			name:       "serve web UI",
			path:       "/",
			wantStatus: http.StatusOK,
			checkHTML:  true,
		},
		{
			name:       "404 for other paths",
			path:       "/some-other-path",
			wantStatus: http.StatusNotFound,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, tt.path, nil)
			w := httptest.NewRecorder()

			handler.handleWebUI(w, req)

			resp := w.Result()
			if resp.StatusCode != tt.wantStatus {
				t.Errorf("expected status %d, got %d", tt.wantStatus, resp.StatusCode)
			}

			if tt.checkHTML {
				contentType := resp.Header.Get("Content-Type")
				if contentType != "text/html; charset=utf-8" {
					t.Errorf("expected Content-Type text/html, got %s", contentType)
				}

				body := w.Body.String()
				if body == "" {
					t.Error("expected HTML content, got empty body")
				}

				// Verify key elements of the web UI
				expectedStrings := []string{
					"<!DOCTYPE html>",
					"EMD Agent",
					"Clip Browser",
					"/api/clips",
					"<video",
				}

				for _, expected := range expectedStrings {
					if !contains(body, expected) {
						t.Errorf("expected HTML to contain %q", expected)
					}
				}
			}
		})
	}
}

// TestScanClips tests the clip scanning functionality.
func TestScanClips(t *testing.T) {
	tmpDir := t.TempDir()

	// Create clips in different cameras
	createTestClip(t, tmpDir, "camera1", "old.ts", 1024)
	time.Sleep(10 * time.Millisecond)
	createTestClip(t, tmpDir, "camera1", "new.ts", 2048)
	createTestClip(t, tmpDir, "camera2", "clip.mp4", 512)

	// Create a non-video file (should be ignored)
	createTestFile(t, tmpDir, "camera1", "readme.txt", 100)

	handler := &Handler{
		supervisor: nil,
		clipRoot:   tmpDir,
	}

	// Test scanning all cameras
	clips, err := handler.scanClips("")
	if err != nil {
		t.Fatalf("scanClips failed: %v", err)
	}

	if len(clips) != 3 {
		t.Errorf("expected 3 clips, got %d", len(clips))
	}

	// Verify clips are sorted by mod time (newest first)
	if len(clips) >= 2 {
		if clips[0].ModTime.Before(clips[1].ModTime) {
			t.Error("clips should be sorted newest first")
		}
	}

	// Test scanning specific camera
	camera1Clips, err := handler.scanClips("camera1")
	if err != nil {
		t.Fatalf("scanClips for camera1 failed: %v", err)
	}

	if len(camera1Clips) != 2 {
		t.Errorf("expected 2 clips for camera1, got %d", len(camera1Clips))
	}

	for _, clip := range camera1Clips {
		if clip.Camera != "camera1" {
			t.Errorf("expected camera=camera1, got %s", clip.Camera)
		}
	}

	// Verify URL format
	for _, clip := range clips {
		expectedURL := "/api/clips/" + clip.Camera + "/" + clip.Filename
		if clip.URL != expectedURL {
			t.Errorf("expected URL %s, got %s", expectedURL, clip.URL)
		}
	}
}

// TestClipsSortOrder tests that clips are returned in correct order.
func TestClipsSortOrder(t *testing.T) {
	tmpDir := t.TempDir()

	// Create clips with specific timestamps
	times := []struct {
		name string
		age  time.Duration
	}{
		{"oldest.ts", 3 * time.Hour},
		{"middle.ts", 2 * time.Hour},
		{"newest.ts", 1 * time.Hour},
	}

	now := time.Now()
	for _, tc := range times {
		path := filepath.Join(tmpDir, "camera1", tc.name)
		if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("test"), 0644); err != nil {
			t.Fatal(err)
		}
		modTime := now.Add(-tc.age)
		if err := os.Chtimes(path, modTime, modTime); err != nil {
			t.Fatal(err)
		}
	}

	handler := &Handler{
		supervisor: nil,
		clipRoot:   tmpDir,
	}

	clips, err := handler.scanClips("")
	if err != nil {
		t.Fatalf("scanClips failed: %v", err)
	}

	if len(clips) != 3 {
		t.Fatalf("expected 3 clips, got %d", len(clips))
	}

	// Should be sorted newest first
	if clips[0].Filename != "newest.ts" {
		t.Errorf("expected first clip to be newest.ts, got %s", clips[0].Filename)
	}
	if clips[2].Filename != "oldest.ts" {
		t.Errorf("expected last clip to be oldest.ts, got %s", clips[2].Filename)
	}
}

// Helper functions

func createTestClip(t *testing.T, root, camera, filename string, size int64) {
	t.Helper()
	path := filepath.Join(root, camera, filename)
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	data := make([]byte, size)
	if err := os.WriteFile(path, data, 0644); err != nil {
		t.Fatal(err)
	}
}

func createTestFile(t *testing.T, root, camera, filename string, size int64) {
	t.Helper()
	createTestClip(t, root, camera, filename, size)
}

func contains(s, substr string) bool {
	return len(s) >= len(substr) && (s == substr || len(s) > len(substr) && containsAt(s, substr, 0))
}

func containsAt(s, substr string, start int) bool {
	for i := start; i <= len(s)-len(substr); i++ {
		if s[i:i+len(substr)] == substr {
			return true
		}
	}
	return false
}
