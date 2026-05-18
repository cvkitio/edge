# EMD Agent REST API

Complete REST API documentation for the Edge Motion Detector Agent.

## 🚀 Quick Start

Start the agent with the API server:

```bash
./emd-agent --config config.toml --api :8080
```

**API Documentation:** http://localhost:8080/docs  
**OpenAPI Spec:** http://localhost:8080/docs/openapi.json  
**Web UI:** http://localhost:8080/

## 📚 Interactive Documentation

The agent includes **Swagger UI** for interactive API exploration at `/docs`:

![Swagger UI](https://img.shields.io/badge/Swagger-OpenAPI%203.0-green)

Features:
- ✅ Browse all endpoints and schemas
- ✅ Try API calls directly from the browser
- ✅ View example requests/responses
- ✅ Download OpenAPI 3.0 specification

## 🔌 Endpoints Overview

### Health & Monitoring

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/health` | GET | Health check (always returns 200 OK) |

### Camera Configuration

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/cameras` | GET | List all configured cameras |
| `/api/cameras/{name}/config` | GET | Get inspector configuration for a camera |
| `/api/cameras/{name}/config` | PUT | Update inspector configuration at runtime |

### Video Clips

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/clips` | GET | List all recorded clips (optional `?camera=` filter) |
| `/api/clips/{camera}/{filename}` | GET | Stream a video clip with Range support |

### Documentation & UI

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/docs` | GET | Interactive Swagger UI documentation |
| `/docs/openapi.json` | GET | OpenAPI 3.0 specification (JSON) |
| `/` | GET | Embedded web UI for browsing clips |

## 📖 Detailed Examples

### List Cameras

```bash
curl http://localhost:8080/api/cameras | jq
```

Response:
```json
{
  "cameras": ["axis_81_1", "axis_81_2", "axis_82_2"]
}
```

### Get Camera Configuration

```bash
curl http://localhost:8080/api/cameras/axis_82_2/config | jq
```

Response:
```json
{
  "motion_z_high": 3.0,
  "intra_ratio_high": 2.5,
  "on_threshold": 2,
  "off_threshold": 45,
  "bpf_floor": 100.0,
  "configured_periodic_kf": false,
  "gradual_enabled": false,
  "gradual_threshold": 0.15,
  "gradual_window_frames": 900
}
```

### Update Camera Configuration

**Reduce sensitivity (fewer false positives):**

```bash
curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config \
  -H "Content-Type: application/json" \
  -d '{
    "motion_z_high": 5.0,
    "on_threshold": 3
  }' | jq
```

**Increase sensitivity (catch more motion):**

```bash
curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config \
  -H "Content-Type: application/json" \
  -d '{
    "motion_z_high": 2.5,
    "on_threshold": 2
  }' | jq
```

Response:
```json
{
  "success": true,
  "message": "Configuration updated for camera axis_82_2"
}
```

### List Clips

**All clips:**
```bash
curl http://localhost:8080/api/clips | jq
```

**Filter by camera:**
```bash
curl http://localhost:8080/api/clips?camera=axis_82_2 | jq
```

Response:
```json
{
  "clips": [
    {
      "camera": "axis_82_2",
      "filename": "20260518_091523_motion.ts",
      "path": "/var/lib/emd-agent/clips/axis_82_2/20260518_091523_motion.ts",
      "size": 2456789,
      "mod_time": "2026-05-18T09:15:23Z",
      "url": "/api/clips/axis_82_2/20260518_091523_motion.ts"
    }
  ],
  "total": 1
}
```

### Stream a Clip

```bash
# Download clip
curl http://localhost:8080/api/clips/axis_82_2/20260518_091523_motion.ts \
  -o motion_event.ts

# Or open in browser (HTML5 video player)
open http://localhost:8080/api/clips/axis_82_2/20260518_091523_motion.ts
```

## 🔧 Configuration Parameters

### InspectorConfig Schema

| Parameter | Type | Range | Default | Description |
|-----------|------|-------|---------|-------------|
| `motion_z_high` | float | 0-100 | 3.0 | Z-score threshold (higher = less sensitive) |
| `intra_ratio_high` | float | 0-100 | 2.5 | Intra-macroblock ratio threshold |
| `on_threshold` | uint8 | 1-255 | 2 | Frames above threshold to trigger |
| `off_threshold` | uint8 | 1-255 | 45 | Frames below threshold to return to idle |
| `bpf_floor` | float | >0 | 100.0 | Minimum bytes-per-frame (prevents div/0) |
| `configured_periodic_kf` | bool | - | false | Camera sends periodic keyframes |
| `gradual_enabled` | bool | - | false | Enable gradual scene change detection |
| `gradual_threshold` | float | 0-1 | 0.15 | Gradual change threshold |
| `gradual_window_frames` | uint32 | >0 | 900 | Gradual detection window size |

### Tuning Tips

**High false positives (too sensitive):**
- ⬆️ Increase `motion_z_high` to 4.5 or 5.0
- ⬆️ Increase `on_threshold` to 3 or 4
- ⬆️ Increase `off_threshold` to 60+

**Missing motion events (not sensitive enough):**
- ⬇️ Decrease `motion_z_high` to 2.5 or 2.0
- ⬇️ Decrease `on_threshold` to 1 or 2

**Scene changes (sunrise/sunset triggering):**
- ✅ Enable `gradual_enabled: true`
- Adjust `gradual_threshold` (0.15-0.25)

## 🔒 Security

### Current Status (Development)

⚠️ **No authentication** - suitable for internal/trusted networks only

### Production Recommendations

Before deploying to production:

1. **Add Authentication**
   ```go
   // Example: API key middleware
   func authMiddleware(next http.Handler) http.Handler {
       return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
           apiKey := r.Header.Get("X-API-Key")
           if apiKey != expectedKey {
               http.Error(w, "Unauthorized", http.StatusUnauthorized)
               return
           }
           next.ServeHTTP(w, r)
       })
   }
   ```

2. **Enable HTTPS**
   ```bash
   ./emd-agent --api :8443 --tls-cert cert.pem --tls-key key.pem
   ```

3. **Add Rate Limiting**
   - Limit requests per IP/token
   - Prevent abuse and DoS

4. **Network Policies**
   - Use Kubernetes NetworkPolicies
   - Restrict API access to specific pods/namespaces

5. **Audit Logging**
   - Log all config changes
   - Include timestamp, source IP, old/new values

### Current Security Features

✅ **Path traversal protection** - blocks `..` in file paths  
✅ **Input validation** - validates parameter ranges  
✅ **File access control** - only serves files within `clip_root`  
✅ **CORS headers** - enables cross-origin requests for OpenAPI spec  

## 🧪 Testing

The API includes comprehensive test coverage:

```bash
go test ./internal/api -v
```

**Test Coverage:**
- ✅ All endpoints (health, cameras, clips, docs)
- ✅ Request validation and error handling
- ✅ Security (path traversal, file access)
- ✅ OpenAPI spec structure and completeness
- ✅ Swagger UI serving

## 📊 HTTP Status Codes

| Code | Meaning | When |
|------|---------|------|
| 200 | OK | Successful request |
| 400 | Bad Request | Invalid input, path, or JSON |
| 403 | Forbidden | Path traversal attempt |
| 404 | Not Found | Camera or clip not found |
| 405 | Method Not Allowed | Wrong HTTP method |
| 500 | Internal Server Error | Server-side failure |

## 🔌 Integration Examples

### Python

```python
import requests

# Get clips
response = requests.get('http://localhost:8080/api/clips')
clips = response.json()['clips']

# Update camera config
config = {
    'motion_z_high': 5.0,
    'on_threshold': 3
}
response = requests.put(
    'http://localhost:8080/api/cameras/axis_82_2/config',
    json=config
)
print(response.json())
```

### JavaScript

```javascript
// Fetch clips
const response = await fetch('/api/clips?camera=axis_82_2');
const data = await response.json();
console.log(`Found ${data.total} clips`);

// Update config
await fetch('/api/cameras/axis_82_2/config', {
  method: 'PUT',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({
    motion_z_high: 5.0,
    on_threshold: 3
  })
});
```

### Go

```go
type InspectorConfig struct {
    MotionZHigh float64 `json:"motion_z_high"`
    OnThreshold uint8   `json:"on_threshold"`
}

func updateCamera(baseURL, camera string, cfg InspectorConfig) error {
    data, _ := json.Marshal(cfg)
    url := fmt.Sprintf("%s/api/cameras/%s/config", baseURL, camera)
    resp, err := http.Post(url, "application/json", bytes.NewBuffer(data))
    if err != nil {
        return err
    }
    defer resp.Body.Close()
    return nil
}
```

### Kubernetes Liveness Probe

```yaml
livenessProbe:
  httpGet:
    path: /health
    port: 8080
  initialDelaySeconds: 10
  periodSeconds: 30
```

## 📈 Monitoring

The API can be monitored using:

1. **Health endpoint** - `/health` (always returns 200)
2. **Prometheus metrics** - `:9464/metrics`
3. **Logs** - structured logging for all API requests

Example monitoring setup:

```yaml
# Prometheus scrape config
- job_name: 'emd-agent'
  static_configs:
    - targets: ['localhost:9464']
```

## 🌐 CORS Configuration

The OpenAPI spec endpoint includes CORS headers:

```
Access-Control-Allow-Origin: *
```

For production, restrict origins:

```go
w.Header().Set("Access-Control-Allow-Origin", "https://your-frontend.com")
```

## 🔍 Troubleshooting

### API Not Responding

1. Check agent is running: `ps aux | grep emd-agent`
2. Verify API flag: `--api :8080`
3. Check port binding: `netstat -an | grep 8080`

### 404 Not Found

- Verify exact endpoint path
- Check spelling of camera/filename
- Use `/docs` to see all available endpoints

### Config Update Not Taking Effect

- Verify response shows `success: true`
- Check camera name is correct
- Changes apply immediately (no restart needed)

### Clips Not Listing

1. Check `clip_root` path in config
2. Verify clips exist: `ls -la /var/lib/emd-agent/clips/`
3. Check file permissions

## 🚀 Advanced Usage

### Batch Config Updates

```bash
# Update multiple cameras
for camera in camera1 camera2 camera3; do
  curl -X PUT "http://localhost:8080/api/cameras/$camera/config" \
    -H "Content-Type: application/json" \
    -d '{"motion_z_high": 5.0}'
done
```

### Monitoring Clips

```bash
# Watch for new clips
watch -n 5 'curl -s http://localhost:8080/api/clips | jq ".total"'
```

### Downloading All Clips

```bash
# Download all clips from a camera
clips=$(curl -s "http://localhost:8080/api/clips?camera=axis_82_2" | jq -r '.clips[].url')
for url in $clips; do
  curl -O "http://localhost:8080$url"
done
```

## 📝 OpenAPI Specification

The complete OpenAPI 3.0 specification is available at:

**JSON:** http://localhost:8080/docs/openapi.json

You can use this specification with:
- Code generators (OpenAPI Generator, Swagger Codegen)
- Testing tools (Postman, Insomnia)
- Documentation generators
- Contract testing frameworks

## 🎯 Next Steps

1. Start the agent: `./emd-agent --config config.toml --api :8080`
2. Open Swagger UI: http://localhost:8080/docs
3. Try the endpoints interactively
4. View example clips: http://localhost:8080/
5. Integrate with your monitoring/automation tools

## 📚 Related Documentation

- [API_CLIPS.md](API_CLIPS.md) - Detailed clips API documentation
- [API_COMPLETE.md](API_COMPLETE.md) - Runtime configuration API details
- [README.md](README.md) - Main documentation and setup guide
