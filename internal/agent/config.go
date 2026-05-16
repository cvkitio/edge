package agent

import (
	"fmt"
	"os"

	"github.com/BurntSushi/toml"
	"github.com/cvkitio/cvkit/edge/emd-agent/internal/libemd"
)

// Config represents the agent configuration file.
type Config struct {
	Agent     AgentConfig              `toml:"agent"`
	Runtime   RuntimeConfig            `toml:"runtime"`
	Recording RecordingConfig          `toml:"recording"`
	Disk      DiskConfig               `toml:"disk"`
	Cameras   map[string]CameraConfig  `toml:"cameras"`
}

type AgentConfig struct {
	Mode       string `toml:"mode"`
	InstanceID string `toml:"instance_id"`
	DataDir    string `toml:"data_dir"`
	SDNotify   bool   `toml:"sd_notify"`
}

type RuntimeConfig struct {
	LogLevel      string `toml:"log_level"`
	ClipRoot      string `toml:"clip_root"`
	InflightRoot  string `toml:"inflight_root"`
}

type RecordingConfig struct {
	Container    string `toml:"container"`
	FsyncPolicy  string `toml:"fsync_policy"`
}

type DiskConfig struct {
	MaxBytesPerCamera uint64 `toml:"max_bytes_per_camera"`
	RetentionDays     uint32 `toml:"retention_days"`
}

type CameraConfig struct {
	URL              string  `toml:"url"`
	Transport        string  `toml:"transport"`
	CodecHint        string  `toml:"codec_hint"`
	BufferSeconds    uint32  `toml:"buffer_seconds"`
	PreRollSeconds   uint32  `toml:"pre_roll_seconds"`
	PostRollSeconds  uint32  `toml:"post_roll_seconds"`
	ClipMaxSeconds   uint32  `toml:"clip_max_seconds"`
	MotionZHigh      float64 `toml:"motion_z_high"`
	IntraRatioHigh   float64 `toml:"intra_ratio_high"`
	OnThreshold      uint8   `toml:"on_threshold"`
	OffThreshold     uint8   `toml:"off_threshold"`
	GradualEnabled   bool    `toml:"gradual_enabled"`
	MaxBitrateBPS    uint32  `toml:"max_bitrate_bps"`
}

// LoadConfig loads the agent configuration from a TOML file.
func LoadConfig(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config: %w", err)
	}

	var cfg Config
	if err := toml.Unmarshal(data, &cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}

	return &cfg, nil
}

// ToLibemdConfig converts agent CameraConfig to libemd.CameraConfig.
func (c *CameraConfig) ToLibemdConfig(name string, camID uint16) *libemd.CameraConfig {
	transport := uint8(0) // TCP
	if c.Transport == "udp" {
		transport = 1
	}

	codecHint := uint8(1) // H.264
	if c.CodecHint == "h265" || c.CodecHint == "hevc" {
		codecHint = 2
	} else if c.CodecHint == "auto" {
		codecHint = 0
	}

	return &libemd.CameraConfig{
		Name:            name,
		URL:             c.URL,
		CamID:           camID,
		Transport:       transport,
		CodecHint:       codecHint,
		BufferSeconds:   c.BufferSeconds,
		PreRollSeconds:  c.PreRollSeconds,
		PostRollSeconds: c.PostRollSeconds,
		ClipMaxSeconds:  c.ClipMaxSeconds,
		MaxBitrateBPS:   c.MaxBitrateBPS,
		MotionZHigh:     c.MotionZHigh,
		IntraRatioHigh:  c.IntraRatioHigh,
		OnThreshold:     c.OnThreshold,
		OffThreshold:    c.OffThreshold,
		GradualEnabled:  c.GradualEnabled,
	}
}
