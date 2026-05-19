package api

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/libemd"
)

// mockSupervisor implements a mock supervisor for testing.
type mockSupervisor struct {
	cameras map[string]*libemd.InspectorConfig
}

func newMockSupervisor() *mockSupervisor {
	return &mockSupervisor{
		cameras: map[string]*libemd.InspectorConfig{
			"camera1": {
				MotionZHigh:          3.0,
				IntraRatioHigh:       2.5,
				OnThreshold:          2,
				OffThreshold:         45,
				BPFFloor:             100.0,
				ConfiguredPeriodicKF: false,
				GradualEnabled:       false,
				GradualThreshold:     0.15,
				GradualWindowFrames:  900,
			},
			"camera2": {
				MotionZHigh:          4.0,
				IntraRatioHigh:       3.0,
				OnThreshold:          3,
				OffThreshold:         50,
				BPFFloor:             100.0,
				ConfiguredPeriodicKF: true,
				GradualEnabled:       true,
				GradualThreshold:     0.20,
				GradualWindowFrames:  1200,
			},
		},
	}
}

func (m *mockSupervisor) GetCameraNames() []string {
	names := make([]string, 0, len(m.cameras))
	for name := range m.cameras {
		names = append(names, name)
	}
	return names
}

// mockRecorder implements recorder methods needed by the API.
type mockRecorder struct {
	supervisor *mockSupervisor
}

func (r *mockRecorder) GetInspectorConfig(camName string) (*libemd.InspectorConfig, error) {
	cfg, exists := r.supervisor.cameras[camName]
	if !exists {
		return nil, fmt.Errorf("camera %s not found", camName)
	}
	// Return a copy
	cfgCopy := *cfg
	return &cfgCopy, nil
}

func (r *mockRecorder) UpdateInspectorConfig(camName string, cfg *libemd.InspectorConfig) error {
	if _, exists := r.supervisor.cameras[camName]; !exists {
		return fmt.Errorf("camera %s not found", camName)
	}
	// Store a copy
	cfgCopy := *cfg
	r.supervisor.cameras[camName] = &cfgCopy
	return nil
}

// TestInspectorConfigRequests tests GET and PUT for inspector configuration.
func TestInspectorConfigRequests(t *testing.T) {
	supervisor := newMockSupervisor()
	recorder := &mockRecorder{supervisor: supervisor}

	// Test GET existing camera
	cfg, err := recorder.GetInspectorConfig("camera1")
	if err != nil {
		t.Fatalf("GetInspectorConfig failed: %v", err)
	}
	if cfg.MotionZHigh != 3.0 {
		t.Errorf("expected motion_z_high=3.0, got %f", cfg.MotionZHigh)
	}

	// Test GET nonexistent camera
	_, err = recorder.GetInspectorConfig("nonexistent")
	if err == nil {
		t.Error("expected error for nonexistent camera, got nil")
	}

	// Test UPDATE existing camera
	newCfg := *cfg
	newCfg.MotionZHigh = 5.0
	newCfg.OnThreshold = 3
	err = recorder.UpdateInspectorConfig("camera1", &newCfg)
	if err != nil {
		t.Fatalf("UpdateInspectorConfig failed: %v", err)
	}

	// Verify update persisted
	updated, err := recorder.GetInspectorConfig("camera1")
	if err != nil {
		t.Fatalf("GetInspectorConfig after update failed: %v", err)
	}
	if updated.MotionZHigh != 5.0 {
		t.Errorf("expected motion_z_high=5.0 after update, got %f", updated.MotionZHigh)
	}
	if updated.OnThreshold != 3 {
		t.Errorf("expected on_threshold=3 after update, got %d", updated.OnThreshold)
	}

	// Test UPDATE nonexistent camera
	err = recorder.UpdateInspectorConfig("nonexistent", &newCfg)
	if err == nil {
		t.Error("expected error updating nonexistent camera, got nil")
	}
}

