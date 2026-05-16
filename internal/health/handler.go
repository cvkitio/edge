// Package health provides Kubernetes health check endpoints.
package health

import (
	"encoding/json"
	"net/http"
	"sync"
	"time"
)

// Handler provides health check endpoints for Kubernetes.
type Handler struct {
	mu               sync.RWMutex
	cameraStatus     map[string]CameraStatus
	startTime        time.Time
	ready            bool
	readinessChecks  []ReadinessCheck
}

// CameraStatus represents the status of a camera.
type CameraStatus struct {
	Name      string    `json:"name"`
	Connected bool      `json:"connected"`
	LastSeen  time.Time `json:"last_seen"`
}

// ReadinessCheck is a function that checks if the system is ready.
type ReadinessCheck func() bool

// NewHandler creates a new health check handler.
func NewHandler() *Handler {
	return &Handler{
		cameraStatus:    make(map[string]CameraStatus),
		startTime:       time.Now(),
		ready:           false,
		readinessChecks: make([]ReadinessCheck, 0),
	}
}

// RegisterRoutes registers health check routes on the given mux.
func (h *Handler) RegisterRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/healthz", h.handleLiveness)
	mux.HandleFunc("/readyz", h.handleReadiness)
	mux.HandleFunc("/health/cameras", h.handleCameraStatus)
}

// SetReady marks the system as ready.
func (h *Handler) SetReady(ready bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.ready = ready
}

// AddReadinessCheck adds a custom readiness check.
func (h *Handler) AddReadinessCheck(check ReadinessCheck) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.readinessChecks = append(h.readinessChecks, check)
}

// UpdateCameraStatus updates the status of a camera.
func (h *Handler) UpdateCameraStatus(name string, connected bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.cameraStatus[name] = CameraStatus{
		Name:      name,
		Connected: connected,
		LastSeen:  time.Now(),
	}
}

// handleLiveness handles the /healthz endpoint (liveness probe).
// This always returns 200 OK if the process is running.
func (h *Handler) handleLiveness(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"status": "ok",
		"uptime": time.Since(h.startTime).Seconds(),
	})
}

// handleReadiness handles the /readyz endpoint (readiness probe).
// Returns 200 OK if the system is ready to serve traffic.
func (h *Handler) handleReadiness(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	h.mu.RLock()
	ready := h.ready
	cameraCount := len(h.cameraStatus)
	connectedCount := 0
	for _, status := range h.cameraStatus {
		if status.Connected {
			connectedCount++
		}
	}

	// Run custom readiness checks
	allChecksPass := true
	for _, check := range h.readinessChecks {
		if !check() {
			allChecksPass = false
			break
		}
	}
	h.mu.RUnlock()

	// System is ready if:
	// 1. Ready flag is set
	// 2. At least one camera is connected
	// 3. All custom checks pass
	isReady := ready && connectedCount > 0 && allChecksPass

	status := "ready"
	httpStatus := http.StatusOK
	if !isReady {
		status = "not ready"
		httpStatus = http.StatusServiceUnavailable
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(httpStatus)
	json.NewEncoder(w).Encode(map[string]interface{}{
		"status":          status,
		"ready":           isReady,
		"cameras_total":   cameraCount,
		"cameras_connected": connectedCount,
		"uptime":          time.Since(h.startTime).Seconds(),
	})
}

// handleCameraStatus handles the /health/cameras endpoint.
// Returns detailed status of all cameras.
func (h *Handler) handleCameraStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	h.mu.RLock()
	cameras := make([]CameraStatus, 0, len(h.cameraStatus))
	for _, status := range h.cameraStatus {
		cameras = append(cameras, status)
	}
	h.mu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"cameras": cameras,
		"total":   len(cameras),
	})
}
