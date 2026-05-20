// Package api provides HTTP endpoints for runtime configuration and monitoring.
package api

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"log"
	"math"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/agent"
	"github.com/cvkitio/cvkit/edge/emd-agent/internal/eventlog"
)

// Handler provides HTTP endpoints for the agent API.
type Handler struct {
	supervisor   *agent.Supervisor
	clipRoot     string
	eventLogRoot string

	cpuMu     sync.Mutex
	cpuCached float64
	cpuAt     time.Time
}

// NewHandler creates a new API handler.
func NewHandler(supervisor *agent.Supervisor, clipRoot, eventLogRoot string) *Handler {
	return &Handler{
		supervisor:   supervisor,
		clipRoot:     clipRoot,
		eventLogRoot: eventLogRoot,
	}
}

// InspectorConfigResponse represents the inspector configuration in JSON.
type InspectorConfigResponse struct {
	MotionZHigh          float64 `json:"motion_z_high"`
	IntraRatioHigh       float64 `json:"intra_ratio_high"`
	OnThreshold          uint8   `json:"on_threshold"`
	OffThreshold         uint8   `json:"off_threshold"`
	BPFFloor             float64 `json:"bpf_floor"`
	ConfiguredPeriodicKF bool    `json:"configured_periodic_kf"`
	GradualEnabled       bool    `json:"gradual_enabled"`
	GradualThreshold     float64 `json:"gradual_threshold"`
	GradualWindowFrames  uint32  `json:"gradual_window_frames"`
}

// InspectorConfigRequest represents a configuration update request.
type InspectorConfigRequest struct {
	MotionZHigh          *float64 `json:"motion_z_high,omitempty"`
	IntraRatioHigh       *float64 `json:"intra_ratio_high,omitempty"`
	OnThreshold          *uint8   `json:"on_threshold,omitempty"`
	OffThreshold         *uint8   `json:"off_threshold,omitempty"`
	BPFFloor             *float64 `json:"bpf_floor,omitempty"`
	ConfiguredPeriodicKF *bool    `json:"configured_periodic_kf,omitempty"`
	GradualEnabled       *bool    `json:"gradual_enabled,omitempty"`
	GradualThreshold     *float64 `json:"gradual_threshold,omitempty"`
	GradualWindowFrames  *uint32  `json:"gradual_window_frames,omitempty"`
}

// ErrorResponse represents an error response.
type ErrorResponse struct {
	Error string `json:"error"`
}

// SuccessResponse represents a success response.
type SuccessResponse struct {
	Success bool   `json:"success"`
	Message string `json:"message"`
}

// CameraListResponse represents the list of cameras.
type CameraListResponse struct {
	Cameras []string `json:"cameras"`
}

// SystemStatsResponse is returned by GET /api/system.
type SystemStatsResponse struct {
	Disk   DiskStats   `json:"disk"`
	Memory MemoryStats `json:"memory"`
	CPU    CPUStats    `json:"cpu"`
}

// DiskStats describes the filesystem where clips are stored.
type DiskStats struct {
	Path        string  `json:"path"`
	TotalBytes  uint64  `json:"total_bytes"`
	UsedBytes   uint64  `json:"used_bytes"`
	FreeBytes   uint64  `json:"free_bytes"`
	UsedPercent float64 `json:"used_percent"`
}

// MemoryStats describes system RAM and Go heap usage.
type MemoryStats struct {
	TotalBytes  uint64  `json:"total_bytes"`
	UsedBytes   uint64  `json:"used_bytes"`
	FreeBytes   uint64  `json:"free_bytes"`
	UsedPercent float64 `json:"used_percent"`
	GoHeapBytes uint64  `json:"go_heap_bytes"`
}

// CPUStats describes CPU utilisation.
type CPUStats struct {
	UsedPercent float64 `json:"used_percent"`
	NumCPU      int     `json:"num_cpu"`
}

// CameraStatsEntry holds aggregated signal metrics for one camera over a time window.
type CameraStatsEntry struct {
	Name           string     `json:"name"`
	EventCount24h  int        `json:"event_count_24h"`
	LastEventTS    *time.Time `json:"last_event_ts,omitempty"`
	ZScoreLast     float64    `json:"z_score_last"`
	ZScoreAvg      float64    `json:"z_score_avg"`
	ZScoreMax      float64    `json:"z_score_max"`
	BPFSlowLast    float64    `json:"bpf_slow_last"`
	BPFEwmaLast    float64    `json:"bpf_ewma_last"`
	IntraRatioLast float64    `json:"intra_ratio_last"`
	IntraRatioAvg  float64    `json:"intra_ratio_avg"`
}

// AllCameraStatsResponse is returned by GET /api/cameras/stats.
type AllCameraStatsResponse struct {
	Cameras []CameraStatsEntry `json:"cameras"`
	From    time.Time          `json:"from"`
	To      time.Time          `json:"to"`
}

// RegisterRoutes registers all API routes on the given mux.
func (h *Handler) RegisterRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/cameras", h.handleCameras)
	mux.HandleFunc("/api/cameras/", h.handleCameraConfig)
	mux.HandleFunc("/api/clips", h.handleClipsList)
	mux.HandleFunc("/api/clips/", h.handleClipFile)
	mux.HandleFunc("/api/system", h.handleSystemStats)
	mux.HandleFunc("/health", h.handleHealth)
	mux.HandleFunc("/docs/openapi.json", h.handleOpenAPISpec)
	mux.HandleFunc("/docs", h.handleSwaggerUI)
	mux.HandleFunc("/", h.handleWebUI)
}

// handleHealth returns a simple health check.
func (h *Handler) handleHealth(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

// handleCameras returns the list of cameras.
func (h *Handler) handleCameras(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	cameras := h.supervisor.GetCameraNames()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(CameraListResponse{Cameras: cameras})
}

// handleCameraConfig handles requests under /api/cameras/{name}/.
func (h *Handler) handleCameraConfig(w http.ResponseWriter, r *http.Request) {
	// Extract camera name and sub-resource from path: /api/cameras/{name}/{sub}
	path := strings.TrimPrefix(r.URL.Path, "/api/cameras/")
	parts := strings.Split(path, "/")

	if len(parts) < 1 || parts[0] == "" {
		http.Error(w, "Not found", http.StatusNotFound)
		return
	}

	camName := parts[0]

	// /api/cameras/stats — aggregate stats for all cameras.
	if camName == "stats" && len(parts) == 1 {
		h.handleAllCameraStats(w, r)
		return
	}

	if len(parts) < 2 {
		http.Error(w, "Not found", http.StatusNotFound)
		return
	}

	sub := parts[1]

	switch sub {
	case "config":
		switch r.Method {
		case http.MethodGet:
			h.getInspectorConfig(w, camName)
		case http.MethodPut:
			h.updateInspectorConfig(w, r, camName)
		default:
			http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		}
	case "events":
		h.handleCameraEvents(w, r, camName)
	default:
		http.Error(w, "Not found", http.StatusNotFound)
	}
}

// handleCameraEvents streams per-camera motion events from the JSONL event log.
// GET /api/cameras/{name}/events?from=<RFC3339>&to=<RFC3339>&limit=<int>
func (h *Handler) handleCameraEvents(w http.ResponseWriter, r *http.Request, camName string) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Parse query parameters.
	from := time.Now().Add(-24 * time.Hour)
	to := time.Now()
	limit := 10000

	if s := r.URL.Query().Get("from"); s != "" {
		if t, err := time.Parse(time.RFC3339, s); err == nil {
			from = t
		}
	}
	if s := r.URL.Query().Get("to"); s != "" {
		if t, err := time.Parse(time.RFC3339, s); err == nil {
			to = t
		}
	}
	if s := r.URL.Query().Get("limit"); s != "" {
		if n, err := strconv.Atoi(s); err == nil && n > 0 {
			limit = n
		}
	}

	reader, err := eventlog.Open(h.eventLogRoot, camName)
	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusInternalServerError)
		json.NewEncoder(w).Encode(ErrorResponse{Error: fmt.Sprintf("open event log: %v", err)})
		return
	}

	events, err := reader.Range(from, to, limit)
	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusInternalServerError)
		json.NewEncoder(w).Encode(ErrorResponse{Error: fmt.Sprintf("read events: %v", err)})
		return
	}

	w.Header().Set("Content-Type", "application/x-ndjson")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	enc := json.NewEncoder(w)
	for i := range events {
		if err := enc.Encode(&events[i]); err != nil {
			log.Printf("API: events encode error for %s: %v", camName, err)
			return
		}
	}
}

