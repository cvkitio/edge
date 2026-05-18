package tuner

import "sort"

// GridConfig defines the search space for the coarse grid sweep.
type GridConfig struct {
	MotionZHighValues       []float64
	MinBytesThresholdValues []uint32
	BPFRelativeFloorValues  []float64
	ZHighWarmupValues       []float64
	ZHighWarmupFrameValues  []uint16
}

// DefaultGrid returns a reasonable default search space based on the spec.
func DefaultGrid() GridConfig {
	return GridConfig{
		MotionZHighValues:       []float64{2.5, 3.0, 3.5, 4.0, 4.5, 5.0},
		MinBytesThresholdValues: []uint32{0, 500, 1000, 2000, 4000, 8000},
		BPFRelativeFloorValues:  []float64{0, 1.5, 2.0, 3.0},
		ZHighWarmupValues:       []float64{0, 5.0, 8.0},
		ZHighWarmupFrameValues:  []uint16{0, 4},
	}
}

// GridSearch runs the coarse grid sweep and returns all evaluated candidates
// sorted by score descending.
func GridSearch(events []LabelledEvent, grid GridConfig) []Candidate {
	var candidates []Candidate

	for _, z := range grid.MotionZHighValues {
		for _, mb := range grid.MinBytesThresholdValues {
			for _, bf := range grid.BPFRelativeFloorValues {
				for _, wu := range grid.ZHighWarmupValues {
					for _, wf := range grid.ZHighWarmupFrameValues {
						// Skip invalid combos: warmup z only applies if warmup frames > 0
						if wu > 0 && wf == 0 {
							continue
						}
						c := Candidate{
							MotionZHigh:       z,
							MinBytesThreshold: mb,
							BPFRelativeFloor:  bf,
							ZHighWarmup:       wu,
							ZHighWarmupFrames: wf,
						}
						c.Evaluate(events)
						candidates = append(candidates, c)
					}
				}
			}
		}
	}

	sort.Slice(candidates, func(i, j int) bool {
		return candidates[i].Score > candidates[j].Score
	})
	return candidates
}

// TopN returns the top n candidates from a sorted slice.
func TopN(candidates []Candidate, n int) []Candidate {
	if n > len(candidates) {
		n = len(candidates)
	}
	return candidates[:n]
}
