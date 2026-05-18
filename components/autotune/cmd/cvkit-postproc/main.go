// cvkit-postproc is the long-running post-processor that subscribes to raw
// motion events from NATS, runs the two-stage confirmation gate, uploads
// confirmed clips to S3, and republishes results.
//
// Phase B skeleton: connects to NATS and passes events through unchanged
// (passthrough mode) until the gate logic is implemented in Phase C.
package main

import (
	"context"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/cvkitio/autotune/pkg/obs"
)

func main() {
	obs.InitLog()

	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	if err := run(ctx); err != nil {
		slog.Error("postproc fatal error", "err", err)
		os.Exit(1)
	}
}

func run(ctx context.Context) error {
	slog.Info("cvkit-postproc starting (Phase B passthrough)")

	// TODO (Phase B): load postproc.toml, connect to NATS, subscribe to
	// events.raw.>, pass each event through to events.confirmed. unchanged.
	// TODO (Phase C): add OpenCV BG-subtraction fast path, SSIM gate, libav decode.
	// TODO (Phase D): add YOLOv8/ONNX vision model, S3 upload.

	slog.Info("postproc ready — waiting for shutdown signal")
	<-ctx.Done()
	slog.Info("postproc shutting down")
	return nil
}
