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
		natsURL  string
		site     string
		camera   string
		since    string
		out      string
	)
	cmd := &cobra.Command{
		Use:   "scan",
		Short: "Pull raw event history from NATS JetStream (or edge agent HTTP as fallback)",
		Long: `scan replays historical events for a camera from NATS JetStream.
It uses an ordered ephemeral consumer so no durable state is created.
Falls back to the edge agent HTTP event log when --nats is not provided.

Examples:
  # NATS (preferred — gets all fields including clip_url, z_timeline_url)
  cvkit-autotune scan --nats nats://emd-agent.au01-0.internal:4222 --site au01-0 --camera axis_81_1 --since 7d

  # HTTP fallback
  cvkit-autotune scan --agent http://emd-agent.au01-0.internal:8080 --camera axis_81_1 --since 7d`,
		RunE: func(cmd *cobra.Command, _ []string) error {
			ctx := cmd.Context()

			dur, err := parseDuration(since)
			if err != nil {
				return fmt.Errorf("--since: %w", err)
			}
			sinceTime := time.Now().Add(-dur)

			var events []edge.RawEvent

			if natsURL != "" {
				if camera == "" {
					return fmt.Errorf("scan: --camera is required with --nats")
				}
				if site == "" {
					return fmt.Errorf("scan: --site is required with --nats")
				}
				slog.Info("scanning events from NATS", "site", site, "camera", camera, "since", since)
				events, err = edge.ScanFromNATS(natsURL, site, camera, sinceTime, 0)
				if err != nil {
					return fmt.Errorf("scan: %w", err)
				}
			} else if agentURL != "" {
				if camera == "" {
					return fmt.Errorf("scan: --camera is required with --agent")
				}
				_ = ctx
				client := edge.New(agentURL)
				slog.Info("scanning events from HTTP", "camera", camera, "since", since)
				events, err = client.GetEvents(ctx, camera, edge.EventsOptions{
					From: sinceTime,
					To:   time.Now(),
				})
				if err != nil {
					return fmt.Errorf("scan: %w", err)
				}
			} else {
				return fmt.Errorf("scan: provide --nats (and --site) or --agent")
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
	cmd.Flags().StringVar(&natsURL, "nats", "", "NATS URL (e.g. nats://edge:4222) — reads from JetStream EVENTS stream")
	cmd.Flags().StringVar(&site, "site", "", "Site/instance ID (required with --nats, e.g. au01-0)")
	cmd.Flags().StringVar(&agentURL, "agent", "", "Edge agent base URL (e.g. http://edge:8080) — HTTP fallback")
	cmd.Flags().StringVar(&camera, "camera", "", "Camera name [required]")
	cmd.Flags().StringVar(&since, "since", "7d", "History window (e.g. 24h, 7d)")
	cmd.Flags().StringVar(&out, "out", "", "Output file path (default: stdout)")
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

			// Fetch operator labels from the agent clip library.  When the agent URL
			// is provided (even with --events), operator TP/FP labels applied via
			// the UI take precedence over the auto-labeling heuristic.
			var operatorLabels map[string]string
			if agentURL != "" && camera != "" {
				client := edge.New(agentURL)
				lbls, err := client.GetClipLabels(ctx, camera)
				if err != nil {
					slog.Warn("could not fetch operator clip labels, using heuristic", "err", err)
				} else {
					operatorLabels = lbls
					slog.Info("fetched operator labels", "count", len(operatorLabels))
				}
			}

			// Label events: operator labels win, heuristic fills the rest.
			labelled := autoLabel(rawEvents, operatorLabels)

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

// autoLabel labels events for grid search.  Operator labels (from the UI clip
// browser) take precedence; the heuristic covers unlabeled events as a fallback.
// operatorLabels maps event_id → "tp"|"fp"|"reference" and may be nil.
func autoLabel(events []edge.RawEvent, operatorLabels map[string]string) []tuner.LabelledEvent {
	labelled := make([]tuner.LabelledEvent, len(events))
	for i, evt := range events {
		label := tuner.LabelTP

		if lbl, ok := operatorLabels[evt.EventID]; ok {
			// Operator-assigned label wins.
			switch lbl {
			case "fp":
				label = tuner.LabelFP
			case "tp", "reference":
				label = tuner.LabelTP
			}
		} else if evt.BPFSlow > 0 && float64(evt.Bytes) < 1.5*evt.BPFSlow && evt.ZScore > 4.0 {
			// Heuristic: very low bytes + high z-score → encoder artefact on a static scene.
			label = tuner.LabelFP
		}

		labelled[i] = tuner.LabelledEvent{Event: evt, Label: label}
	}
	return labelled
}
