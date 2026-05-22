// Package postproc provides a pluggable event-processing pipeline for cvkit-postproc.
// Each Stage inspects an incoming motion event and returns a Verdict:
//   - VerdictPass     — continue to the next stage
//   - VerdictConfirm  — stop the chain; publish to events.confirmed.*
//   - VerdictSuppress — stop the chain; publish to events.suppressed.*
//
// Stages are chained in order; the first non-Pass verdict wins.
// A PassThrough stage placed last always emits VerdictConfirm, ensuring every
// event is eventually published even when earlier stages can't decide.
package postproc

import (
	"context"

	natspkg "github.com/cvkitio/autotune/pkg/nats"
)

// Verdict is the outcome of a Stage's evaluation.
type Verdict int

const (
	VerdictPass     Verdict = iota // continue to the next stage
	VerdictConfirm                 // publish to events.confirmed.*
	VerdictSuppress                // publish to events.suppressed.*
)

// Stage evaluates a single motion event and returns a verdict.
type Stage interface {
	// Name returns a short human-readable identifier used in log output.
	Name() string

	// Process evaluates the event. It returns:
	//   VerdictPass      — I have no opinion; continue to the next stage
	//   VerdictConfirm   — this is real motion; stop here
	//   VerdictSuppress  — this is a false positive; stop here and give the reason
	Process(ctx context.Context, evt *natspkg.Event) (verdict Verdict, reason string, err error)
}
