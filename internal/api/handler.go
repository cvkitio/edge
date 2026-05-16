// Package api provides HTTP endpoints for runtime configuration and monitoring.
package api

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/agent"
)

// Handler provides HTTP endpoints for the agent API.
type Handler struct {
	supervisor *agent.Supervisor
}

// NewHandler creates a new API handler.
func NewHandler(supervisor *agent.Supervisor) *Handler {
	return &Handler{supervisor: supervisor}
}

// InspectorConfigResponse represents the inspector configuration in JSON.
type InspectorConfigResponse struct {
	MotionZHigh          float64 `json:"motion_z_high"`
	IntraRatioHigh       float64 `json:"intra_ratio_high"`
	OnThreshold          uint8   `json:"on_threshold"`
	OffThreshold         uint8   `json:"off_threshold"`
	BPFFloor             float64 `json:"bpf_floor"`
	ConfiguredPeriodicKF bool    `json:"configured_periodic_kf"`
	GradualEnabled       bool    `json:"gradual_enabled"`
	GradualThreshold     float64 `json:"gradual_threshold"`
	GradualWindowFrames  uint32  `json:"gradual_window_frames"`
}

// InspectorConfigRequest represents a configuration update request.
type InspectorConfigRequest struct {
	MotionZHigh          *float64 `json:"motion_z_high,omitempty"`
	IntraRatioHigh       *float64 `json:"intra_ratio_high,omitempty"`
	OnThreshold          *uint8   `json:"on_threshold,omitempty"`
	OffThreshold         *uint8   `json:"off_threshold,omitempty"`
	BPFFloor             *float64 `json:"bpf_floor,omitempty"`
	ConfiguredPeriodicKF *bool    `json:"configured_periodic_kf,omitempty"`
	GradualEnabled       *bool    `json:"gradual_enabled,omitempty"`
	GradualThreshold     *float64 `json:"gradual_threshold,omitempty"`
	GradualWindowFrames  *uint32  `json:"gradual_window_frames,omitempty"`
}

// ErrorResponse represents an error response.
type ErrorResponse struct {
	Error string `json:"error"`
}

// SuccessResponse represents a success response.
type SuccessResponse struct {
	Success bool   `json:"success"`
	Message string `json:"message"`
}

// CameraListResponse represents the list of cameras.
type CameraListResponse struct {
	Cameras []string `json:"cameras"`
}

// RegisterRoutes registers all API routes on the given mux.
func (h *Handler) RegisterRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/cameras", h.handleCameras)
	mux.HandleFunc("/api/cameras/", h.handleCameraConfig)
	mux.HandleFunc("/health", h.handleHealth)
}

// handleHealth returns a simple health check.
func (h *Handler) handleHealth(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

// handleCameras returns the list of cameras.
func (h *Handler) handleCameras(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	cameras := h.supervisor.GetCameraNames()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(CameraListResponse{Cameras: cameras})
}

// handleCameraConfig handles GET and PUT requests for camera configuration.
func (h *Handler) handleCameraConfig(w http.ResponseWriter, r *http.Request) {
	// Extract camera name from path: /api/cameras/{name}/config
	path := strings.TrimPrefix(r.URL.Path, "/api/cameras/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 || parts[1] != "config" {
		http.Error(w, "Not found", http.StatusNotFound)
		return
	}

	camName := parts[0]
	if camName == "" {
		http.Error(w, "Camera name required", http.StatusBadRequest)
		return
	}

	switch r.Method {
	case http.MethodGet:
		h.getInspectorConfig(w, camName)
	case http.MethodPut:
		h.updateInspectorConfig(w, r, camName)
	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// getInspectorConfig returns the current inspector configuration for a camera.
func (h *Handler) getInspectorConfig(w http.ResponseWriter, camName string) {
	recorder := h.supervisor.GetRecorder()
	cfg, err := recorder.GetInspectorConfig(camName)
	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: err.Error()})
		return
	}

	response := InspectorConfigResponse{
		MotionZHigh:          cfg.MotionZHigh,
		IntraRatioHigh:       cfg.IntraRatioHigh,
		OnThreshold:          cfg.OnThreshold,
		OffThreshold:         cfg.OffThreshold,
		BPFFloor:             cfg.BPFFloor,
		ConfiguredPeriodicKF: cfg.ConfiguredPeriodicKF,
		GradualEnabled:       cfg.GradualEnabled,
		GradualThreshold:     cfg.GradualThreshold,
		GradualWindowFrames:  cfg.GradualWindowFrames,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

// updateInspectorConfig updates the inspector configuration for a camera.
func (h *Handler) updateInspectorConfig(w http.ResponseWriter, r *http.Request, camName string) {
	recorder := h.supervisor.GetRecorder()

	// Get current config
	currentCfg, err := recorder.GetInspectorConfig(camName)
	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: err.Error()})
		return
	}

	// Parse request body
	var req InspectorConfigRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: fmt.Sprintf("Invalid JSON: %v", err)})
		return
	}

	// Apply updates (only fields that were provided)
	if req.MotionZHigh != nil {
		currentCfg.MotionZHigh = *req.MotionZHigh
	}
	if req.IntraRatioHigh != nil {
		currentCfg.IntraRatioHigh = *req.IntraRatioHigh
	}
	if req.OnThreshold != nil {
		currentCfg.OnThreshold = *req.OnThreshold
	}
	if req.OffThreshold != nil {
		currentCfg.OffThreshold = *req.OffThreshold
	}
	if req.BPFFloor != nil {
		currentCfg.BPFFloor = *req.BPFFloor
	}
	if req.ConfiguredPeriodicKF != nil {
		currentCfg.ConfiguredPeriodicKF = *req.ConfiguredPeriodicKF
	}
	if req.GradualEnabled != nil {
		currentCfg.GradualEnabled = *req.GradualEnabled
	}
	if req.GradualThreshold != nil {
		currentCfg.GradualThreshold = *req.GradualThreshold
	}
	if req.GradualWindowFrames != nil {
		currentCfg.GradualWindowFrames = *req.GradualWindowFrames
	}

	// Validate parameters
	if currentCfg.MotionZHigh < 0 || currentCfg.MotionZHigh > 100 {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "motion_z_high must be between 0 and 100"})
		return
	}
	if currentCfg.IntraRatioHigh < 0 || currentCfg.IntraRatioHigh > 100 {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "intra_ratio_high must be between 0 and 100"})
		return
	}
	if currentCfg.OnThreshold == 0 || currentCfg.OnThreshold > 255 {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "on_threshold must be between 1 and 255"})
		return
	}
	if currentCfg.OffThreshold == 0 || currentCfg.OffThreshold > 255 {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "off_threshold must be between 1 and 255"})
		return
	}

	// Update configuration
	if err := recorder.UpdateInspectorConfig(camName, currentCfg); err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusInternalServerError)
		json.NewEncoder(w).Encode(ErrorResponse{Error: fmt.Sprintf("Failed to update config: %v", err)})
		return
	}

	log.Printf("API: updated inspector config for camera %s (motion_z_high=%.1f, on_threshold=%d)",
		camName, currentCfg.MotionZHigh, currentCfg.OnThreshold)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(SuccessResponse{
		Success: true,
		Message: fmt.Sprintf("Configuration updated for camera %s", camName),
	})
}
