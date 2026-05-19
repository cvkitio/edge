package edge

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
)

// InspectorConfig mirrors the inspector configuration returned by the edge API.
type InspectorConfig struct {
	MotionZHigh         float64 `json:"motion_z_high"`
	IntraRatioHigh      float64 `json:"intra_ratio_high"`
	OnThreshold         uint8   `json:"on_threshold"`
	OffThreshold        uint8   `json:"off_threshold"`
	BPFFloor            float64 `json:"bpf_floor"`
	GradualEnabled      bool    `json:"gradual_enabled"`
	GradualThreshold    float64 `json:"gradual_threshold"`
	GradualWindowFrames uint32  `json:"gradual_window_frames"`
	MinBytesThreshold   uint32  `json:"min_bytes_threshold"`
	BPFRelativeFloor    float64 `json:"bpf_relative_floor"`
	ZHighWarmup         float64 `json:"z_high_warmup"`
	ZHighWarmupFrames   uint16  `json:"z_high_warmup_frames"`
	TargetClassMask     uint8   `json:"target_class_mask"`
}

// GetConfig fetches the current inspector configuration for a camera.
func (c *Client) GetConfig(ctx context.Context, camera string) (*InspectorConfig, error) {
	var cfg InspectorConfig
	if err := c.get(ctx, "/api/cameras/"+camera+"/config", &cfg); err != nil {
		return nil, err
	}
	return &cfg, nil
}

// PutConfig replaces the inspector configuration for a camera.
// All fields in cfg are sent; this is a full replace, not a partial update.
func (c *Client) PutConfig(ctx context.Context, camera string, cfg *InspectorConfig) error {
	b, err := json.Marshal(cfg)
	if err != nil {
		return fmt.Errorf("edge put config: marshal: %w", err)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPut,
		c.baseURL+"/api/cameras/"+camera+"/config", bytes.NewReader(b))
	if err != nil {
		return fmt.Errorf("edge put config: build request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("edge put config %s: %w", camera, err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		var errResp struct {
			Error string `json:"error"`
		}
		_ = json.NewDecoder(resp.Body).Decode(&errResp)
		return fmt.Errorf("edge put config %s: status %d: %s", camera, resp.StatusCode, errResp.Error)
	}
	return nil
}
