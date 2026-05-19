package health

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

// TestHandleLiveness tests the /healthz endpoint.
func TestHandleLiveness(t *testing.T) {
	tests := []struct {
		name       string
		method     string
		wantStatus int
		wantHealth bool
	}{
		{
			name:       "GET request returns healthy",
			method:     http.MethodGet,
			wantStatus: http.StatusOK,
			wantHealth: true,
		},
		{
			name:       "POST request not allowed",
			method:     http.MethodPost,
			wantStatus: http.StatusMethodNotAllowed,
			wantHealth: false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			handler := NewHandler()
			req := httptest.NewRequest(tt.method, "/healthz", nil)
			w := httptest.NewRecorder()

			handler.handleLiveness(w, req)

			resp := w.Result()
			if resp.StatusCode != tt.wantStatus {
				t.Errorf("expected status %d, got %d", tt.wantStatus, resp.StatusCode)
			}

			if tt.wantHealth && tt.method == http.MethodGet {
				contentType := resp.Header.Get("Content-Type")
				if contentType != "application/json" {
					t.Errorf("expected Content-Type application/json, got %s", contentType)
				}

				var response map[string]interface{}
				if err := json.NewDecoder(resp.Body).Decode(&response); err != nil {
					t.Fatalf("failed to decode response: %v", err)
				}

				if response["status"] != "ok" {
					t.Errorf("expected status 'ok', got '%v'", response["status"])
				}

				if _, ok := response["uptime"]; !ok {
					t.Error("expected uptime field in response")
				}
			}
		})
	}
}

// TestHandleReadiness tests the /readyz endpoint.
func TestHandleReadiness(t *testing.T) {
	tests := []struct {
		name          string
		method        string
		ready         bool
		cameras       map[string]bool // name -> connected
		wantStatus    int
		wantReady     bool
	}{
		{
			name:   "all cameras connected and ready",
			method: http.MethodGet,
			ready:  true,
			cameras: map[string]bool{
				"camera1": true,
				"camera2": true,
			},
			wantStatus: http.StatusOK,
			wantReady:  true,
		},
		{
			name:   "some cameras disconnected but at least one connected",
			method: http.MethodGet,
			ready:  true,
			cameras: map[string]bool{
				"camera1": true,
				"camera2": false,
			},
			wantStatus: http.StatusOK,
			wantReady:  true,
		},
		{
			name:   "all cameras disconnected",
			method: http.MethodGet,
			ready:  true,
			cameras: map[string]bool{
				"camera1": false,
				"camera2": false,
			},
			wantStatus: http.StatusServiceUnavailable,
			wantReady:  false,
		},
		{
			name:       "no cameras connected",
			method:     http.MethodGet,
			ready:      true,
			cameras:    map[string]bool{},
			wantStatus: http.StatusServiceUnavailable,
			wantReady:  false,
		},
		{
			name:   "not marked ready yet",
			method: http.MethodGet,
			ready:  false,
			cameras: map[string]bool{
				"camera1": true,
			},
			wantStatus: http.StatusServiceUnavailable,
			wantReady:  false,
		},
		{
			name:   "POST not allowed",
			method: http.MethodPost,
			ready:  true,
			cameras: map[string]bool{
				"camera1": true,
			},
			wantStatus: http.StatusMethodNotAllowed,
			wantReady:  false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			handler := NewHandler()
			handler.SetReady(tt.ready)

			for name, connected := range tt.cameras {
				handler.UpdateCameraStatus(name, connected)
			}

			req := httptest.NewRequest(tt.method, "/readyz", nil)
			w := httptest.NewRecorder()

			handler.handleReadiness(w, req)

			resp := w.Result()
			if resp.StatusCode != tt.wantStatus {
				t.Errorf("expected status %d, got %d", tt.wantStatus, resp.StatusCode)
			}

			if tt.method == http.MethodGet {
				var response map[string]interface{}
				if err := json.NewDecoder(resp.Body).Decode(&response); err != nil {
					t.Fatalf("failed to decode response: %v", err)
				}

				if tt.wantReady {
					if response["status"] != "ready" {
						t.Errorf("expected status 'ready', got '%v'", response["status"])
					}
					if response["ready"] != true {
						t.Error("expected ready=true")
					}
				} else if tt.wantStatus == http.StatusServiceUnavailable {
					if response["status"] != "not ready" {
						t.Errorf("expected status 'not ready', got '%v'", response["status"])
					}
				}

				// Verify response includes expected fields
				if _, ok := response["cameras_total"]; !ok {
					t.Error("expected cameras_total field")
				}
				if _, ok := response["cameras_connected"]; !ok {
					t.Error("expected cameras_connected field")
				}
				if _, ok := response["uptime"]; !ok {
					t.Error("expected uptime field")
				}
			}
		})
	}
}