// handleAllCameraStats returns aggregated z-score / signal metrics for all cameras.
// GET /api/cameras/stats
func (h *Handler) handleAllCameraStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	to := time.Now()
	from := to.Add(-24 * time.Hour)

	cameras := h.supervisor.GetCameraNames()
	entries := make([]CameraStatsEntry, 0, len(cameras))

	for _, cam := range cameras {
		reader, err := eventlog.Open(h.eventLogRoot, cam)
		if err != nil {
			entries = append(entries, CameraStatsEntry{Name: cam})
			continue
		}
		events, err := reader.Range(from, to, 10000)
		if err != nil || len(events) == 0 {
			entries = append(entries, CameraStatsEntry{Name: cam})
			continue
		}

		var zSum, irSum, zMax float64
		for _, e := range events {
			zSum += e.ZScore
			irSum += e.IntraRatio
			if e.ZScore > zMax {
				zMax = e.ZScore
			}
		}
		count := len(events)
		last := events[count-1]
		lastTS := last.TS

		entries = append(entries, CameraStatsEntry{
			Name:           cam,
			EventCount24h:  count,
			LastEventTS:    &lastTS,
			ZScoreLast:     last.ZScore,
			ZScoreAvg:      zSum / float64(count),
			ZScoreMax:      zMax,
			BPFSlowLast:    last.BPFSlow,
			BPFEwmaLast:    last.BPFEwma,
			IntraRatioLast: last.IntraRatio,
			IntraRatioAvg:  irSum / float64(count),
		})
	}

	sort.Slice(entries, func(i, j int) bool {
		return entries[i].Name < entries[j].Name
	})

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	json.NewEncoder(w).Encode(AllCameraStatsResponse{Cameras: entries, From: from, To: to})
}

// getInspectorConfig returns the current inspector configuration for a camera.
func (h *Handler) getInspectorConfig(w http.ResponseWriter, camName string) {
	recorder := h.supervisor.GetRecorder()
	cfg, err := recorder.GetInspectorConfig(camName)
	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: err.Error()})
		return
	}

	response := InspectorConfigResponse{
		MotionZHigh:          cfg.MotionZHigh,
		IntraRatioHigh:       cfg.IntraRatioHigh,
		OnThreshold:          cfg.OnThreshold,
		OffThreshold:         cfg.OffThreshold,
		BPFFloor:             cfg.BPFFloor,
		ConfiguredPeriodicKF: cfg.ConfiguredPeriodicKF,
		GradualEnabled:       cfg.GradualEnabled,
		GradualThreshold:     cfg.GradualThreshold,
		GradualWindowFrames:  cfg.GradualWindowFrames,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

// updateInspectorConfig updates the inspector configuration for a camera.
func (h *Handler) updateInspectorConfig(w http.ResponseWriter, r *http.Request, camName string) {
	recorder := h.supervisor.GetRecorder()

	// Get current config
	currentCfg, err := recorder.GetInspectorConfig(camName)
	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: err.Error()})
		return
	}

	// Parse request body
	var req InspectorConfigRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: fmt.Sprintf("Invalid JSON: %v", err)})
		return
	}

	// Apply updates (only fields that were provided)
	if req.MotionZHigh != nil {
		currentCfg.MotionZHigh = *req.MotionZHigh
	}
	if req.IntraRatioHigh != nil {
		currentCfg.IntraRatioHigh = *req.IntraRatioHigh
	}
	if req.OnThreshold != nil {
		currentCfg.OnThreshold = *req.OnThreshold
	}
	if req.OffThreshold != nil {
		currentCfg.OffThreshold = *req.OffThreshold
	}
	if req.BPFFloor != nil {
		currentCfg.BPFFloor = *req.BPFFloor
	}
	if req.ConfiguredPeriodicKF != nil {
		currentCfg.ConfiguredPeriodicKF = *req.ConfiguredPeriodicKF
	}
	if req.GradualEnabled != nil {
		currentCfg.GradualEnabled = *req.GradualEnabled
	}
	if req.GradualThreshold != nil {
		currentCfg.GradualThreshold = *req.GradualThreshold
	}
	if req.GradualWindowFrames != nil {
		currentCfg.GradualWindowFrames = *req.GradualWindowFrames
	}

	// Validate parameters
	if currentCfg.MotionZHigh < 0 || currentCfg.MotionZHigh > 100 {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "motion_z_high must be between 0 and 100"})
		return
	}
	if currentCfg.IntraRatioHigh < 0 || currentCfg.IntraRatioHigh > 100 {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "intra_ratio_high must be between 0 and 100"})
		return
	}
	if currentCfg.OnThreshold == 0 || currentCfg.OnThreshold > 255 {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "on_threshold must be between 1 and 255"})
		return
	}
	if currentCfg.OffThreshold == 0 || currentCfg.OffThreshold > 255 {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "off_threshold must be between 1 and 255"})
		return
	}

	// Update configuration
	if err := recorder.UpdateInspectorConfig(camName, currentCfg); err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusInternalServerError)
		json.NewEncoder(w).Encode(ErrorResponse{Error: fmt.Sprintf("Failed to update config: %v", err)})
		return
	}

	log.Printf("API: updated inspector config for camera %s (motion_z_high=%.1f, on_threshold=%d)",
		camName, currentCfg.MotionZHigh, currentCfg.OnThreshold)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(SuccessResponse{
		Success: true,
		Message: fmt.Sprintf("Configuration updated for camera %s", camName),
	})
}

// ClipInfo represents metadata about a video clip.
type ClipInfo struct {
	Camera   string           `json:"camera"`
	Filename string           `json:"filename"`
	Path     string           `json:"path"`
	Size     int64            `json:"size"`
	ModTime  time.Time        `json:"mod_time"`
	URL      string           `json:"url"`
	Label    string           `json:"label,omitempty"` // "tp", "fp", "reference", or ""
	Meta     *agent.ClipMeta  `json:"meta,omitempty"`  // trigger stats sidecar, nil if not present
}

// ClipsListResponse represents a paginated list of clips.
type ClipsListResponse struct {
	Clips      []ClipInfo `json:"clips"`
	Total      int        `json:"total"`       // total matching clips across all pages
	Page       int        `json:"page"`        // current page (1-indexed)
	PageSize   int        `json:"page_size"`   // clips per page
	TotalPages int        `json:"total_pages"` // ceil(total/page_size)
}

// ClipLabel is stored as a sidecar JSON file alongside each labeled clip.
type ClipLabel struct {
	Label     string    `json:"label"`      // "tp", "fp", or "reference"
	LabeledAt time.Time `json:"labeled_at"`
	Camera    string    `json:"camera"`
	Clip      string    `json:"clip"`
}

// LabelRequest is the body for POST /api/clips/{camera}/{filename}/label.
type LabelRequest struct {
	Label string `json:"label"` // "tp", "fp", or "reference"
}

// clipLabelPath returns the sidecar file path for a clip.
func clipLabelPath(clipFilePath string) string {
	return clipFilePath + ".label.json"
}

// readClipMeta reads the trigger-stats sidecar for a clip; returns nil if absent.
func readClipMeta(clipFilePath string) *agent.ClipMeta {
	b, err := os.ReadFile(clipFilePath + ".meta.json")
	if err != nil {
		return nil
	}
	var m agent.ClipMeta
	if err := json.Unmarshal(b, &m); err != nil {
		return nil
	}
	return &m
}

// readClipLabel reads the sidecar label for a clip; returns nil if unlabeled.
func readClipLabel(clipFilePath string) *ClipLabel {
	b, err := os.ReadFile(clipLabelPath(clipFilePath))
	if err != nil {
		return nil
	}
	var l ClipLabel
	if err := json.Unmarshal(b, &l); err != nil {
		return nil
	}
	return &l
}

// writeClipLabel atomically writes a sidecar label for a clip.
func writeClipLabel(clipFilePath string, label ClipLabel) error {
	b, err := json.MarshalIndent(label, "", "  ")
	if err != nil {
		return err
	}
	tmp := clipLabelPath(clipFilePath) + ".tmp"
	if err := os.WriteFile(tmp, b, 0640); err != nil {
		return err
	}
	return os.Rename(tmp, clipLabelPath(clipFilePath))
}

// handleClipsList returns a list of all recorded clips.
func (h *Handler) handleClipsList(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	q := r.URL.Query()
	camera := q.Get("camera")

	// Pagination parameters: page is 1-indexed, default page_size 50, max 500.
	page, pageSize := 1, 50
	if s := q.Get("page"); s != "" {
		if n, err := strconv.Atoi(s); err == nil && n > 0 {
			page = n
		}
	}
	if s := q.Get("page_size"); s != "" {
		if n, err := strconv.Atoi(s); err == nil && n > 0 && n <= 500 {
			pageSize = n
		}
	}

	clips, err := h.scanClips(camera)
	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusInternalServerError)
		json.NewEncoder(w).Encode(ErrorResponse{Error: fmt.Sprintf("Failed to scan clips: %v", err)})
		return
	}

	total := len(clips)
	totalPages := (total + pageSize - 1) / pageSize
	if totalPages == 0 {
		totalPages = 1
	}
	if page > totalPages {
		page = totalPages
	}
	start := (page - 1) * pageSize
	end := start + pageSize
	if end > total {
		end = total
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(ClipsListResponse{
		Clips:      clips[start:end],
		Total:      total,
		Page:       page,
		PageSize:   pageSize,
		TotalPages: totalPages,
	})
}

