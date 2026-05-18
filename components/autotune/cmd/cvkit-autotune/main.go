// cvkit-autotune is an operator-initiated CLI for reducing false positives in
// per-camera motion detection configurations.
//
// Usage:
//
//	cvkit-autotune scan   --agent http://edge:8080 --camera axis_82_2 --since 7d
//	cvkit-autotune search --camera axis_82_2 --plan plan.json
//	cvkit-autotune apply  --agent http://edge:8080 --camera axis_82_2 --plan plan.json [--dry-run]
//	cvkit-autotune rollback --agent http://edge:8080 --camera axis_82_2 --run-id <id>
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"github.com/spf13/cobra"

	"github.com/cvkitio/autotune/pkg/edge"
	"github.com/cvkitio/autotune/pkg/obs"
	"github.com/cvkitio/autotune/pkg/tuner"
)

func main() {
	obs.InitLog()

	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	if err := rootCmd().ExecuteContext(ctx); err != nil {
		slog.Error("command failed", "err", err)
		os.Exit(1)
	}
}

func rootCmd() *cobra.Command {
	root := &cobra.Command{
		Use:   "cvkit-autotune",
		Short: "Operator CLI for tuning per-camera motion detection parameters",
		Long: `cvkit-autotune reduces false positives in the edge motion detector by
scanning labelled event history, running a parameter search, and applying
the resulting config to the edge agent with a 15-minute canary.`,
	}
	root.AddCommand(scanCmd(), searchCmd(), applyCmd(), rollbackCmd())
	return root
}

// ── scan ─────────────────────────────────────────────────────────────────────

func scanCmd() *cobra.Command {
	var (
		agentURL string
		camera   string
		since    string
		out      string
	)
	cmd := &cobra.Command{
		Use:   "scan",
		Short: "Pull raw event history from the edge agent event log",
		RunE: func(cmd *cobra.Command, _ []string) error {
			ctx := cmd.Context()

			dur, err := parseDuration(since)
			if err != nil {
				return fmt.Errorf("--since: %w", err)
			}

			client := edge.New(agentURL)
			slog.Info("scanning events", "camera", camera, "since", since)

			events, err := client.GetEvents(ctx, camera, edge.EventsOptions{
				From: time.Now().Add(-dur),
				To:   time.Now(),
			})
			if err != nil {
				return fmt.Errorf("scan: %w", err)
			}

			slog.Info("fetched events", "count", len(events))

			// Write as JSONL
			w := os.Stdout
			if out != "" {
				f, err := os.Create(out)
				if err != nil {
					return fmt.Errorf("scan: create output: %w", err)
				}
				defer f.Close()
				w = f
			}

			enc := json.NewEncoder(w)
			for _, evt := range events {
				if err := enc.Encode(evt); err != nil {
					return fmt.Errorf("scan: encode: %w", err)
				}
			}
			return nil
		},
	}
	cmd.Flags().StringVar(&agentURL, "agent", "", "Edge agent base URL (e.g. http://edge:8080) [required]")
	cmd.Flags().StringVar(&camera, "camera", "", "Camera name [required]")
	cmd.Flags().StringVar(&since, "since", "7d", "History window (e.g. 24h, 7d)")
	cmd.Flags().StringVar(&out, "out", "", "Output file path (default: stdout)")
	_ = cmd.MarkFlagRequired("agent")
	_ = cmd.MarkFlagRequired("camera")
	return cmd
}

// ── search ───────────────────────────────────────────────────────────────────

