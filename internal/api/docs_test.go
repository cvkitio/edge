package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// TestHandleOpenAPISpec tests the OpenAPI specification endpoint.
func TestHandleOpenAPISpec(t *testing.T) {
	handler := &Handler{
		supervisor: nil,
		clipRoot:   t.TempDir(),
	}

	tests := []struct {
		name       string
		method     string
		wantStatus int
		checkJSON  bool
	}{
		{
			name:       "GET returns spec",
			method:     http.MethodGet,
			wantStatus: http.StatusOK,
			checkJSON:  true,
		},
		{
			name:       "POST not allowed",
			method:     http.MethodPost,
			wantStatus: http.StatusMethodNotAllowed,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := httptest.NewRequest(tt.method, "/docs/openapi.json", nil)
			w := httptest.NewRecorder()

			handler.handleOpenAPISpec(w, req)

			resp := w.Result()
			if resp.StatusCode != tt.wantStatus {
				t.Errorf("expected status %d, got %d", tt.wantStatus, resp.StatusCode)
			}

			if tt.checkJSON {
				// Verify content type
				contentType := resp.Header.Get("Content-Type")
				if contentType != "application/json" {
					t.Errorf("expected Content-Type application/json, got %s", contentType)
				}

				// Verify CORS header
				corsHeader := resp.Header.Get("Access-Control-Allow-Origin")
				if corsHeader != "*" {
					t.Errorf("expected CORS header *, got %s", corsHeader)
				}

				// Verify valid JSON
				var spec map[string]interface{}
				if err := json.NewDecoder(resp.Body).Decode(&spec); err != nil {
					t.Fatalf("failed to parse OpenAPI spec as JSON: %v", err)
				}

				// Verify OpenAPI version
				if openapi, ok := spec["openapi"].(string); !ok || openapi != "3.0.0" {
					t.Errorf("expected openapi version 3.0.0, got %v", spec["openapi"])
				}

				// Verify info section
				info, ok := spec["info"].(map[string]interface{})
				if !ok {
					t.Fatal("info section missing")
				}

				if title, ok := info["title"].(string); !ok || title != "EMD Agent API" {
					t.Errorf("expected title 'EMD Agent API', got %v", info["title"])
				}

				// Verify paths section exists
				paths, ok := spec["paths"].(map[string]interface{})
				if !ok {
					t.Fatal("paths section missing")
				}

				// Verify key endpoints are documented
				expectedPaths := []string{
					"/health",
					"/api/cameras",
					"/api/cameras/{name}/config",
					"/api/clips",
					"/api/clips/{camera}/{filename}",
				}

				for _, path := range expectedPaths {
					if _, ok := paths[path]; !ok {
						t.Errorf("expected path %s to be documented", path)
					}
				}

				// Verify components/schemas section
				components, ok := spec["components"].(map[string]interface{})
				if !ok {
					t.Fatal("components section missing")
				}

				schemas, ok := components["schemas"].(map[string]interface{})
				if !ok {
					t.Fatal("schemas section missing")
				}

				expectedSchemas := []string{
					"InspectorConfig",
					"InspectorConfigUpdate",
					"ClipInfo",
					"Error",
					"Success",
				}

				for _, schema := range expectedSchemas {
					if _, ok := schemas[schema]; !ok {
						t.Errorf("expected schema %s to be defined", schema)
					}
				}
			}
		})
	}
}

// TestHandleSwaggerUI tests the Swagger UI endpoint.
func TestHandleSwaggerUI(t *testing.T) {
	handler := &Handler{
		supervisor: nil,
		clipRoot:   t.TempDir(),
	}

	tests := []struct {
		name       string
		path       string
		wantStatus int
		checkHTML  bool
	}{
		{
			name:       "GET /docs",
			path:       "/docs",
			wantStatus: http.StatusOK,
			checkHTML:  true,
		},
		{
			name:       "GET /docs/",
			path:       "/docs/",
			wantStatus: http.StatusOK,
			checkHTML:  true,
		},
		{
			name:       "other path returns 404",
			path:       "/docs/other",
			wantStatus: http.StatusNotFound,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, tt.path, nil)
			w := httptest.NewRecorder()

			handler.handleSwaggerUI(w, req)

			resp := w.Result()
			if resp.StatusCode != tt.wantStatus {
				t.Errorf("expected status %d, got %d", tt.wantStatus, resp.StatusCode)
			}

			if tt.checkHTML {
				contentType := resp.Header.Get("Content-Type")
				if contentType != "text/html; charset=utf-8" {
					t.Errorf("expected Content-Type text/html, got %s", contentType)
				}

				body := w.Body.String()
				if body == "" {
					t.Error("expected HTML content, got empty body")
				}

				// Verify Swagger UI elements
				expectedStrings := []string{
					"<!DOCTYPE html>",
					"EMD Agent API - Documentation",
					"swagger-ui",
					"/docs/openapi.json",
					"SwaggerUIBundle",
				}

				for _, expected := range expectedStrings {
					if !strings.Contains(body, expected) {
						t.Errorf("expected HTML to contain %q", expected)
					}
				}
			}
		})
	}
}