// scanClips scans the clip directory and returns metadata for all clips.
func (h *Handler) scanClips(cameraFilter string) ([]ClipInfo, error) {
	var clips []ClipInfo

	// If camera filter is provided, only scan that camera's directory
	var searchDirs []string
	if cameraFilter != "" {
		searchDirs = []string{filepath.Join(h.clipRoot, cameraFilter)}
	} else {
		// Scan all camera directories
		entries, err := os.ReadDir(h.clipRoot)
		if err != nil {
			return nil, err
		}
		for _, entry := range entries {
			if entry.IsDir() {
				searchDirs = append(searchDirs, filepath.Join(h.clipRoot, entry.Name()))
			}
		}
	}

	for _, dir := range searchDirs {
		cameraName := filepath.Base(dir)

		err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
			if err != nil {
				return nil // Skip errors for individual files
			}

			if info.IsDir() {
				return nil
			}

			// Only include video clip files
			ext := filepath.Ext(path)
			if ext == ".ts" || ext == ".mpegts" || ext == ".mp4" || ext == ".fmp4" || ext == ".mkv" {
				filename := filepath.Base(path)

				// For MPEG-TS files, expose via .m3u8 HLS manifest
				displayFilename := filename
				urlPath := filename
				if ext == ".mpegts" || ext == ".ts" {
					// Display original filename but URL points to .m3u8
					urlPath = strings.TrimSuffix(filename, ext) + ".m3u8"
				}

				label := ""
				if l := readClipLabel(path); l != nil {
					label = l.Label
				}
				clips = append(clips, ClipInfo{
					Camera:   cameraName,
					Filename: displayFilename,
					Path:     path,
					Size:     info.Size(),
					ModTime:  info.ModTime(),
					URL:      fmt.Sprintf("/api/clips/%s/%s", cameraName, urlPath),
					Label:    label,
					Meta:     readClipMeta(path),
				})
			}

			return nil
		})

		if err != nil {
			log.Printf("API: error scanning %s: %v", dir, err)
		}
	}

	// Sort by modification time (newest first)
	sort.Slice(clips, func(i, j int) bool {
		return clips[i].ModTime.After(clips[j].ModTime)
	})

	return clips, nil
}

// handleClipFile serves a specific clip file, and dispatches label sub-resource requests.
func (h *Handler) handleClipFile(w http.ResponseWriter, r *http.Request) {
	// Dispatch label sub-resource: /api/clips/{camera}/{filename}/label
	if strings.HasSuffix(r.URL.Path, "/label") {
		h.handleClipLabel(w, r)
		return
	}

	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Extract camera and filename from path: /api/clips/{camera}/{filename}
	path := strings.TrimPrefix(r.URL.Path, "/api/clips/")
	parts := strings.SplitN(path, "/", 2)

	if len(parts) != 2 {
		http.Error(w, "Invalid path format", http.StatusBadRequest)
		return
	}

	camera := parts[0]
	filename := parts[1]

	// Validate inputs to prevent path traversal
	if strings.Contains(camera, "..") || strings.Contains(filename, "..") {
		http.Error(w, "Invalid path", http.StatusBadRequest)
		return
	}

	// Handle HLS manifest requests (.m3u8)
	if strings.HasSuffix(filename, ".m3u8") {
		h.handleHLSManifest(w, r, camera, filename)
		return
	}

	// Handle MP4 conversion requests (request .mp4 for a .mpegts file)
	if strings.HasSuffix(filename, ".mp4") {
		baseName := strings.TrimSuffix(filename, ".mp4")
		mpegtsFile := baseName + ".mpegts"
		mpegtsPath := filepath.Join(h.clipRoot, camera, mpegtsFile)
		if _, err := os.Stat(mpegtsPath); err == nil {
			// MPEG-TS file exists, convert to MP4 on the fly
			h.handleMpegtsToMp4(w, r, mpegtsPath, filename)
			return
		}
	}

	// Construct file path
	filePath := filepath.Join(h.clipRoot, camera, filename)

	// Verify file exists and is within clipRoot
	absFilePath, err := filepath.Abs(filePath)
	if err != nil {
		http.Error(w, "Invalid path", http.StatusBadRequest)
		return
	}

	absClipRoot, err := filepath.Abs(h.clipRoot)
	if err != nil {
		http.Error(w, "Internal error", http.StatusInternalServerError)
		return
	}

	if !strings.HasPrefix(absFilePath, absClipRoot) {
		http.Error(w, "Access denied", http.StatusForbidden)
		return
	}

	// Check file exists
	fileInfo, err := os.Stat(filePath)
	if err != nil {
		if os.IsNotExist(err) {
			http.Error(w, "Clip not found", http.StatusNotFound)
		} else {
			http.Error(w, "Internal error", http.StatusInternalServerError)
		}
		return
	}

	// Set appropriate headers for video streaming
	ext := filepath.Ext(filename)
	switch ext {
	case ".ts", ".mpegts":
		w.Header().Set("Content-Type", "video/mp2t")
	case ".mp4", ".fmp4":
		w.Header().Set("Content-Type", "video/mp4")
	case ".mkv":
		w.Header().Set("Content-Type", "video/x-matroska")
	case ".json":
		w.Header().Set("Content-Type", "application/json")
	default:
		w.Header().Set("Content-Type", "application/octet-stream")
	}

	// Enable range requests for seeking and CORS
	w.Header().Set("Accept-Ranges", "bytes")
	w.Header().Set("Cache-Control", "public, max-age=3600")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	// Open file for ServeContent
	file, err := os.Open(filePath)
	if err != nil {
		http.Error(w, "Failed to open file", http.StatusInternalServerError)
		return
	}
	defer file.Close()

	// Serve the file with range request support
	http.ServeContent(w, r, filename, fileInfo.ModTime(), file)
}

// handleMpegtsToMp4 converts an MPEG-TS file to MP4 on-the-fly.
func (h *Handler) handleMpegtsToMp4(w http.ResponseWriter, r *http.Request, mpegtsPath string, mp4Filename string) {
	// Set headers for MP4 streaming
	w.Header().Set("Content-Type", "video/mp4")
	w.Header().Set("Cache-Control", "public, max-age=3600")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	// Use ffmpeg to convert MPEG-TS to MP4 (remux only, no transcoding)
	cmd := exec.Command("ffmpeg",
		"-i", mpegtsPath,
		"-c", "copy", // Copy streams without re-encoding
		"-f", "mp4",
		"-movflags", "frag_keyframe+empty_moov", // Enable streaming
		"-",
	)

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		http.Error(w, "Failed to start conversion", http.StatusInternalServerError)
		return
	}

	if err := cmd.Start(); err != nil {
		http.Error(w, "Failed to start conversion", http.StatusInternalServerError)
		return
	}

	// Stream the output directly to the client
	_, copyErr := io.Copy(w, stdout)
	waitErr := cmd.Wait()

	if copyErr != nil || waitErr != nil {
		log.Printf("API: error converting %s: copy=%v wait=%v", mpegtsPath, copyErr, waitErr)
	}
}

// handleHLSManifest generates an HLS manifest for an MPEG-TS clip.
func (h *Handler) handleHLSManifest(w http.ResponseWriter, r *http.Request, camera string, manifestFilename string) {
	// Extract the base filename (remove .m3u8 extension)
	baseName := strings.TrimSuffix(manifestFilename, ".m3u8")
	tsFilename := baseName + ".mpegts"

	// Check if the .mpegts file exists
	tsPath := filepath.Join(h.clipRoot, camera, tsFilename)
	fileInfo, err := os.Stat(tsPath)
	if err != nil {
		if os.IsNotExist(err) {
			http.Error(w, "Clip not found", http.StatusNotFound)
		} else {
			http.Error(w, "Internal error", http.StatusInternalServerError)
		}
		return
	}

	// Get clip duration (approximate from file size, assuming 3Mbps average bitrate)
	// Cameras configured for 8Mbps max, but motion clips average ~3Mbps
	durationSec := float64(fileInfo.Size()) * 8 / (3 * 1024 * 1024)
	if durationSec < 1 {
		durationSec = 8.0 // Default to 8 seconds
	}

	// Generate HLS VOD manifest with independent segments flag
	manifest := fmt.Sprintf(`#EXTM3U
#EXT-X-VERSION:3
#EXT-X-PLAYLIST-TYPE:VOD
#EXT-X-INDEPENDENT-SEGMENTS
#EXT-X-TARGETDURATION:%d
#EXTINF:%.2f,
/api/clips/%s/%s
#EXT-X-ENDLIST
`, int(durationSec)+1, durationSec, camera, tsFilename)

	w.Header().Set("Content-Type", "application/vnd.apple.mpegurl")
	w.Header().Set("Cache-Control", "public, max-age=3600")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.WriteHeader(http.StatusOK)
	w.Write([]byte(manifest))
}