// TestConcurrentHealthUpdates tests thread-safety of health updates.
func TestConcurrentHealthUpdates(t *testing.T) {
	handler := NewHandler()
	handler.SetReady(true)

	// Simulate concurrent updates from multiple goroutines
	done := make(chan bool)
	for i := 0; i < 10; i++ {
		go func(id int) {
			handler.UpdateCameraStatus("camera1", id%2 == 0)
			handler.UpdateCameraStatus("camera2", id%3 == 0)
			done <- true
		}(i)
	}

	// Wait for all goroutines
	for i := 0; i < 10; i++ {
		<-done
	}

	// Verify we can still query health without panic
	req := httptest.NewRequest(http.MethodGet, "/readyz", nil)
	w := httptest.NewRecorder()
	handler.handleReadiness(w, req)

	if w.Code != http.StatusOK && w.Code != http.StatusServiceUnavailable {
		t.Errorf("unexpected status code after concurrent updates: %d", w.Code)
	}
}

// TestCameraStatus tests camera status updates and retrieval.
func TestCameraStatus(t *testing.T) {
	handler := NewHandler()

	// Update camera status
	handler.UpdateCameraStatus("camera1", true)
	handler.UpdateCameraStatus("camera2", false)

	// Wait a bit to ensure different timestamps
	time.Sleep(10 * time.Millisecond)
	handler.UpdateCameraStatus("camera1", false)

	req := httptest.NewRequest(http.MethodGet, "/health/cameras", nil)
	w := httptest.NewRecorder()

	handler.handleCameraStatus(w, req)

	resp := w.Result()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("expected status 200, got %d", resp.StatusCode)
	}

	var response map[string]interface{}
	if err := json.NewDecoder(resp.Body).Decode(&response); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}

	cameras, ok := response["cameras"].([]interface{})
	if !ok {
		t.Fatal("expected cameras field to be an array")
	}

	if len(cameras) != 2 {
		t.Errorf("expected 2 cameras, got %d", len(cameras))
	}

	total, ok := response["total"].(float64)
	if !ok || int(total) != 2 {
		t.Errorf("expected total=2, got %v", response["total"])
	}
}

// TestRegisterRoutes tests route registration.
func TestRegisterRoutes(t *testing.T) {
	handler := NewHandler()
	handler.SetReady(true)
	handler.UpdateCameraStatus("test", true)

	mux := http.NewServeMux()
	handler.RegisterRoutes(mux)

	// Test that routes are registered
	tests := []struct {
		path   string
		method string
	}{
		{"/healthz", http.MethodGet},
		{"/readyz", http.MethodGet},
		{"/health/cameras", http.MethodGet},
	}

	for _, tt := range tests {
		req := httptest.NewRequest(tt.method, tt.path, nil)
		w := httptest.NewRecorder()

		mux.ServeHTTP(w, req)

		// Should not return 404
		if w.Code == http.StatusNotFound {
			t.Errorf("route %s not registered", tt.path)
		}
	}
}

// TestReadinessChecks tests custom readiness check functions.
func TestReadinessChecks(t *testing.T) {
	handler := NewHandler()
	handler.SetReady(true)
	handler.UpdateCameraStatus("camera1", true)

	// Add a passing readiness check
	handler.AddReadinessCheck(func() bool {
		return true
	})

	req := httptest.NewRequest(http.MethodGet, "/readyz", nil)
	w := httptest.NewRecorder()
	handler.handleReadiness(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("expected status 200 with passing check, got %d", w.Code)
	}

	// Add a failing readiness check
	handler.AddReadinessCheck(func() bool {
		return false
	})

	req = httptest.NewRequest(http.MethodGet, "/readyz", nil)
	w = httptest.NewRecorder()
	handler.handleReadiness(w, req)

	if w.Code != http.StatusServiceUnavailable {
		t.Errorf("expected status 503 with failing check, got %d", w.Code)
	}
}
