// Package eventlog writes per-camera motion events to append-only JSONL files,
// rotated daily with a configurable retention policy.
package eventlog

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// Config holds eventlog configuration.
type Config struct {
	MaxBytesPerCamera int64 // 0 = default 100 MB
	RetentionDays     int   // 0 = default 30
}

// Event is one record written to the JSONL log.
type Event struct {
	EventID         string    `json:"event_id"`
	Site            string    `json:"site,omitempty"`
	Camera          string    `json:"camera"`
	CamID           uint16    `json:"cam_id"`
	TS              time.Time `json:"ts"`
	TsMonoNs        uint64    `json:"ts_mono_ns"`
	Type            string    `json:"type"`
	ZScore          float64   `json:"z_score"`
	IntraRatio      float64   `json:"intra_ratio"`
	Bytes           uint64    `json:"bytes"`
	BPFSlow         float64   `json:"bpf_slow"`
	BPFEwma         float64   `json:"bpf_ewma"`
	Reason          string    `json:"reason"`
	PTSStart        uint64    `json:"pts_start"`
	PTSEnd          uint64    `json:"pts_end"`
	Codec           string    `json:"codec"`
	FPS             float64   `json:"fps"`
	ClipID          string    `json:"clip_id,omitempty"`
	ClipPath        string    `json:"clip_path,omitempty"`
	TargetClassMask uint8     `json:"target_class_mask"`
	AgentVersion    string    `json:"agent_version"`
}

const (
	defaultMaxBytesPerCamera = 100 * 1024 * 1024 // 100 MB
	defaultRetentionDays     = 30
)

// Writer appends events to per-camera JSONL files.
type Writer struct {
	root  string
	cfg   Config
	mu    sync.Mutex
	files map[string]*cameraWriter
}

type cameraWriter struct {
	mu   sync.Mutex
	f    *os.File
	bw   *bufio.Writer
	date string // YYYY-MM-DD
	size int64
}

// New creates a new Writer. root is the base directory for event log files.
func New(root string, cfg Config) (*Writer, error) {
	if cfg.MaxBytesPerCamera <= 0 {
		cfg.MaxBytesPerCamera = defaultMaxBytesPerCamera
	}
	if cfg.RetentionDays <= 0 {
		cfg.RetentionDays = defaultRetentionDays
	}
	if err := os.MkdirAll(root, 0755); err != nil {
		return nil, fmt.Errorf("eventlog: create root: %w", err)
	}
	return &Writer{
		root:  root,
		cfg:   cfg,
		files: make(map[string]*cameraWriter),
	}, nil
}

// Append writes an event for the given camera. It is safe for concurrent use.
func (w *Writer) Append(evt Event) error {
	w.mu.Lock()
	cw, ok := w.files[evt.Camera]
	if !ok {
		cw = &cameraWriter{}
		w.files[evt.Camera] = cw
	}
	w.mu.Unlock()

	return cw.write(w.root, evt, w.cfg.MaxBytesPerCamera)
}

// Close flushes and closes all open file handles.
func (w *Writer) Close() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	var lastErr error
	for _, cw := range w.files {
		if err := cw.close(); err != nil {
			lastErr = err
		}
	}
	return lastErr
}

func (cw *cameraWriter) write(root string, evt Event, maxBytes int64) error {
	cw.mu.Lock()
	defer cw.mu.Unlock()

	today := evt.TS.UTC().Format("2006-01-02")
	if cw.f == nil || cw.date != today {
		if err := cw.rotate(root, evt.Camera, today); err != nil {
			return err
		}
	}

	b, err := json.Marshal(evt)
	if err != nil {
		return fmt.Errorf("eventlog: marshal: %w", err)
	}

	n, err := cw.bw.Write(append(b, '\n'))
	if err != nil {
		return fmt.Errorf("eventlog: write: %w", err)
	}
	cw.size += int64(n)
	_ = cw.bw.Flush()

	return nil
}

func (cw *cameraWriter) rotate(root, camera, date string) error {
	if cw.f != nil {
		_ = cw.bw.Flush()
		_ = cw.f.Close()
		cw.f = nil
	}

	dir := filepath.Join(root, camera)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return fmt.Errorf("eventlog: mkdir %s: %w", dir, err)
	}

	path := filepath.Join(dir, date+".jsonl")
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0640)
	if err != nil {
		return fmt.Errorf("eventlog: open %s: %w", path, err)
	}

	info, err := f.Stat()
	if err != nil {
		_ = f.Close()
		return fmt.Errorf("eventlog: stat %s: %w", path, err)
	}

	cw.f = f
	cw.bw = bufio.NewWriterSize(f, 64*1024)
	cw.date = date
	cw.size = info.Size()
	return nil
}

func (cw *cameraWriter) close() error {
	cw.mu.Lock()
	defer cw.mu.Unlock()
	if cw.f == nil {
		return nil
	}
	_ = cw.bw.Flush()
	err := cw.f.Close()
	cw.f = nil
	return err
}

// Reader reads events from per-camera JSONL files.
type Reader struct {
	root   string
	camera string
}

// Open creates a Reader for the given camera under root.
func Open(root, camera string) (*Reader, error) {
	return &Reader{root: root, camera: camera}, nil
}

// Range returns events with ts in [from, to], in chronological order.
// limit caps the number of returned events (0 = use default 10000).
func (r *Reader) Range(from, to time.Time, limit int) ([]Event, error) {
	if limit <= 0 {
		limit = 10000
	}

	dir := filepath.Join(r.root, r.camera)
	entries, err := os.ReadDir(dir)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("eventlog: readdir %s: %w", dir, err)
	}

	var events []Event
	for _, entry := range entries {
		if entry.IsDir() || filepath.Ext(entry.Name()) != ".jsonl" {
			continue
		}
		// Parse date from filename YYYY-MM-DD.jsonl
		name := entry.Name()
		dateStr := name[:len(name)-len(".jsonl")]
		fileDate, err := time.ParseInLocation("2006-01-02", dateStr, time.UTC)
		if err != nil {
			continue
		}
		// Skip files entirely before or after the window
		fileTo := fileDate.Add(24 * time.Hour)
		if fileTo.Before(from) || fileDate.After(to) {
			continue
		}

		path := filepath.Join(dir, name)
		f, err := os.Open(path)
		if err != nil {
			continue
		}
		scanner := bufio.NewScanner(f)
		scanner.Buffer(make([]byte, 1024*1024), 1024*1024)
		for scanner.Scan() {
			var evt Event
			if err := json.Unmarshal(scanner.Bytes(), &evt); err != nil {
				continue
			}
			if evt.TS.Before(from) || evt.TS.After(to) {
				continue
			}
			events = append(events, evt)
			if len(events) >= limit {
				_ = f.Close()
				return events, nil
			}
		}
		_ = f.Close()
	}
	return events, nil
}

// Close is a no-op for Reader (files are opened and closed per Range call).
func (r *Reader) Close() error { return nil }