// handleWebUI serves the web UI.
func (h *Handler) handleWebUI(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write([]byte(webUIHTML))
}

// handleOpenAPISpec serves the OpenAPI 3.0 specification as JSON.
func (h *Handler) handleOpenAPISpec(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Write([]byte(openAPISpec))
}

// handleClipLabel handles GET/POST/DELETE for /api/clips/{camera}/{filename}/label.
// Labels are stored as sidecar JSON files alongside the clip on disk.
func (h *Handler) handleClipLabel(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Header().Set("Content-Type", "application/json")

	// Strip /label suffix then split into camera + filename.
	trimmed := strings.TrimSuffix(strings.TrimPrefix(r.URL.Path, "/api/clips/"), "/label")
	parts := strings.SplitN(trimmed, "/", 2)
	if len(parts) != 2 || parts[0] == "" || parts[1] == "" {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid clip path"})
		return
	}
	camera, filename := parts[0], parts[1]

	if strings.Contains(camera, "..") || strings.Contains(filename, "..") {
		w.WriteHeader(http.StatusForbidden)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid path"})
		return
	}

	clipPath := filepath.Join(h.clipRoot, camera, filename)
	if _, err := os.Stat(clipPath); os.IsNotExist(err) {
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "clip not found"})
		return
	}

	switch r.Method {
	case http.MethodGet:
		lbl := readClipLabel(clipPath)
		if lbl == nil {
			w.WriteHeader(http.StatusNotFound)
			json.NewEncoder(w).Encode(ErrorResponse{Error: "not labeled"})
			return
		}
		json.NewEncoder(w).Encode(lbl)

	case http.MethodPost:
		var req LabelRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			w.WriteHeader(http.StatusBadRequest)
			json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid JSON"})
			return
		}
		switch req.Label {
		case "tp", "fp", "reference":
			// valid
		default:
			w.WriteHeader(http.StatusBadRequest)
			json.NewEncoder(w).Encode(ErrorResponse{Error: `label must be "tp", "fp", or "reference"`})
			return
		}
		lbl := ClipLabel{
			Label:     req.Label,
			LabeledAt: time.Now().UTC(),
			Camera:    camera,
			Clip:      filename,
		}
		if err := writeClipLabel(clipPath, lbl); err != nil {
			w.WriteHeader(http.StatusInternalServerError)
			json.NewEncoder(w).Encode(ErrorResponse{Error: fmt.Sprintf("write label: %v", err)})
			return
		}
		json.NewEncoder(w).Encode(lbl)

	case http.MethodDelete:
		err := os.Remove(clipLabelPath(clipPath))
		if err != nil && !os.IsNotExist(err) {
			w.WriteHeader(http.StatusInternalServerError)
			json.NewEncoder(w).Encode(ErrorResponse{Error: fmt.Sprintf("remove label: %v", err)})
			return
		}
		json.NewEncoder(w).Encode(SuccessResponse{Success: true, Message: "label removed"})

	default:
		w.WriteHeader(http.StatusMethodNotAllowed)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "method not allowed"})
	}
}

// handleSystemStats returns disk, memory, and CPU usage for the host.
func (h *Handler) handleSystemStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	stats := SystemStatsResponse{
		Disk:   h.getDiskStats(),
		Memory: h.getMemoryStats(),
		CPU:    h.getCPUStats(),
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(stats)
}

func (h *Handler) getDiskStats() DiskStats {
	path := h.clipRoot
	if path == "" {
		path = "/"
	}

	// Use the data root (parent of clipRoot) so we account for clips, eventlog,
	// and inflight — all directories written by emd-agent under /var/lib/emd-agent.
	// On local-hostpath PVCs the volume is a directory on a shared host partition,
	// so syscall.Statfs returns the whole node's partition stats, not emd's usage.
	// Walk the data directory to get the actual bytes emd-agent has written.
	dataRoot := filepath.Dir(path)
	var emdUsed uint64
	_ = filepath.WalkDir(dataRoot, func(_ string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		if info, ie := d.Info(); ie == nil {
			emdUsed += uint64(info.Size()) //nolint:gosec
		}
		return nil
	})

	var stat syscall.Statfs_t
	if err := syscall.Statfs(path, &stat); err != nil {
		log.Printf("statfs %s: %v", path, err)
		return DiskStats{Path: path, UsedBytes: emdUsed}
	}
	total := stat.Blocks * uint64(stat.Bsize) //nolint:unconvert
	free := stat.Bavail * uint64(stat.Bsize)  //nolint:unconvert
	var pct float64
	if total > 0 {
		pct = math.Round(float64(emdUsed)/float64(total)*1000) / 10
	}
	return DiskStats{
		Path:        dataRoot,
		TotalBytes:  total,
		UsedBytes:   emdUsed,
		FreeBytes:   free,
		UsedPercent: pct,
	}
}

func (h *Handler) getMemoryStats() MemoryStats {
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)

	total, available := readProcMeminfo()
	used := total - available
	var pct float64
	if total > 0 {
		pct = math.Round(float64(used)/float64(total)*1000) / 10
	}
	return MemoryStats{
		TotalBytes:  total,
		UsedBytes:   used,
		FreeBytes:   available,
		UsedPercent: pct,
		GoHeapBytes: ms.HeapAlloc,
	}
}

// readProcMeminfo returns total and available bytes from /proc/meminfo.
func readProcMeminfo() (total, available uint64) {
	f, err := os.Open("/proc/meminfo")
	if err != nil {
		return 0, 0
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := scanner.Text()
		var key string
		var val uint64
		if _, err := fmt.Sscanf(line, "%s %d kB", &key, &val); err != nil {
			continue
		}
		switch key {
		case "MemTotal:":
			total = val * 1024
		case "MemAvailable:":
			available = val * 1024
		}
		if total > 0 && available > 0 {
			break
		}
	}
	return total, available
}

func (h *Handler) getCPUStats() CPUStats {
	h.cpuMu.Lock()
	if time.Since(h.cpuAt) < 10*time.Second {
		pct := h.cpuCached
		h.cpuMu.Unlock()
		return CPUStats{UsedPercent: pct, NumCPU: runtime.NumCPU()}
	}
	h.cpuMu.Unlock()

	pct := measureCPUPercent()

	h.cpuMu.Lock()
	h.cpuCached = pct
	h.cpuAt = time.Now()
	h.cpuMu.Unlock()

	return CPUStats{UsedPercent: pct, NumCPU: runtime.NumCPU()}
}

type cpuSample struct {
	user, nice, system, idle, iowait, irq, softirq, steal uint64
}

func readCPUSample() (cpuSample, error) {
	f, err := os.Open("/proc/stat")
	if err != nil {
		return cpuSample{}, err
	}
	defer f.Close()

	var s cpuSample
	var label string
	scanner := bufio.NewScanner(f)
	if scanner.Scan() {
		_, err = fmt.Sscanf(scanner.Text(), "%s %d %d %d %d %d %d %d %d",
			&label, &s.user, &s.nice, &s.system, &s.idle,
			&s.iowait, &s.irq, &s.softirq, &s.steal)
		if err != nil {
			return cpuSample{}, err
		}
	}
	return s, nil
}

// measureCPUPercent reads /proc/stat twice 200 ms apart and returns usage %.
// The result is cached for 10 s so the overhead only hits the first caller.
func measureCPUPercent() float64 {
	s1, err := readCPUSample()
	if err != nil {
		return 0
	}
	time.Sleep(200 * time.Millisecond)
	s2, err := readCPUSample()
	if err != nil {
		return 0
	}

	idle1 := s1.idle + s1.iowait
	total1 := s1.user + s1.nice + s1.system + s1.idle + s1.iowait + s1.irq + s1.softirq + s1.steal
	idle2 := s2.idle + s2.iowait
	total2 := s2.user + s2.nice + s2.system + s2.idle + s2.iowait + s2.irq + s2.softirq + s2.steal

	deltaTotal := total2 - total1
	deltaIdle := idle2 - idle1
	if deltaTotal == 0 {
		return 0
	}
	return math.Round(float64(deltaTotal-deltaIdle)/float64(deltaTotal)*1000) / 10
}