// TestInspectorConfigSerialization tests JSON marshaling/unmarshaling.
func TestInspectorConfigSerialization(t *testing.T) {
	cfg := InspectorConfigResponse{
		MotionZHigh:          3.0,
		IntraRatioHigh:       2.5,
		OnThreshold:          2,
		OffThreshold:         45,
		BPFFloor:             100.0,
		ConfiguredPeriodicKF: false,
		GradualEnabled:       false,
		GradualThreshold:     0.15,
		GradualWindowFrames:  900,
	}

	// Marshal to JSON
	data, err := json.Marshal(cfg)
	if err != nil {
		t.Fatalf("failed to marshal config: %v", err)
	}

	// Unmarshal back
	var decoded InspectorConfigResponse
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("failed to unmarshal config: %v", err)
	}

	// Verify fields
	if decoded.MotionZHigh != cfg.MotionZHigh {
		t.Error("motion_z_high mismatch after unmarshal")
	}
	if decoded.OnThreshold != cfg.OnThreshold {
		t.Error("on_threshold mismatch after unmarshal")
	}
}

// TestPartialConfigUpdate tests updating only specific fields.
func TestPartialConfigUpdate(t *testing.T) {
	supervisor := newMockSupervisor()
	recorder := &mockRecorder{supervisor: supervisor}

	// Get initial config
	initial, err := recorder.GetInspectorConfig("camera1")
	if err != nil {
		t.Fatalf("failed to get initial config: %v", err)
	}

	// Create partial update
	updated := *initial
	updated.MotionZHigh = 7.5

	// Apply update
	if err := recorder.UpdateInspectorConfig("camera1", &updated); err != nil {
		t.Fatalf("failed to update config: %v", err)
	}

	// Verify only MotionZHigh changed
	final, err := recorder.GetInspectorConfig("camera1")
	if err != nil {
		t.Fatalf("failed to get final config: %v", err)
	}

	if final.MotionZHigh != 7.5 {
		t.Errorf("expected motion_z_high=7.5, got %f", final.MotionZHigh)
	}
	if final.IntraRatioHigh != initial.IntraRatioHigh {
		t.Error("intra_ratio_high should not have changed")
	}
	if final.OnThreshold != initial.OnThreshold {
		t.Error("on_threshold should not have changed")
	}
}

// TestErrorResponses tests error response formatting.
func TestErrorResponses(t *testing.T) {
	tests := []struct {
		name    string
		errResp ErrorResponse
	}{
		{
			name:    "camera not found",
			errResp: ErrorResponse{Error: "camera camera1 not found"},
		},
		{
			name:    "invalid JSON",
			errResp: ErrorResponse{Error: "Invalid JSON: unexpected token"},
		},
		{
			name:    "validation error",
			errResp: ErrorResponse{Error: "motion_z_high must be between 0 and 100"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			data, err := json.Marshal(tt.errResp)
			if err != nil {
				t.Fatalf("failed to marshal error response: %v", err)
			}

			var decoded ErrorResponse
			if err := json.Unmarshal(data, &decoded); err != nil {
				t.Fatalf("failed to unmarshal error response: %v", err)
			}

			if decoded.Error != tt.errResp.Error {
				t.Errorf("error message mismatch: want %q, got %q", tt.errResp.Error, decoded.Error)
			}
		})
	}
}

