// Package obs provides structured logging and metrics for autotune binaries.
package obs

import (
	"log/slog"
	"os"
	"strings"
)

// InitLog initialises the global slog logger from the LOG_LEVEL env variable.
// Default level is INFO. JSON output when LOG_FORMAT=json (default in prod).
func InitLog() {
	level := slog.LevelInfo
	if v := os.Getenv("LOG_LEVEL"); v != "" {
		switch strings.ToLower(v) {
		case "debug":
			level = slog.LevelDebug
		case "warn", "warning":
			level = slog.LevelWarn
		case "error":
			level = slog.LevelError
		}
	}

	var handler slog.Handler
	if os.Getenv("LOG_FORMAT") == "text" {
		handler = slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level})
	} else {
		handler = slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: level})
	}
	slog.SetDefault(slog.New(handler))
}
