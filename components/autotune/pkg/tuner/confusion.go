// Package tuner implements the parameter search loop for per-camera inspector
// configuration. It reads labelled events from the edge event log, builds a
// confusion table, and searches for the configuration that maximises F0.5.
package tuner

import (
	"github.com/cvkitio/autotune/pkg/edge"
)

// Label classifies an event as a true positive or false positive.
type Label int8

const (
	LabelUnknown Label = 0
	LabelTP      Label = 1  // true positive — real motion of the target class
	LabelFP      Label = -1 // false positive — irrelevant trigger
)

// LabelledEvent pairs a raw edge event with its ground-truth or model label.
type LabelledEvent struct {
	Event edge.RawEvent
	Label Label
}

// ConfusionTable holds TP/FP/FN counts for a given configuration candidate.
type ConfusionTable struct {
	TP int
	FP int
	FN int // labelled TPs that the candidate config would suppress
}

// Precision returns TP / (TP + FP). Returns 0 if no positives predicted.
func (t ConfusionTable) Precision() float64 {
	denom := float64(t.TP + t.FP)
	if denom == 0 {
		return 0
	}
	return float64(t.TP) / denom
}

// Recall returns TP / (TP + FN). Returns 0 if no actual positives.
func (t ConfusionTable) Recall() float64 {
	denom := float64(t.TP + t.FN)
	if denom == 0 {
		return 0
	}
	return float64(t.TP) / denom
}

// FBeta computes the F-beta score. beta=0.5 weights precision twice as much as recall.
func (t ConfusionTable) FBeta(beta float64) float64 {
	p := t.Precision()
	r := t.Recall()
	b2 := beta * beta
	denom := b2*p + r
	if denom == 0 {
		return 0
	}
	return (1 + b2) * p * r / denom
}

// F05 returns the F0.5 score (precision-weighted).
func (t ConfusionTable) F05() float64 {
	return t.FBeta(0.5)
}

// Candidate is one parameter combination evaluated during grid or Bayesian search.
type Candidate struct {
	// Inspector parameters
	MotionZHigh       float64
	MinBytesThreshold uint32
	BPFRelativeFloor  float64
	ZHighWarmup       float64
	ZHighWarmupFrames uint16

	// Post-processor gate parameters (encoded-domain only for Phase A)
	// Future: SSIMSkipThreshold float64

	Table ConfusionTable
	Score float64 // F0.5
}

// Evaluate computes the confusion table for this candidate against the labelled events.
// It simulates what the inspector would have done with the candidate parameters
// using only the encoded-domain signals available in the event log.
func (c *Candidate) Evaluate(events []LabelledEvent) {
	c.Table = ConfusionTable{}
	for _, le := range events {
		wouldFire := c.wouldFire(le.Event)
		switch le.Label {
		case LabelTP:
			if wouldFire {
				c.Table.TP++
			} else {
				c.Table.FN++
			}
		case LabelFP:
			if wouldFire {
				c.Table.FP++
			}
		}
	}
	c.Score = c.Table.F05()
}

// wouldFire returns true if the candidate configuration would have fired on this event.
func (c *Candidate) wouldFire(evt edge.RawEvent) bool {
	// Absolute byte floor
	if c.MinBytesThreshold > 0 && evt.Bytes < uint64(c.MinBytesThreshold) {
		return false
	}
	// Relative byte floor
	if c.BPFRelativeFloor > 0 && evt.BPFSlow > 0 {
		floor := c.BPFRelativeFloor * evt.BPFSlow
		if float64(evt.Bytes) < floor {
			return false
		}
	}
	// Z-score threshold
	return evt.ZScore >= c.MotionZHigh
}