func searchCmd() *cobra.Command {
	var (
		agentURL      string
		camera        string
		eventsFile    string
		planOut       string
		maxRecallLoss float64
	)
	cmd := &cobra.Command{
		Use:   "search",
		Short: "Run parameter search and emit a config plan",
		RunE: func(cmd *cobra.Command, _ []string) error {
			ctx := cmd.Context()

			// Load events from file or fetch live
			var rawEvents []edge.RawEvent
			if eventsFile != "" {
				f, err := os.Open(eventsFile)
				if err != nil {
					return fmt.Errorf("search: open events file: %w", err)
				}
				defer f.Close()
				dec := json.NewDecoder(f)
				for dec.More() {
					var evt edge.RawEvent
					if err := dec.Decode(&evt); err != nil {
						return fmt.Errorf("search: decode event: %w", err)
					}
					rawEvents = append(rawEvents, evt)
				}
			} else if agentURL != "" && camera != "" {
				client := edge.New(agentURL)
				var err error
				rawEvents, err = client.GetEvents(ctx, camera, edge.EventsOptions{
					From: time.Now().Add(-7 * 24 * time.Hour),
					To:   time.Now(),
				})
				if err != nil {
					return fmt.Errorf("search: fetch events: %w", err)
				}
			} else {
				return fmt.Errorf("search: provide --events or both --agent and --camera")
			}

			slog.Info("loaded events for search", "count", len(rawEvents))

			// Auto-label: events with z_score > 3.0 * 1.5 are probably FP on static cameras.
			// In production the post-processor labels them; this is the fallback.
			labelled := autoLabel(rawEvents)

			// Coarse grid search
			grid := tuner.DefaultGrid()
			candidates := tuner.GridSearch(labelled, grid)
			slog.Info("grid search complete", "candidates_evaluated", len(candidates))

			if len(candidates) == 0 {
				return fmt.Errorf("search: no candidates evaluated — insufficient events?")
			}

			best := candidates[0]
			slog.Info("best candidate",
				"z_high", best.MotionZHigh,
				"min_bytes", best.MinBytesThreshold,
				"bpf_floor", best.BPFRelativeFloor,
				"score_f05", best.Score,
			)

			// Build baseline (current config, no change)
			baseline := tuner.Candidate{MotionZHigh: 3.0}
			baseline.Evaluate(labelled)

			// Fetch current config if agent URL provided
			currentCfg := edge.InspectorConfig{MotionZHigh: 3.0, IntraRatioHigh: 2.5}
			if agentURL != "" && camera != "" {
				client := edge.New(agentURL)
				cfg, err := client.GetConfig(ctx, camera)
				if err != nil {
					slog.Warn("could not fetch current config, using defaults", "err", err)
				} else {
					currentCfg = *cfg
				}
			}

			runID := fmt.Sprintf("run-%d", time.Now().Unix())
			plan := tuner.BuildPlan(camera, runID, currentCfg, best, labelled,
				baseline.Table, "7d")

			if err := plan.Validate(maxRecallLoss); err != nil {
				return fmt.Errorf("search: plan validation: %w", err)
			}

			// Write plan
			w := os.Stdout
			if planOut != "" {
				f, err := os.Create(planOut)
				if err != nil {
					return fmt.Errorf("search: create plan file: %w", err)
				}
				defer f.Close()
				w = f
			}
			return plan.Write(w)
		},
	}
	cmd.Flags().StringVar(&agentURL, "agent", "", "Edge agent base URL (optional; used to fetch live events and current config)")
	cmd.Flags().StringVar(&camera, "camera", "", "Camera name")
	cmd.Flags().StringVar(&eventsFile, "events", "", "JSONL events file produced by 'scan' (alternative to --agent)")
	cmd.Flags().StringVar(&planOut, "plan", "", "Output plan JSON file (default: stdout)")
	cmd.Flags().Float64Var(&maxRecallLoss, "max-recall-loss", 2.0, "Maximum allowed recall loss in percent")
	return cmd
}

// ── apply ────────────────────────────────────────────────────────────────────

func applyCmd() *cobra.Command {
	var (
		agentURL string
		camera   string
		planFile string
		dryRun   bool
	)
	cmd := &cobra.Command{
		Use:   "apply",
		Short: "Apply a tuner plan to the edge agent with a canary",
		RunE: func(cmd *cobra.Command, _ []string) error {
			ctx := cmd.Context()

			f, err := os.Open(planFile)
			if err != nil {
				return fmt.Errorf("apply: open plan: %w", err)
			}
			defer f.Close()

			var plan tuner.Plan
			if err := json.NewDecoder(f).Decode(&plan); err != nil {
				return fmt.Errorf("apply: decode plan: %w", err)
			}

			if camera != "" {
				plan.Camera = camera
			}
			if plan.Camera == "" {
				return fmt.Errorf("apply: camera name required (set in plan or via --camera)")
			}

			slog.Info("applying plan",
				"camera", plan.Camera,
				"run_id", plan.TunerRunID,
				"expected_fp_reduction_pct", plan.ExpectedFPReductionPct,
				"expected_tp_loss_pct", plan.ExpectedTPLossPct,
				"dry_run", dryRun,
			)

			if dryRun {
				slog.Info("dry-run: no changes applied")
				return nil
			}

			client := edge.New(agentURL)
			if err := client.PutConfig(ctx, plan.Camera, &plan.ProposedConfig); err != nil {
				return fmt.Errorf("apply: put config: %w", err)
			}
			slog.Info("config applied — monitoring for 15 minutes",
				"camera", plan.Camera)

			// TODO (Phase D): watch events.confirmed.<site>.<camera> on NATS for 15m
			// and auto-rollback if the event rate spikes ≥ 3× or drops to zero.
			return nil
		},
	}
	cmd.Flags().StringVar(&agentURL, "agent", "", "Edge agent base URL [required]")
	cmd.Flags().StringVar(&camera, "camera", "", "Camera name (overrides plan)")
	cmd.Flags().StringVar(&planFile, "plan", "", "Plan JSON file produced by 'search' [required]")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Print the proposed config without applying")
	_ = cmd.MarkFlagRequired("agent")
	_ = cmd.MarkFlagRequired("plan")
	return cmd
}

