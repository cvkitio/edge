// cvkit-postproc subscribes to raw motion events on NATS JetStream, runs each
// event through a configurable chain of filter stages, and republishes the result
// to events.confirmed.<site>.<camera> or events.suppressed.<site>.<camera>.
//
// Built-in stages (evaluated in order):
//
//  1. idr_burst  — suppress unexpected IDR encoder bursts above a z threshold
//  2. bytes_floor — suppress events with suspiciously low byte count
//  3. intra_ratio — suppress high intra_ratio events at low z-score
//  4. passthrough — confirm everything that survived the earlier stages
//
// Configuration is via command-line flags. For production use, set flags in the
// systemd unit or docker-compose environment.
//
// Usage:
//
//	cvkit-postproc run --nats nats://edge:4222 --site au01-0 --id postproc-au01-0
package main

import (
	"context"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/spf13/cobra"

	"github.com/cvkitio/autotune/pkg/obs"
	"github.com/cvkitio/autotune/pkg/postproc"
)

func main() {
	obs.InitLog()

	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	if err := rootCmd().ExecuteContext(ctx); err != nil {
		slog.Error("postproc fatal error", "err", err)
		os.Exit(1)
	}
}

func rootCmd() *cobra.Command {
	root := &cobra.Command{
		Use:   "cvkit-postproc",
		Short: "Post-processor for raw motion events — filters FPs and republishes",
	}
	root.AddCommand(runCmd())
	return root
}

func runCmd() *cobra.Command {
	var (
		natsURL string
		site    string
		id      string

		// Stage tuning
		idrMinZ       float64
		bytesBPFRatio float64
		minAbsBytes   uint64
		intraMaxRatio float64
		intraMinZ     float64
	)

	cmd := &cobra.Command{
		Use:   "run",
		Short: "Start the post-processor event pipeline",
		RunE: func(cmd *cobra.Command, _ []string) error {
			ctx := cmd.Context()

			stages := []postproc.Stage{
				postproc.IDRBurstFilter{MinZToSuppress: idrMinZ},
				postproc.BytesFloor{
					BPFRatio:    bytesBPFRatio,
					MinAbsBytes: minAbsBytes,
				},
				postproc.IntraRatioFilter{
					MaxRatio:    intraMaxRatio,
					RequireZMin: intraMinZ,
				},
				postproc.PassThrough{},
			}

			slog.Info("postproc stages configured",
				"idr_min_z", idrMinZ,
				"bytes_bpf_ratio", bytesBPFRatio,
				"min_abs_bytes", minAbsBytes,
				"intra_max_ratio", intraMaxRatio,
				"intra_min_z", intraMinZ,
			)

			p, err := postproc.New(postproc.Config{
				NATSURL: natsURL,
				Site:    site,
				ID:      id,
				Stages:  stages,
			})
			if err != nil {
				return fmt.Errorf("postproc init: %w", err)
			}
			defer p.Close()

			slog.Info("cvkit-postproc running", "site", site, "consumer", id)
			return p.Run(ctx)
		},
	}

	cmd.Flags().StringVar(&natsURL, "nats", "nats://localhost:4222", "NATS server URL")
	cmd.Flags().StringVar(&site, "site", "*", "Site ID to process (use '*' for all sites)")
	cmd.Flags().StringVar(&id, "id", "postproc", "Durable consumer name — must be unique per postproc instance")

	cmd.Flags().Float64Var(&idrMinZ, "idr-min-z", 0, "IDR burst: suppress if z_score >= this (0 = suppress all IDR bursts)")
	cmd.Flags().Float64Var(&bytesBPFRatio, "bytes-bpf-ratio", 0, "BytesFloor: suppress if bytes < ratio*bpf_slow (0 = disabled)")
	cmd.Flags().Uint64Var(&minAbsBytes, "min-abs-bytes", 0, "BytesFloor: suppress if bytes < this absolute floor (0 = disabled)")
	cmd.Flags().Float64Var(&intraMaxRatio, "intra-max-ratio", 0, "IntraRatio: suppress if intra_ratio > this (0 = disabled)")
	cmd.Flags().Float64Var(&intraMinZ, "intra-min-z", 0, "IntraRatio: only suppress when z_score < this (0 = disabled)")

	_ = cmd.MarkFlagRequired("site")

	return cmd
}
