// Package pipeline orchestrates post-processing stages for motion events.
package pipeline

import "context"

// Event represents a motion event received from NATS.
type Event struct {
	Site      string `json:"site"`
	Camera    string `json:"camera"`
	ClipPath  string `json:"clip_path"`
	Timestamp int64  `json:"timestamp_ms"`
	Subject   string `json:"-"` // NATS subject
}

// Stage is a processing step in the pipeline.
type Stage interface {
	Name() string
	Process(ctx context.Context, ev *Event) error
}

// Pipeline runs an ordered sequence of stages for each event.
type Pipeline struct {
	stages []Stage
}

// New creates a Pipeline with the given stages.
func New(stages ...Stage) *Pipeline {
	return &Pipeline{stages: stages}
}

// Run executes all stages for the event, stopping on first error.
func (p *Pipeline) Run(ctx context.Context, ev *Event) error {
	for _, s := range p.stages {
		if err := s.Process(ctx, ev); err != nil {
			return err
		}
	}
	return nil
}
