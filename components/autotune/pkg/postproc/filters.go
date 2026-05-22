package postproc

import (
	"context"
	"fmt"
	"strings"

	natspkg "github.com/cvkitio/autotune/pkg/nats"
)

// IDRBurstFilter suppresses events where the trigger reason indicates an
// unexpected IDR burst (encoder artefact) with a z-score above a threshold.
// IDR bursts produce huge byte counts even on static scenes because the encoder
// re-sends all reference frames; a high z-score caused purely by this is an FP.
//
// Suppress condition: reason contains "unexpected_idr" AND z_score >= MinZToSuppress.
// If MinZToSuppress == 0, all IDR-burst events are suppressed regardless of z.
type IDRBurstFilter struct {
	MinZToSuppress float64 // minimum z-score to suppress (0 = suppress all IDR bursts)
}

func (f IDRBurstFilter) Name() string { return "idr_burst" }

func (f IDRBurstFilter) Process(_ context.Context, evt *natspkg.Event) (Verdict, string, error) {
	if !strings.Contains(evt.Reason, "unexpected_idr") {
		return VerdictPass, "", nil
	}
	if f.MinZToSuppress > 0 && evt.ZScore < f.MinZToSuppress {
		return VerdictPass, "", nil
	}
	return VerdictSuppress,
		fmt.Sprintf("idr_burst z=%.2f", evt.ZScore),
		nil
}

// BytesFloor suppresses events where the frame byte count is suspiciously low
// relative to the background model. Very small events are typically encoder
// artefacts or single-frame glitches, not real motion.
//
// Suppress condition: bytes < BPFRatio * bpf_slow AND bytes < MinAbsBytes.
// Either condition alone is enough to suppress (OR semantics).
type BytesFloor struct {
	BPFRatio    float64 // suppress if bytes < BPFRatio * bpf_slow (0 = disabled)
	MinAbsBytes uint64  // suppress if bytes < MinAbsBytes (0 = disabled)
}

func (f BytesFloor) Name() string { return "bytes_floor" }

func (f BytesFloor) Process(_ context.Context, evt *natspkg.Event) (Verdict, string, error) {
	if f.BPFRatio > 0 && evt.BPFSlow > 0 {
		floor := f.BPFRatio * evt.BPFSlow
		if float64(evt.Bytes) < floor {
			return VerdictSuppress,
				fmt.Sprintf("bytes %d < %.0f (%.1fx bpf_slow)", evt.Bytes, floor, f.BPFRatio),
				nil
		}
	}
	if f.MinAbsBytes > 0 && evt.Bytes < f.MinAbsBytes {
		return VerdictSuppress,
			fmt.Sprintf("bytes %d < min %d", evt.Bytes, f.MinAbsBytes),
			nil
	}
	return VerdictPass, "", nil
}

// IntraRatioFilter suppresses events where a high intra-frame ratio at low
// z-score indicates an encoder scene-change detection artefact rather than real
// motion. The intra_ratio signal measures how many frames in the AU are fully
// intra-coded; a burst of intra frames on a static scene (low z) is an FP.
//
// Suppress condition: intra_ratio > MaxRatio AND z_score < RequireZMin.
type IntraRatioFilter struct {
	MaxRatio    float64 // intra_ratio threshold (e.g. 2.5)
	RequireZMin float64 // only suppress if z_score is below this (e.g. 3.5)
}

func (f IntraRatioFilter) Name() string { return "intra_ratio" }

func (f IntraRatioFilter) Process(_ context.Context, evt *natspkg.Event) (Verdict, string, error) {
	if f.MaxRatio <= 0 || f.RequireZMin <= 0 {
		return VerdictPass, "", nil
	}
	if evt.IntraRatio > f.MaxRatio && evt.ZScore < f.RequireZMin {
		return VerdictSuppress,
			fmt.Sprintf("intra_ratio=%.2f > %.2f with z=%.2f < %.2f",
				evt.IntraRatio, f.MaxRatio, evt.ZScore, f.RequireZMin),
			nil
	}
	return VerdictPass, "", nil
}

// PassThrough always emits VerdictConfirm. It should be the last stage in the
// chain to ensure every event that reached it gets published.
type PassThrough struct{}

func (PassThrough) Name() string { return "passthrough" }

func (PassThrough) Process(_ context.Context, _ *natspkg.Event) (Verdict, string, error) {
	return VerdictConfirm, "passthrough", nil
}