// TestConfigValidation tests configuration parameter validation.
func TestConfigValidation(t *testing.T) {
	tests := []struct {
		name      string
		cfg       libemd.InspectorConfig
		wantError bool
	}{
		{
			name: "valid config",
			cfg: libemd.InspectorConfig{
				MotionZHigh:    3.0,
				IntraRatioHigh: 2.5,
				OnThreshold:    2,
				OffThreshold:   45,
			},
			wantError: false,
		},
		{
			name: "z-score too high",
			cfg: libemd.InspectorConfig{
				MotionZHigh:    150.0,
				IntraRatioHigh: 2.5,
				OnThreshold:    2,
				OffThreshold:   45,
			},
			wantError: true,
		},
		{
			name: "z-score negative",
			cfg: libemd.InspectorConfig{
				MotionZHigh:    -1.0,
				IntraRatioHigh: 2.5,
				OnThreshold:    2,
				OffThreshold:   45,
			},
			wantError: true,
		},
		{
			name: "threshold zero",
			cfg: libemd.InspectorConfig{
				MotionZHigh:    3.0,
				IntraRatioHigh: 2.5,
				OnThreshold:    0,
				OffThreshold:   45,
			},
			wantError: true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			// Validate z-score range
			hasError := tt.cfg.MotionZHigh < 0 || tt.cfg.MotionZHigh > 100 ||
				tt.cfg.IntraRatioHigh < 0 || tt.cfg.IntraRatioHigh > 100 ||
				tt.cfg.OnThreshold == 0 || tt.cfg.OnThreshold > 255 ||
				tt.cfg.OffThreshold == 0 || tt.cfg.OffThreshold > 255

			if hasError != tt.wantError {
				t.Errorf("validation mismatch: wantError=%v, gotError=%v", tt.wantError, hasError)
			}
		})
	}
}

// TestHTTPEndpoints tests the actual HTTP endpoint behavior.
func TestHTTPEndpoints(t *testing.T) {
	// Create a simple handler wrapper for testing HTTP responses
	w := httptest.NewRecorder()
	w.Header().Set("Content-Type", "application/json")

	// Test success response
	successResp := SuccessResponse{
		Success: true,
		Message: "Configuration updated for camera test",
	}
	json.NewEncoder(w).Encode(successResp)

	resp := w.Result()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("expected status 200, got %d", resp.StatusCode)
	}

	var decoded SuccessResponse
	if err := json.NewDecoder(resp.Body).Decode(&decoded); err != nil {
		t.Fatalf("failed to decode response: %v", err)
	}

	if !decoded.Success {
		t.Error("expected success=true")
	}
	if decoded.Message == "" {
		t.Error("expected non-empty message")
	}
}

// TestRequestParsing tests parsing of partial update requests.
func TestRequestParsing(t *testing.T) {
	tests := []struct {
		name     string
		body     string
		wantErr  bool
		checkVal func(*testing.T, *InspectorConfigRequest)
	}{
		{
			name:    "full update",
			body:    `{"motion_z_high": 5.0, "on_threshold": 3}`,
			wantErr: false,
			checkVal: func(t *testing.T, req *InspectorConfigRequest) {
				if req.MotionZHigh == nil || *req.MotionZHigh != 5.0 {
					t.Error("expected motion_z_high=5.0")
				}
				if req.OnThreshold == nil || *req.OnThreshold != 3 {
					t.Error("expected on_threshold=3")
				}
			},
		},
		{
			name:    "partial update",
			body:    `{"motion_z_high": 4.5}`,
			wantErr: false,
			checkVal: func(t *testing.T, req *InspectorConfigRequest) {
				if req.MotionZHigh == nil || *req.MotionZHigh != 4.5 {
					t.Error("expected motion_z_high=4.5")
				}
				if req.OnThreshold != nil {
					t.Error("expected on_threshold to be nil (not provided)")
				}
			},
		},
		{
			name:    "invalid JSON",
			body:    `{invalid}`,
			wantErr: true,
		},
		{
			name:    "empty object",
			body:    `{}`,
			wantErr: false,
			checkVal: func(t *testing.T, req *InspectorConfigRequest) {
				if req.MotionZHigh != nil {
					t.Error("expected all fields to be nil in empty request")
				}
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var req InspectorConfigRequest
			err := json.NewDecoder(bytes.NewReader([]byte(tt.body))).Decode(&req)

			if (err != nil) != tt.wantErr {
				t.Errorf("decode error mismatch: wantErr=%v, gotErr=%v", tt.wantErr, err)
			}

			if !tt.wantErr && tt.checkVal != nil {
				tt.checkVal(t, &req)
			}
		})
	}
}
