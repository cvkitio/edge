// Package s3 provides an S3 upload stage for the post-processing pipeline.
package s3

import (
	"context"
	"fmt"

	"github.com/cvkitio/postprocess/pkg/pipeline"
)

// Config holds S3 connection parameters.
type Config struct {
	Endpoint  string `toml:"endpoint"`
	Bucket    string `toml:"bucket"`
	Region    string `toml:"region"`
	AccessKey string `toml:"access_key"`
	SecretKey string `toml:"secret_key"`
	Prefix    string `toml:"prefix"` // key prefix, e.g. "clips/"
}

// Uploader uploads clip files to S3 after a motion event.
// Implements pipeline.Stage.
type Uploader struct {
	cfg Config
}

// New creates an Uploader with the given config.
func New(cfg Config) *Uploader {
	return &Uploader{cfg: cfg}
}

func (u *Uploader) Name() string { return "s3-uploader" }

// Process uploads the clip at ev.ClipPath to S3.
// TODO: implement using aws-sdk-go-v2 or minio client.
func (u *Uploader) Process(ctx context.Context, ev *pipeline.Event) error {
	key := fmt.Sprintf("%s%s/%s/%d.ts", u.cfg.Prefix, ev.Site, ev.Camera, ev.Timestamp)
	_ = key
	// TODO: open ev.ClipPath, stream to s3, publish clips.uploaded NATS message
	return fmt.Errorf("s3 upload not yet implemented: target bucket=%s key=%s", u.cfg.Bucket, key)
}
