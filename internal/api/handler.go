// Package api provides HTTP endpoints for runtime configuration and monitoring.
package api

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/cvkitio/cvkit/edge/emd-agent/internal/agent"
)

// Handler provides HTTP endpoints for the agent API.
type Handler struct {
	supervisor *agent.Supervisor
	clipRoot   string
}

// NewHandler creates a new API handler.
func NewHandler(supervisor *agent.Supervisor, clipRoot string) *Handler {
	return &Handler{
		supervisor: supervisor,
		clipRoot:   clipRoot,
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

// RegisterRoutes registers all API routes on the given mux.
func (h *Handler) RegisterRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/cameras", h.handleCameras)
	mux.HandleFunc("/api/cameras/", h.handleCameraConfig)
	mux.HandleFunc("/api/clips", h.handleClipsList)
	mux.HandleFunc("/api/clips/", h.handleClipFile)
	mux.HandleFunc("/health", h.handleHealth)
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

// handleCameraConfig handles GET and PUT requests for camera configuration.
func (h *Handler) handleCameraConfig(w http.ResponseWriter, r *http.Request) {
	// Extract camera name from path: /api/cameras/{name}/config
	path := strings.TrimPrefix(r.URL.Path, "/api/cameras/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 || parts[1] != "config" {
		http.Error(w, "Not found", http.StatusNotFound)
		return
	}

	camName := parts[0]
	if camName == "" {
		http.Error(w, "Camera name required", http.StatusBadRequest)
		return
	}

	switch r.Method {
	case http.MethodGet:
		h.getInspectorConfig(w, camName)
	case http.MethodPut:
		h.updateInspectorConfig(w, r, camName)
	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
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
	Camera   string    `json:"camera"`
	Filename string    `json:"filename"`
	Path     string    `json:"path"`
	Size     int64     `json:"size"`
	ModTime  time.Time `json:"mod_time"`
	URL      string    `json:"url"`
}

// ClipsListResponse represents the list of clips.
type ClipsListResponse struct {
	Clips []ClipInfo `json:"clips"`
	Total int        `json:"total"`
}

// handleClipsList returns a list of all recorded clips.
func (h *Handler) handleClipsList(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Optional query parameter to filter by camera
	camera := r.URL.Query().Get("camera")

	clips, err := h.scanClips(camera)
	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusInternalServerError)
		json.NewEncoder(w).Encode(ErrorResponse{Error: fmt.Sprintf("Failed to scan clips: %v", err)})
		return
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(ClipsListResponse{
		Clips: clips,
		Total: len(clips),
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
			if ext == ".ts" || ext == ".mp4" || ext == ".mkv" {
				filename := filepath.Base(path)
				clips = append(clips, ClipInfo{
					Camera:   cameraName,
					Filename: filename,
					Path:     path,
					Size:     info.Size(),
					ModTime:  info.ModTime(),
					URL:      fmt.Sprintf("/api/clips/%s/%s", cameraName, filename),
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

// handleClipFile serves a specific clip file.
func (h *Handler) handleClipFile(w http.ResponseWriter, r *http.Request) {
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
	case ".ts":
		w.Header().Set("Content-Type", "video/mp2t")
	case ".mp4":
		w.Header().Set("Content-Type", "video/mp4")
	case ".mkv":
		w.Header().Set("Content-Type", "video/x-matroska")
	default:
		w.Header().Set("Content-Type", "application/octet-stream")
	}

	// Enable range requests for seeking
	w.Header().Set("Accept-Ranges", "bytes")
	w.Header().Set("Cache-Control", "public, max-age=3600")

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

// handleWebUI serves the web UI.
func (h *Handler) handleWebUI(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write([]byte(webUIHTML))
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
    </style>
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

        <div id="errorContainer"></div>

        <div class="player-section" id="playerSection">
            <div class="player-title" id="playerTitle">Now Playing</div>
            <video id="videoPlayer" controls preload="metadata"></video>
        </div>

        <div id="clipsContainer">
            <div class="loading">Loading clips...</div>
        </div>
    </div>

    <script>
        let allClips = [];
        let currentClip = null;

        async function loadClips(camera = '') {
            try {
                const url = camera ? ` + "`/api/clips?camera=${encodeURIComponent(camera)}`" + ` : '/api/clips';
                const response = await fetch(url);

                if (!response.ok) {
                    throw new Error(` + "`HTTP ${response.status}: ${response.statusText}`" + `);
                }

                const data = await response.json();
                allClips = data.clips || [];

                displayClips();
                updateStats();
                updateCameraFilter();
                clearError();
            } catch (error) {
                showError('Failed to load clips: ' + error.message);
                document.getElementById('clipsContainer').innerHTML =
                    '<div class="empty-state">Failed to load clips</div>';
            }
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

                card.innerHTML = ` + "`" + `
                    <div class="clip-camera">📹 ${clip.camera}</div>
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
            currentClip = clip;
            const player = document.getElementById('videoPlayer');
            const section = document.getElementById('playerSection');
            const title = document.getElementById('playerTitle');

            player.src = clip.url;
            player.load();
            player.play();

            title.textContent = ` + "`Now Playing: ${clip.camera} - ${clip.filename}`" + `;
            section.classList.add('active');
            section.scrollIntoView({ behavior: 'smooth', block: 'nearest' });

            displayClips(); // Refresh to show playing state
        }

        function updateStats() {
            document.getElementById('totalClips').textContent = allClips.length;

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

        function showError(message) {
            const container = document.getElementById('errorContainer');
            container.innerHTML = ` + "`<div class=\"error\">${message}</div>`" + `;
        }

        function clearError() {
            document.getElementById('errorContainer').innerHTML = '';
        }

        function refreshClips() {
            const camera = document.getElementById('cameraFilter').value;
            loadClips(camera);
        }

        // Event listeners
        document.getElementById('cameraFilter').addEventListener('change', refreshClips);

        // Initial load
        loadClips();

        // Auto-refresh every 30 seconds
        setInterval(() => {
            const camera = document.getElementById('cameraFilter').value;
            loadClips(camera);
        }, 30000);
    </script>
</body>
</html>
`
