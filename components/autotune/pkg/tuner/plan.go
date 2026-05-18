package tuner

import (
	"encoding/json"
	"fmt"
	"io"
	"time"

	"github.com/cvkitio/autotune/pkg/edge"
)

// Plan is the output of a tuner run: the proposed config change and the evidence.
type Plan struct {
	TunerRunID   string    `json:"tuner_run_id"`
	Camera       string    `json:"camera"`
	GeneratedAt  time.Time `json:"generated_at"`
	Objective    string    `json:"objective"`
	EventWindow  string    `json:"event_window"`
	EventCount   int       `json:"event_count"`
	LabelledTPs  int       `json:"labelled_tps"`
	LabelledFPs  int       `json:"labelled_fps"`

	CurrentConfig  edge.InspectorConfig `json:"current_config"`
	ProposedConfig edge.InspectorConfig `json:"proposed_config"`

	ExpectedFPReductionPct float64 `json:"expected_fp_reduction_pct"`
	ExpectedTPLossPct      float64 `json:"expected_tp_loss_pct"`

	// Sample events that change classification with the new config.
	FlipFPtoTN []edge.RawEvent `json:"flip_fp_to_tn,omitempty"` // were FP, now suppressed
	FlipTPtoFN []edge.RawEvent `json:"flip_tp_to_fn,omitempty"` // were TP, now suppressed (bad)
}

// Write serialises the plan as indented JSON to w.
func (p *Plan) Write(w io.Writer) error {
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	return enc.Encode(p)
}

// Validate checks that the plan does not violate the max-recall-loss constraint.
func (p *Plan) Validate(maxRecallLossPct float64) error {
	if p.ExpectedTPLossPct > maxRecallLossPct {
		return fmt.Errorf("plan would lose %.1f%% recall (max allowed: %.1f%%)",
			p.ExpectedTPLossPct, maxRecallLossPct)
	}
	return nil
}

// BuildPlan creates a Plan from the best candidate found during search.
func BuildPlan(
	camera string,
	runID string,
	currentCfg edge.InspectorConfig,
	best Candidate,
	events []LabelledEvent,
	baselineTable ConfusionTable,
	window string,
) Plan {
	proposed := currentCfg
	proposed.MotionZHigh = best.MotionZHigh
	proposed.MinBytesThreshold = best.MinBytesThreshold
	proposed.BPFRelativeFloor = best.BPFRelativeFloor
	proposed.ZHighWarmup = best.ZHighWarmup
	proposed.ZHighWarmupFrames = best.ZHighWarmupFrames

	// Compute expected improvement vs. baseline (no-change)
	var fpReduction, tpLoss float64
	if baselineTable.FP > 0 {
		fpReduction = float64(baselineTable.FP-best.Table.FP) / float64(baselineTable.FP) * 100
	}
	if baselineTable.TP > 0 {
		tpLoss = float64(baselineTable.FN-0) / float64(baselineTable.TP) * 100
		// FN in best vs TP in baseline
		tpLoss = float64(best.Table.FN) / float64(baselineTable.TP+baselineTable.FN) * 100
	}

	var tps, fps int
	var flipFPtoTN, flipTPtoFN []edge.RawEvent
	for _, le := range events {
		switch le.Label {
		case LabelTP:
			tps++
			if !best.wouldFire(le.Event) && len(flipTPtoFN) < 10 {
				flipTPtoFN = append(flipTPtoFN, le.Event)
			}
		case LabelFP:
			fps++
			if !best.wouldFire(le.Event) && len(flipFPtoTN) < 10 {
				flipFPtoTN = append(flipFPtoTN, le.Event)
			}
		}
	}

	return Plan{
		TunerRunID:             runID,
		Camera:                 camera,
		GeneratedAt:            time.Now().UTC(),
		Objective:              "f0.5",
		EventWindow:            window,
		EventCount:             len(events),
		LabelledTPs:            tps,
		LabelledFPs:            fps,
		CurrentConfig:          currentCfg,
		ProposedConfig:         proposed,
		ExpectedFPReductionPct: fpReduction,
		ExpectedTPLossPct:      tpLoss,
		FlipFPtoTN:             flipFPtoTN,
		FlipTPtoFN:             flipTPtoFN,
	}
}
