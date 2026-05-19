package libemd

/*
#include <emd/log.h>
#include <stdlib.h>
*/
import "C"
import "strings"

// LogLevel constants mirror emd_log_level_t.
const (
	LogLevelTrace = 0
	LogLevelDebug = 1
	LogLevelInfo  = 2
	LogLevelWarn  = 3
	LogLevelError = 4
	LogLevelFatal = 5
)

// SetLogLevel configures the C library log level. Thread-safe.
// Accepts 0–5 (trace … fatal) or a level string ("debug", "info", etc.).
func SetLogLevel(level int) {
	C.emd_log_set_level(C.emd_log_level_t(level))
}

// SetLogLevelFromString parses a level name and configures the C library.
// Returns false if the string was not recognised (level unchanged).
func SetLogLevelFromString(s string) bool {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "trace":
		SetLogLevel(LogLevelTrace)
	case "debug":
		SetLogLevel(LogLevelDebug)
	case "info", "":
		SetLogLevel(LogLevelInfo)
	case "warn", "warning":
		SetLogLevel(LogLevelWarn)
	case "error":
		SetLogLevel(LogLevelError)
	case "fatal":
		SetLogLevel(LogLevelFatal)
	default:
		return false
	}
	return true
}
