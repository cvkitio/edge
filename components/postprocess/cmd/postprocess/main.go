// Package main is the entry point for the cvkit post-processor.
// It subscribes to NATS JetStream for motion events and dispatches them
// to configured processing pipelines (e.g. S3 upload, webhook notification).
package main

import (
	"context"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/spf13/cobra"
)

var rootCmd = &cobra.Command{
	Use:   "postprocess",
	Short: "cvkit post-processor — event pipeline for motion clips",
	Long: `Subscribes to NATS JetStream motion events and processes them
through a configurable pipeline: S3 upload, webhook notification, etc.`,
	RunE: runServer,
}

var cfgFile string

func init() {
	rootCmd.PersistentFlags().StringVar(&cfgFile, "config", "", "config file path (required)")
	_ = rootCmd.MarkPersistentFlagRequired("config")
}

func main() {
	if err := rootCmd.Execute(); err != nil {
		os.Exit(1)
	}
}

func runServer(cmd *cobra.Command, args []string) error {
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	slog.Info("postprocess starting", "config", cfgFile)

	// TODO: load config, connect to NATS, start pipeline
	<-ctx.Done()
	slog.Info("postprocess shutting down")
	return nil
}