// handleSwaggerUI serves the Swagger UI for API documentation.
func (h *Handler) handleSwaggerUI(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/docs" && r.URL.Path != "/docs/" {
		http.NotFound(w, r)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write([]byte(swaggerUIHTML))
}

// webUIHTML is the embedded web UI for browsing and playing clips.
const webUIHTML = `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>EMD Agent - Clip Browser</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
            background: #0f172a;
            color: #e2e8f0;
            padding: 20px;
        }
        .container {
            max-width: 1400px;
            margin: 0 auto;
        }
        header {
            margin-bottom: 30px;
            padding-bottom: 20px;
            border-bottom: 2px solid #1e293b;
        }
        h1 {
            font-size: 2rem;
            color: #60a5fa;
            margin-bottom: 10px;
        }
        .subtitle {
            color: #94a3b8;
            font-size: 0.9rem;
        }
        .controls {
            display: flex;
            gap: 15px;
            margin-bottom: 25px;
            flex-wrap: wrap;
        }
        select, button {
            padding: 10px 15px;
            background: #1e293b;
            border: 1px solid #334155;
            color: #e2e8f0;
            border-radius: 6px;
            font-size: 0.9rem;
            cursor: pointer;
            transition: all 0.2s;
        }
        select:hover, button:hover {
            background: #334155;
            border-color: #475569;
        }
        button:active {
            transform: scale(0.98);
        }
        .stats {
            display: flex;
            gap: 20px;
            margin-bottom: 25px;
        }
        .stat {
            background: #1e293b;
            padding: 15px 20px;
            border-radius: 8px;
            border: 1px solid #334155;
        }
        .stat-label {
            font-size: 0.8rem;
            color: #94a3b8;
            margin-bottom: 5px;
        }
        .stat-value {
            font-size: 1.5rem;
            font-weight: 600;
            color: #60a5fa;
        }
        .sys-stats {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
            gap: 15px;
            margin-bottom: 25px;
        }
        .sys-stat {
            background: #1e293b;
            border: 1px solid #334155;
            border-radius: 8px;
            padding: 15px 20px;
        }
        .sys-stat-title {
            font-size: 0.75rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
            color: #64748b;
            margin-bottom: 6px;
        }
        .sys-stat-value {
            font-size: 1.1rem;
            font-weight: 600;
            color: #e2e8f0;
            margin-bottom: 8px;
        }
        .progress-bar {
            height: 6px;
            background: #334155;
            border-radius: 3px;
            overflow: hidden;
            margin-bottom: 6px;
        }
        .progress-fill {
            height: 100%;
            border-radius: 3px;
            transition: width 0.5s ease;
        }
        .progress-fill.ok   { background: #34d399; }
        .progress-fill.warn { background: #fbbf24; }
        .progress-fill.crit { background: #f87171; }
        .sys-stat-sub {
            font-size: 0.75rem;
            color: #64748b;
        }
        .label-buttons {
            display: flex;
            gap: 8px;
            margin-top: 14px;
            flex-wrap: wrap;
            align-items: center;
        }
        .label-btn {
            padding: 7px 16px;
            border-radius: 6px;
            border: 1px solid;
            cursor: pointer;
            font-size: 0.82rem;
            font-weight: 500;
            background: transparent;
            transition: all 0.15s;
        }
        .label-btn.tp  { border-color: #34d399; color: #34d399; }
        .label-btn.fp  { border-color: #f87171; color: #f87171; }
        .label-btn.ref { border-color: #fbbf24; color: #fbbf24; }
        .label-btn.clear { border-color: #475569; color: #64748b; }
        .label-btn.tp:hover,  .label-btn.tp.active  { background: #34d399; color: #0f172a; }
        .label-btn.fp:hover,  .label-btn.fp.active  { background: #f87171; color: #0f172a; }
        .label-btn.ref:hover, .label-btn.ref.active { background: #fbbf24; color: #0f172a; }
        .label-btn.clear:hover { background: #334155; color: #e2e8f0; }
        .label-status {
            font-size: 0.78rem;
            color: #64748b;
            margin-left: 4px;
        }
        .spike-panel {
            display: none;
            margin-top: 14px;
            padding: 12px 14px;
            background: #0f172a;
            border: 1px solid #334155;
            border-radius: 6px;
            font-size: 0.82rem;
        }
        .spike-panel.visible { display: block; }
        .spike-row {
            display: flex;
            flex-wrap: wrap;
            gap: 18px;
            align-items: center;
        }
        .spike-stat { display: flex; flex-direction: column; gap: 2px; }
        .spike-stat-label { font-size: 0.7rem; text-transform: uppercase; letter-spacing: 0.05em; color: #64748b; }
        .spike-stat-value { font-weight: 600; color: #e2e8f0; }
        .spike-stat-value.z-ok   { color: #34d399; }
        .spike-stat-value.z-warn { color: #fbbf24; }
        .spike-stat-value.z-crit { color: #f87171; }
        .spike-reason { font-size: 0.78rem; color: #64748b; margin-top: 8px; font-family: monospace; }
        .video-wrap { position: relative; }
        .spike-marker {
            display: none;
            position: absolute;
            bottom: 0;
            width: 2px;
            background: #f87171;
            pointer-events: none;
            z-index: 10;
        }
        .spike-marker-label {
            position: absolute;
            top: -18px;
            left: 2px;
            font-size: 0.65rem;
            color: #f87171;
            white-space: nowrap;
            font-weight: 600;
        }
        .label-badge {
            display: inline-block;
            font-size: 0.68rem;
            font-weight: 700;
            padding: 2px 6px;
            border-radius: 3px;
            text-transform: uppercase;
            letter-spacing: 0.06em;
            margin-left: 6px;
            vertical-align: middle;
        }
        .label-badge.tp        { background: #14532d; color: #86efac; }
        .label-badge.fp        { background: #7f1d1d; color: #fca5a5; }
        .label-badge.reference { background: #713f12; color: #fde68a; }
        .player-section {
            background: #1e293b;
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 30px;
            border: 1px solid #334155;
            display: none;
        }
        .player-section.active {
            display: block;
        }
        .player-title {
            font-size: 1.2rem;
            margin-bottom: 15px;
            color: #60a5fa;
        }
        video {
            width: 100%;
            max-height: 500px;
            background: #000;
            border-radius: 8px;
        }
        .clips-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
            gap: 20px;
        }
        .clip-card {
            background: #1e293b;
            border: 1px solid #334155;
            border-radius: 10px;
            padding: 15px;
            cursor: pointer;
            transition: all 0.2s;
        }
        .clip-card:hover {
            border-color: #60a5fa;
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(96, 165, 250, 0.2);
        }
        .clip-card.playing {
            border-color: #34d399;
            background: #1e3a2b;
        }
        .clip-camera {
            font-size: 0.85rem;
            color: #94a3b8;
            margin-bottom: 8px;
        }
        .clip-filename {
            font-weight: 500;
            margin-bottom: 10px;
            color: #e2e8f0;
            word-break: break-all;
        }
        .clip-meta {
            display: flex;
            justify-content: space-between;
            font-size: 0.8rem;
            color: #64748b;
        }
        .loading {
            text-align: center;
            padding: 40px;
            color: #94a3b8;
        }
        .error {
            background: #7f1d1d;
            border: 1px solid #991b1b;
            color: #fca5a5;
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 20px;
        }
        .empty-state {
            text-align: center;
            padding: 60px 20px;
            color: #64748b;
        }
        .empty-state svg {
            width: 64px;
            height: 64px;
            margin-bottom: 20px;
            opacity: 0.5;
        }
        .pagination {
            display: flex;
            align-items: center;
            gap: 8px;
            margin-top: 24px;
            justify-content: center;
            flex-wrap: wrap;
        }
        .page-btn {
            padding: 7px 14px;
            background: #1e293b;
            border: 1px solid #334155;
            color: #e2e8f0;
            border-radius: 6px;
            cursor: pointer;
            font-size: 0.85rem;
            transition: all 0.15s;
            min-width: 38px;
        }
        .page-btn:hover:not(:disabled) { background: #334155; border-color: #475569; }
        .page-btn:disabled { opacity: 0.35; cursor: default; }
        .page-btn.active { background: #2563eb; border-color: #3b82f6; color: #fff; }
        .page-info {
            font-size: 0.85rem;
            color: #94a3b8;
            padding: 0 6px;
        }
        .page-size-select {
            padding: 6px 10px;
            background: #1e293b;
            border: 1px solid #334155;
            color: #e2e8f0;
            border-radius: 6px;
            font-size: 0.82rem;
            cursor: pointer;
        }
        .signal-section {
            background: #1e293b;
            border: 1px solid #334155;
            border-radius: 8px;
            margin-bottom: 25px;
            overflow: hidden;
        }
        .signal-header {
            padding: 12px 20px;
            font-size: 0.9rem;
            font-weight: 600;
            color: #94a3b8;
            cursor: pointer;
            display: flex;
            align-items: center;
            gap: 8px;
            user-select: none;
        }
        .signal-header:hover { background: #243044; }
        .signal-toggle { margin-left: auto; font-size: 0.75rem; color: #64748b; }
        .signal-table-wrap { overflow-x: auto; }
        .signal-table {
            width: 100%;
            border-collapse: collapse;
            font-size: 0.82rem;
        }
        .signal-table th {
            padding: 8px 14px;
            text-align: left;
            font-size: 0.72rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
            color: #64748b;
            border-bottom: 1px solid #334155;
            white-space: nowrap;
        }
        .signal-table td {
            padding: 7px 14px;
            border-bottom: 1px solid #1e293b;
            color: #cbd5e1;
            white-space: nowrap;
        }
        .signal-table tr:last-child td { border-bottom: none; }
        .signal-table tr:hover td { background: #243044; }
        .z-chip {
            display: inline-block;
            padding: 2px 8px;
            border-radius: 4px;
            font-weight: 600;
            font-size: 0.8rem;
        }
        .z-ok   { background: #14532d; color: #86efac; }
        .z-warn { background: #713f12; color: #fde68a; }
        .z-crit { background: #7f1d1d; color: #fca5a5; }
        .cam-name-cell { color: #60a5fa; font-weight: 500; }
        .no-data-cell { color: #475569; font-style: italic; }
    </style>
    <script src="https://cdn.jsdelivr.net/npm/hls.js@latest"></script>
</head>
<body>
    <div class="container">
        <header>
            <h1>🎥 EMD Agent - Clip Browser</h1>
            <div class="subtitle">Motion detection video clips</div>
        </header>

        <div class="controls">
            <select id="cameraFilter">
                <option value="">All Cameras</option>
            </select>
            <select id="pageSizeSelect" class="page-size-select" onchange="onPageSizeChange()">
                <option value="25">25 per page</option>
                <option value="50" selected>50 per page</option>
                <option value="100">100 per page</option>
                <option value="250">250 per page</option>
            </select>
            <button onclick="refreshClips()">🔄 Refresh</button>
        </div>

        <div class="stats">
            <div class="stat">
                <div class="stat-label">Total Clips</div>
                <div class="stat-value" id="totalClips">0</div>
            </div>
            <div class="stat">
                <div class="stat-label">Total Size</div>
                <div class="stat-value" id="totalSize">0 MB</div>
            </div>
            <div class="stat">
                <div class="stat-label">Cameras</div>
                <div class="stat-value" id="totalCameras">0</div>
            </div>
        </div>

        <div class="sys-stats">
            <div class="sys-stat">
                <div class="sys-stat-title">Disk (clips volume)</div>
                <div class="sys-stat-value" id="diskValue">—</div>
                <div class="progress-bar"><div class="progress-fill ok" id="diskBar" style="width:0%"></div></div>
                <div class="sys-stat-sub" id="diskSub">Loading…</div>
            </div>
            <div class="sys-stat">
                <div class="sys-stat-title">Memory</div>
                <div class="sys-stat-value" id="memValue">—</div>
                <div class="progress-bar"><div class="progress-fill ok" id="memBar" style="width:0%"></div></div>
                <div class="sys-stat-sub" id="memSub">Loading…</div>
            </div>
            <div class="sys-stat">
                <div class="sys-stat-title">CPU</div>
                <div class="sys-stat-value" id="cpuValue">—</div>
                <div class="progress-bar"><div class="progress-fill ok" id="cpuBar" style="width:0%"></div></div>
                <div class="sys-stat-sub" id="cpuSub">Loading…</div>
            </div>
        </div>

        <div class="signal-section">
            <div class="signal-header" onclick="toggleSignalPanel()">
                📊 Camera Signal Metrics (24 h)
                <span class="signal-toggle" id="signalToggle">▼</span>
            </div>
            <div id="signalPanel">
                <div class="signal-table-wrap">
                    <table class="signal-table">
                        <thead>
                            <tr>
                                <th>Camera</th>
                                <th>Events</th>
                                <th>Last Event</th>
                                <th>Z-Score (last)</th>
                                <th>Z-Score (avg)</th>
                                <th>Z-Score (max)</th>
                                <th>BPF Slow</th>
                                <th>BPF EWMA</th>
                                <th>Intra Ratio (last)</th>
                            </tr>
                        </thead>
                        <tbody id="signalBody">
                            <tr><td colspan="9" style="text-align:center;color:#64748b;padding:20px">Loading…</td></tr>
                        </tbody>
                    </table>
                </div>
            </div>
        </div>

        <div id="errorContainer"></div>

        <div class="player-section" id="playerSection">
            <div class="player-title" id="playerTitle">Now Playing</div>
            <div class="video-wrap">
                <video id="videoPlayer" controls preload="metadata"></video>
                <div class="spike-marker" id="spikeMarker">
                    <span class="spike-marker-label">spike</span>
                </div>
            </div>
            <div class="label-buttons">
                <button class="label-btn tp"    onclick="setLabel('tp')">✓ True Positive</button>
                <button class="label-btn fp"    onclick="setLabel('fp')">✗ False Positive</button>
                <button class="label-btn ref"   onclick="setLabel('reference')">⭐ Reference</button>
                <button class="label-btn clear" onclick="clearLabel()">Clear</button>
                <button id="jumpSpikeBtn" style="display:none" onclick="jumpToSpike()"
                    title="Seek to motion trigger point">⚡ Jump to spike</button>
                <span class="label-status" id="labelStatus"></span>
            </div>
            <div class="spike-panel" id="spikePanel">
                <div class="spike-row" id="spikeRow"></div>
                <div class="spike-reason" id="spikeReason"></div>
            </div>
        </div>

        <div id="clipsContainer">
            <div class="loading">Loading clips...</div>
        </div>
        <div class="pagination" id="paginationBar"></div>
    </div>

    <script>
        let allClips = [];
        let currentClip = null;
        let currentPage = 1;
        let totalPages = 1;
        let totalClips = 0;

        async function loadClips(camera = '', page = 1) {
            currentPage = page;
            try {
                const pageSize = document.getElementById('pageSizeSelect').value;
                let url = ` + "`/api/clips?page=${page}&page_size=${pageSize}`" + `;
                if (camera) url += ` + "`&camera=${encodeURIComponent(camera)}`" + `;
                const response = await fetch(url);

                if (!response.ok) {
                    throw new Error(` + "`HTTP ${response.status}: ${response.statusText}`" + `);
                }

                const data = await response.json();
                allClips = data.clips || [];
                totalClips = data.total || 0;
                totalPages = data.total_pages || 1;
                currentPage = data.page || 1;

                displayClips();
                updateStats();
                updateCameraFilter();
                renderPagination();
                clearError();
            } catch (error) {
                showError('Failed to load clips: ' + error.message);
                document.getElementById('clipsContainer').innerHTML =
                    '<div class="empty-state">Failed to load clips</div>';
            }
        }

        function renderPagination() {
            const bar = document.getElementById('paginationBar');
            if (totalPages <= 1) { bar.innerHTML = ''; return; }

            const camera = document.getElementById('cameraFilter').value;

            // Build page window: always show first, last, current ±2
            const pages = new Set([1, totalPages]);
            for (let p = Math.max(1, currentPage - 2); p <= Math.min(totalPages, currentPage + 2); p++) {
                pages.add(p);
            }
            const sorted = [...pages].sort((a, b) => a - b);

            let html = ` + "`<button class=\"page-btn\" ${currentPage === 1 ? 'disabled' : ''} onclick=\"loadClips('${camera}', ${currentPage - 1})\">‹ Prev</button>`" + `;

            let prev = 0;
            for (const p of sorted) {
                if (p - prev > 1) html += ` + "`<span class=\"page-info\">…</span>`" + `;
                html += ` + "`<button class=\"page-btn ${p === currentPage ? 'active' : ''}\" onclick=\"loadClips('${camera}', ${p})\">${p}</button>`" + `;
                prev = p;
            }

            html += ` + "`<button class=\"page-btn\" ${currentPage === totalPages ? 'disabled' : ''} onclick=\"loadClips('${camera}', ${currentPage + 1})\">Next ›</button>`" + `;
            html += ` + "`<span class=\"page-info\">${totalClips.toLocaleString()} clips total</span>`" + `;

            bar.innerHTML = html;
        }

        function onPageSizeChange() {
            const camera = document.getElementById('cameraFilter').value;
            loadClips(camera, 1);
        }

        function displayClips() {
            const container = document.getElementById('clipsContainer');

            if (allClips.length === 0) {
                container.innerHTML = ` + "`" + `
                    <div class="empty-state">
                        <svg fill="currentColor" viewBox="0 0 20 20">
                            <path d="M2 6a2 2 0 012-2h6a2 2 0 012 2v8a2 2 0 01-2 2H4a2 2 0 01-2-2V6zm12.553 1.106A1 1 0 0014 8v4a1 1 0 00.553.894l2 1A1 1 0 0018 13V7a1 1 0 00-1.447-.894l-2 1z"/>
                        </svg>
                        <h3>No clips found</h3>
                        <p>Motion detection clips will appear here when recorded</p>
                    </div>
                ` + "`" + `;
                return;
            }

            const grid = document.createElement('div');
            grid.className = 'clips-grid';

            allClips.forEach(clip => {
                const card = document.createElement('div');
                card.className = 'clip-card';
                if (currentClip && currentClip.url === clip.url) {
                    card.classList.add('playing');
                }

                const badge = clip.label
                    ? ` + "`<span class=\"label-badge ${clip.label}\">${clip.label === 'reference' ? 'ref' : clip.label}</span>`" + `
                    : '';
                card.innerHTML = ` + "`" + `
                    <div class="clip-camera">📹 ${clip.camera}${badge}</div>
                    <div class="clip-filename">${clip.filename}</div>
                    <div class="clip-meta">
                        <span>${formatSize(clip.size)}</span>
                        <span>${formatTime(clip.mod_time)}</span>
                    </div>
                ` + "`" + `;

                card.onclick = () => playClip(clip);
                grid.appendChild(card);
            });

            container.innerHTML = '';
            container.appendChild(grid);
        }

        function playClip(clip) {
            console.log('=== PLAYCLIP START ===');
            console.log('clip object:', clip);

            currentClip = clip;
            const player = document.getElementById('videoPlayer');
            const section = document.getElementById('playerSection');
            const title = document.getElementById('playerTitle');

            // Destroy any existing HLS instance
            if (window.hls) {
                console.log('Destroying previous HLS instance');
                window.hls.destroy();
                window.hls = null;
            }

            // Reset player
            player.pause();
            player.removeAttribute('src');
            player.load();

            console.log('Playing clip:', clip.filename);
            console.log('URL:', clip.url);
            console.log('typeof Hls:', typeof Hls);

            // Check if this is an HLS manifest
            const isHLS = clip.url.endsWith('.m3u8');
            console.log('isHLS:', isHLS);
            console.log('Hls.isSupported():', typeof Hls !== 'undefined' ? Hls.isSupported() : 'Hls not defined');

            if (isHLS && typeof Hls !== 'undefined' && Hls.isSupported()) {
                try {
                    // Use hls.js for HLS streams
                    console.log('>>> Using hls.js for HLS playback');

                    window.hls = new Hls({
                        debug: true,
                        enableWorker: true,
                        lowLatencyMode: false,
                    });
                    console.log('>>> Created HLS instance');

                    // IMPORTANT: Attach media BEFORE loading source (recommended pattern)
                    window.hls.attachMedia(player);
                    console.log('>>> Attached media to player');

                    window.hls.on(Hls.Events.MEDIA_ATTACHED, function() {
                        console.log('>>> MEDIA_ATTACHED event fired');
                        console.log('>>> Now loading source:', clip.url);
                        window.hls.loadSource(clip.url);
                    });

                    window.hls.on(Hls.Events.MANIFEST_PARSED, function() {
                        console.log('>>> HLS manifest parsed successfully');
                        player.play().catch(e => console.error('>>> Play failed:', e));
                    });

                    window.hls.on(Hls.Events.ERROR, function(event, data) {
                        console.error('>>> HLS.js error:', event, data);
                        if (data.fatal) {
                            alert('HLS Error: ' + data.type + ' - ' + data.details);
                            switch(data.type) {
                                case Hls.ErrorTypes.NETWORK_ERROR:
                                    console.error('>>> Fatal network error, trying to recover');
                                    window.hls.startLoad();
                                    break;
                                case Hls.ErrorTypes.MEDIA_ERROR:
                                    console.error('>>> Fatal media error, trying to recover');
                                    window.hls.recoverMediaError();
                                    break;
                                default:
                                    console.error('>>> Unrecoverable error');
                                    window.hls.destroy();
                                    break;
                            }
                        }
                    });

                    window.hls.on(Hls.Events.FRAG_LOADED, function(event, data) {
                        console.log('>>> Fragment loaded:', data.frag.url);
                    });

                } catch (e) {
                    console.error('>>> Exception in HLS setup:', e);
                    alert('Error setting up HLS: ' + e.message);
                }
            } else if (isHLS && player.canPlayType('application/vnd.apple.mpegurl')) {
                // Safari native HLS support
                console.log('Using Safari native HLS');
                player.src = clip.url;
                player.load();
                player.play().catch(e => console.error('Play failed:', e));
            } else {
                // Direct playback for MP4 or other formats
                console.log('Direct playback:', clip.url);
                player.src = clip.url;
                player.load();
                player.play().catch(e => console.error('Play failed:', e));
            }

            title.textContent = ` + "`Now Playing: ${clip.camera} - ${clip.filename}`" + `;
            section.classList.add('active');
            section.scrollIntoView({ behavior: 'smooth', block: 'nearest' });

            syncLabelButtons(clip.label || '');
            showSpikePanel(clip);
            displayClips(); // Refresh to show playing state
        }

        let currentSpikeSec = null;

        function zClass(val) {
            if (val < 3.5) return 'z-ok';
            if (val < 6)   return 'z-warn';
            return 'z-crit';
        }

        function showSpikePanel(clip) {
            const meta = clip.meta;
            const panel = document.getElementById('spikePanel');
            const row   = document.getElementById('spikeRow');
            const reason = document.getElementById('spikeReason');
            const jumpBtn = document.getElementById('jumpSpikeBtn');
            const marker = document.getElementById('spikeMarker');

            if (!meta) {
                panel.classList.remove('visible');
                jumpBtn.style.display = 'none';
                marker.style.display = 'none';
                currentSpikeSec = null;
                return;
            }

            currentSpikeSec = meta.trigger_offset_ms / 1000;

            const stat = (label, value) =>
                ` + "`<div class=\"spike-stat\"><div class=\"spike-stat-label\">${label}</div><div class=\"spike-stat-value\">${value}</div></div>`" + `;
            const zVal = meta.z_score.toFixed(2);
            const zCls = zClass(meta.z_score);

            row.innerHTML =
                stat('Z-Score', ` + "`<span class=\"${zCls}\">${zVal}</span>`" + `) +
                stat('BPF Slow', fmtBPF(meta.bpf_slow)) +
                stat('BPF EWMA', fmtBPF(meta.bpf_ewma)) +
                stat('Intra Ratio', meta.intra_ratio.toFixed(3)) +
                stat('Pre-roll', ` + "`${meta.pre_roll_seconds}s`" + `) +
                stat('Post-roll', ` + "`${meta.post_roll_seconds}s`" + `) +
                (meta.clip_duration_ms ? stat('Duration', ` + "`${(meta.clip_duration_ms/1000).toFixed(1)}s`" + `) : '') +
                stat('Codec', ` + "`${meta.codec} @ ${meta.fps.toFixed(1)} fps`" + `);

            reason.textContent = meta.reason ? ` + "`Reason: ${meta.reason}`" + ` : '';
            panel.classList.add('visible');
            jumpBtn.style.display = '';

            // Position the spike marker on the video element.
            // The marker is placed at trigger_offset_ms / clip_duration_ms * 100%.
            // We use a rAF loop to update once duration is known.
            marker.style.display = 'none';
            const player = document.getElementById('videoPlayer');
            function tryPositionMarker() {
                if (!player.duration || !meta.trigger_offset_ms) return;
                const pct = (meta.trigger_offset_ms / 1000) / player.duration * 100;
                if (pct > 0 && pct < 100) {
                    // Height covers just the native controls progress bar area (~4px from bottom).
                    // We can't reach inside the shadow DOM, so overlay across the full player bottom.
                    marker.style.left = pct + '%';
                    marker.style.height = '100%';
                    marker.style.display = 'block';
                }
            }
            player.addEventListener('loadedmetadata', tryPositionMarker, { once: true });
            tryPositionMarker();
        }

        function jumpToSpike() {
            if (currentSpikeSec === null) return;
            const player = document.getElementById('videoPlayer');
            player.currentTime = currentSpikeSec;
            player.play().catch(() => {});
        }

        function syncLabelButtons(label) {
            document.querySelectorAll('.label-btn').forEach(b => b.classList.remove('active'));
            if (label === 'tp')        document.querySelector('.label-btn.tp').classList.add('active');
            if (label === 'fp')        document.querySelector('.label-btn.fp').classList.add('active');
            if (label === 'reference') document.querySelector('.label-btn.ref').classList.add('active');
            const statusEl = document.getElementById('labelStatus');
            statusEl.textContent = label ? ` + "`Labeled: ${label}`" + ` : 'Unlabeled';
        }

        async function setLabel(label) {
            if (!currentClip) return;
            const { camera, filename } = currentClip;
            try {
                const resp = await fetch(` + "`/api/clips/${encodeURIComponent(camera)}/${encodeURIComponent(filename)}/label`" + `, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ label }),
                });
                if (!resp.ok) {
                    const err = await resp.json().catch(() => ({ error: resp.statusText }));
                    throw new Error(err.error || resp.statusText);
                }
                currentClip.label = label;
                syncLabelButtons(label);
                displayClips();
            } catch (e) {
                showError('Failed to set label: ' + e.message);
            }
        }

        async function clearLabel() {
            if (!currentClip) return;
            const { camera, filename } = currentClip;
            try {
                await fetch(` + "`/api/clips/${encodeURIComponent(camera)}/${encodeURIComponent(filename)}/label`" + `, {
                    method: 'DELETE',
                });
                currentClip.label = '';
                syncLabelButtons('');
                displayClips();
            } catch (e) {
                showError('Failed to clear label: ' + e.message);
            }
        }

        function updateStats() {
            // totalClips is the full count from the API, not just the current page.
            document.getElementById('totalClips').textContent = totalClips.toLocaleString();

            const totalBytes = allClips.reduce((sum, clip) => sum + clip.size, 0);
            document.getElementById('totalSize').textContent = formatSize(totalBytes);

            const cameras = new Set(allClips.map(clip => clip.camera));
            document.getElementById('totalCameras').textContent = cameras.size;
        }

        function updateCameraFilter() {
            const select = document.getElementById('cameraFilter');
            const cameras = new Set(allClips.map(clip => clip.camera));

            // Keep current selection
            const currentValue = select.value;

            select.innerHTML = '<option value="">All Cameras</option>';
            [...cameras].sort().forEach(camera => {
                const option = document.createElement('option');
                option.value = camera;
                option.textContent = camera;
                select.appendChild(option);
            });

            select.value = currentValue;
        }

        function formatSize(bytes) {
            if (bytes === 0) return '0 B';
            const k = 1024;
            const sizes = ['B', 'KB', 'MB', 'GB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return Math.round((bytes / Math.pow(k, i)) * 100) / 100 + ' ' + sizes[i];
        }

        function formatTime(timestamp) {
            const date = new Date(timestamp);
            const now = new Date();
            const diff = now - date;
            const hours = Math.floor(diff / 3600000);

            if (hours < 1) {
                const minutes = Math.floor(diff / 60000);
                return minutes + 'm ago';
            } else if (hours < 24) {
                return hours + 'h ago';
            } else {
                const days = Math.floor(hours / 24);
                return days + 'd ago';
            }
        }

        async function loadSystemStats() {
            try {
                const resp = await fetch('/api/system');
                if (!resp.ok) return;
                const s = await resp.json();

                // Disk
                const diskPct = s.disk.used_percent;
                document.getElementById('diskValue').textContent =
                    formatSize(s.disk.used_bytes) + ' / ' + formatSize(s.disk.total_bytes);
                const diskBar = document.getElementById('diskBar');
                diskBar.style.width = diskPct + '%';
                diskBar.className = 'progress-fill ' + colorClass(diskPct);
                document.getElementById('diskSub').textContent =
                    formatSize(s.disk.free_bytes) + ' free · ' + diskPct.toFixed(1) + '% used';

                // Memory
                const memPct = s.memory.used_percent;
                document.getElementById('memValue').textContent =
                    formatSize(s.memory.used_bytes) + ' / ' + formatSize(s.memory.total_bytes);
                const memBar = document.getElementById('memBar');
                memBar.style.width = memPct + '%';
                memBar.className = 'progress-fill ' + colorClass(memPct);
                document.getElementById('memSub').textContent =
                    memPct.toFixed(1) + '% used · Go heap: ' + formatSize(s.memory.go_heap_bytes);

                // CPU
                const cpuPct = s.cpu.used_percent;
                document.getElementById('cpuValue').textContent = cpuPct.toFixed(1) + '%';
                const cpuBar = document.getElementById('cpuBar');
                cpuBar.style.width = cpuPct + '%';
                cpuBar.className = 'progress-fill ' + colorClass(cpuPct);
                document.getElementById('cpuSub').textContent = s.cpu.num_cpu + ' CPUs';
            } catch (_) {
                // non-fatal — system stats are informational
            }
        }

        function colorClass(pct) {
            if (pct < 70) return 'ok';
            if (pct < 85) return 'warn';
            return 'crit';
        }

        function showError(message) {
            const container = document.getElementById('errorContainer');
            container.innerHTML = ` + "`<div class=\"error\">${message}</div>`" + `;
        }

        function clearError() {
            document.getElementById('errorContainer').innerHTML = '';
        }

        function refreshClips() {
            const camera = document.getElementById('cameraFilter').value;
            loadClips(camera, currentPage);
        }

        // Camera filter resets to page 1
        document.getElementById('cameraFilter').addEventListener('change', () => {
            loadClips(document.getElementById('cameraFilter').value, 1);
        });

        let signalPanelOpen = true;
        function toggleSignalPanel() {
            signalPanelOpen = !signalPanelOpen;
            document.getElementById('signalPanel').style.display = signalPanelOpen ? '' : 'none';
            document.getElementById('signalToggle').textContent = signalPanelOpen ? '▼' : '▶';
        }

        function zChip(val) {
            if (val === null || val === undefined) return '—';
            const cls = val < 3.5 ? 'z-ok' : val < 6 ? 'z-warn' : 'z-crit';
            return ` + "`<span class=\"z-chip ${cls}\">${val.toFixed(2)}</span>`" + `;
        }

        function fmtBPF(val) {
            if (!val) return '—';
            if (val >= 1e6) return (val / 1e6).toFixed(2) + ' MB/f';
            if (val >= 1e3) return (val / 1e3).toFixed(1) + ' kB/f';
            return val.toFixed(0) + ' B/f';
        }

        function fmtAgo(tsStr) {
            if (!tsStr) return '—';
            const diff = Date.now() - new Date(tsStr).getTime();
            const m = Math.floor(diff / 60000);
            if (m < 1) return 'just now';
            if (m < 60) return m + 'm ago';
            const h = Math.floor(m / 60);
            if (h < 24) return h + 'h ago';
            return Math.floor(h / 24) + 'd ago';
        }

        async function loadSignalStats() {
            try {
                const resp = await fetch('/api/cameras/stats');
                if (!resp.ok) return;
                const data = await resp.json();
                const cameras = data.cameras || [];

                const tbody = document.getElementById('signalBody');
                if (cameras.length === 0) {
                    tbody.innerHTML = '<tr><td colspan="9" style="text-align:center;color:#64748b;padding:20px">No cameras registered</td></tr>';
                    return;
                }

                tbody.innerHTML = cameras.map(c => {
                    if (c.event_count_24h === 0) {
                        return ` + "`" + `<tr>
                            <td class="cam-name-cell">${c.name}</td>
                            <td>0</td>
                            <td class="no-data-cell" colspan="7">no events in 24 h</td>
                        </tr>` + "`" + `;
                    }
                    return ` + "`" + `<tr>
                        <td class="cam-name-cell">${c.name}</td>
                        <td>${c.event_count_24h}</td>
                        <td>${fmtAgo(c.last_event_ts)}</td>
                        <td>${zChip(c.z_score_last)}</td>
                        <td>${c.z_score_avg.toFixed(2)}</td>
                        <td>${zChip(c.z_score_max)}</td>
                        <td>${fmtBPF(c.bpf_slow_last)}</td>
                        <td>${fmtBPF(c.bpf_ewma_last)}</td>
                        <td>${c.intra_ratio_last.toFixed(3)}</td>
                    </tr>` + "`" + `;
                }).join('');
            } catch (_) {
                // non-fatal
            }
        }

        // Initial load
        loadClips();
        loadSystemStats();
        loadSignalStats();

        // Auto-refresh every 30 seconds, stay on current page
        setInterval(() => {
            const camera = document.getElementById('cameraFilter').value;
            loadClips(camera, currentPage);
            loadSystemStats();
            loadSignalStats();
        }, 30000);
    </script>
</body>
</html>
`