// TestOpenAPISpecStructure tests the structure of the OpenAPI spec.
func TestOpenAPISpecStructure(t *testing.T) {
	var spec map[string]interface{}
	if err := json.Unmarshal([]byte(openAPISpec), &spec); err != nil {
		t.Fatalf("OpenAPI spec is not valid JSON: %v", err)
	}

	// Test paths have correct structure
	paths, ok := spec["paths"].(map[string]interface{})
	if !ok {
		t.Fatal("paths section missing")
	}

	// Test /api/cameras/{name}/config has both GET and PUT
	cameraConfigPath, ok := paths["/api/cameras/{name}/config"].(map[string]interface{})
	if !ok {
		t.Fatal("/api/cameras/{name}/config path missing")
	}

	if _, ok := cameraConfigPath["get"]; !ok {
		t.Error("GET method missing for /api/cameras/{name}/config")
	}

	if _, ok := cameraConfigPath["put"]; !ok {
		t.Error("PUT method missing for /api/cameras/{name}/config")
	}

	// Test PUT has requestBody
	putMethod, ok := cameraConfigPath["put"].(map[string]interface{})
	if !ok {
		t.Fatal("PUT method structure invalid")
	}

	if _, ok := putMethod["requestBody"]; !ok {
		t.Error("requestBody missing for PUT /api/cameras/{name}/config")
	}

	// Test InspectorConfig schema has all required fields
	components, ok := spec["components"].(map[string]interface{})
	if !ok {
		t.Fatal("components section missing")
	}

	schemas, ok := components["schemas"].(map[string]interface{})
	if !ok {
		t.Fatal("schemas section missing")
	}

	inspectorConfig, ok := schemas["InspectorConfig"].(map[string]interface{})
	if !ok {
		t.Fatal("InspectorConfig schema missing")
	}

	properties, ok := inspectorConfig["properties"].(map[string]interface{})
	if !ok {
		t.Fatal("InspectorConfig properties missing")
	}

	expectedProperties := []string{
		"motion_z_high",
		"intra_ratio_high",
		"on_threshold",
		"off_threshold",
		"bpf_floor",
		"configured_periodic_kf",
		"gradual_enabled",
		"gradual_threshold",
		"gradual_window_frames",
	}

	for _, prop := range expectedProperties {
		if _, ok := properties[prop]; !ok {
			t.Errorf("expected property %s in InspectorConfig", prop)
		}
	}

	// Test ClipInfo schema
	clipInfo, ok := schemas["ClipInfo"].(map[string]interface{})
	if !ok {
		t.Fatal("ClipInfo schema missing")
	}

	clipProperties, ok := clipInfo["properties"].(map[string]interface{})
	if !ok {
		t.Fatal("ClipInfo properties missing")
	}

	expectedClipProps := []string{"camera", "filename", "path", "size", "mod_time", "url"}
	for _, prop := range expectedClipProps {
		if _, ok := clipProperties[prop]; !ok {
			t.Errorf("expected property %s in ClipInfo", prop)
		}
	}
}

// TestOpenAPISpecValidation tests validation constraints in the spec.
func TestOpenAPISpecValidation(t *testing.T) {
	var spec map[string]interface{}
	if err := json.Unmarshal([]byte(openAPISpec), &spec); err != nil {
		t.Fatalf("OpenAPI spec is not valid JSON: %v", err)
	}

	components := spec["components"].(map[string]interface{})
	schemas := components["schemas"].(map[string]interface{})

	// Test InspectorConfig has validation constraints
	inspectorConfig := schemas["InspectorConfig"].(map[string]interface{})
	properties := inspectorConfig["properties"].(map[string]interface{})

	// Check motion_z_high has min/max
	motionZHigh := properties["motion_z_high"].(map[string]interface{})
	if _, ok := motionZHigh["minimum"]; !ok {
		t.Error("motion_z_high should have minimum constraint")
	}
	if _, ok := motionZHigh["maximum"]; !ok {
		t.Error("motion_z_high should have maximum constraint")
	}

	// Check on_threshold has constraints
	onThreshold := properties["on_threshold"].(map[string]interface{})
	if _, ok := onThreshold["minimum"]; !ok {
		t.Error("on_threshold should have minimum constraint")
	}
	if _, ok := onThreshold["maximum"]; !ok {
		t.Error("on_threshold should have maximum constraint")
	}
}

// TestRouteRegistration tests that docs routes are registered.
func TestRouteRegistration(t *testing.T) {
	handler := &Handler{
		supervisor: nil,
		clipRoot:   t.TempDir(),
	}

	mux := http.NewServeMux()
	handler.RegisterRoutes(mux)

	tests := []struct {
		path       string
		method     string
		wantStatus int
	}{
		{"/docs", http.MethodGet, http.StatusOK},
		{"/docs/openapi.json", http.MethodGet, http.StatusOK},
	}

	for _, tt := range tests {
		t.Run(tt.path, func(t *testing.T) {
			req := httptest.NewRequest(tt.method, tt.path, nil)
			w := httptest.NewRecorder()

			mux.ServeHTTP(w, req)

			if w.Code == http.StatusNotFound {
				t.Errorf("route %s not registered", tt.path)
			}
		})
	}
}