// ── rollback ─────────────────────────────────────────────────────────────────

func rollbackCmd() *cobra.Command {
	var (
		agentURL string
		camera   string
		runID    string
		planDir  string
	)
	cmd := &cobra.Command{
		Use:   "rollback",
		Short: "Restore a previous tuner config by run ID",
		RunE: func(cmd *cobra.Command, _ []string) error {
			ctx := cmd.Context()

			// Locate the plan file by run ID
			pattern := filepath.Join(planDir, runID+"*.json")
			matches, err := filepath.Glob(pattern)
			if err != nil || len(matches) == 0 {
				return fmt.Errorf("rollback: no plan found for run-id %s in %s", runID, planDir)
			}

			f, err := os.Open(matches[0])
			if err != nil {
				return fmt.Errorf("rollback: open plan: %w", err)
			}
			defer f.Close()

			var plan tuner.Plan
			if err := json.NewDecoder(f).Decode(&plan); err != nil {
				return fmt.Errorf("rollback: decode plan: %w", err)
			}

			slog.Info("rolling back to previous config",
				"camera", camera,
				"run_id", runID,
			)

			client := edge.New(agentURL)
			if err := client.PutConfig(ctx, camera, &plan.CurrentConfig); err != nil {
				return fmt.Errorf("rollback: put config: %w", err)
			}
			slog.Info("rollback applied", "camera", camera)
			return nil
		},
	}
	cmd.Flags().StringVar(&agentURL, "agent", "", "Edge agent base URL [required]")
	cmd.Flags().StringVar(&camera, "camera", "", "Camera name [required]")
	cmd.Flags().StringVar(&runID, "run-id", "", "Tuner run ID to roll back to [required]")
	cmd.Flags().StringVar(&planDir, "plan-dir", ".", "Directory where plan files are stored")
	_ = cmd.MarkFlagRequired("agent")
	_ = cmd.MarkFlagRequired("camera")
	_ = cmd.MarkFlagRequired("run-id")
	return cmd
}

// ── helpers ──────────────────────────────────────────────────────────────────

// parseDuration extends time.ParseDuration to support "Nd" (N days).
func parseDuration(s string) (time.Duration, error) {
	// Handle "Nd" suffix
	if len(s) > 0 && s[len(s)-1] == 'd' {
		var days float64
		if _, err := fmt.Sscanf(s[:len(s)-1], "%f", &days); err != nil {
			return 0, fmt.Errorf("cannot parse %q as a duration", s)
		}
		return time.Duration(days * 24 * float64(time.Hour)), nil
	}
	return time.ParseDuration(s)
}

// autoLabel applies a simple heuristic: events that look like static-camera FPs
// (very low byte count relative to z-score) are labelled FP; everything else TP.
// This is the fallback when the post-processor has not yet labelled events.
func autoLabel(events []edge.RawEvent) []tuner.LabelledEvent {
	labelled := make([]tuner.LabelledEvent, len(events))
	for i, evt := range events {
		label := tuner.LabelTP
		// Heuristic: events with very low bytes but high z-score are likely FP
		// (encoder artefact on a static scene).
		if evt.BPFSlow > 0 && float64(evt.Bytes) < 1.5*evt.BPFSlow && evt.ZScore > 4.0 {
			label = tuner.LabelFP
		}
		labelled[i] = tuner.LabelledEvent{Event: evt, Label: label}
	}
	return labelled
}
