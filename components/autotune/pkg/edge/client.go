// Package edge provides an HTTP client for the emd-agent REST API.
// Endpoints are documented in cvkit/edge/docs/emd-tuning-reporting-spec.md.
package edge

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"time"
)

// Client is an HTTP client for a single emd-agent instance.
type Client struct {
	baseURL    string
	httpClient *http.Client
}

// New creates an edge Client targeting baseURL (e.g. "https://edge-3.internal:8443").
func New(baseURL string) *Client {
	return &Client{
		baseURL: baseURL,
		httpClient: &http.Client{
			Timeout: 30 * time.Second,
		},
	}
}

// get performs a GET request and JSON-decodes the response body into dst.
func (c *Client) get(ctx context.Context, path string, dst any) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.baseURL+path, nil)
	if err != nil {
		return fmt.Errorf("edge get %s: build request: %w", path, err)
	}

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("edge get %s: %w", path, err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("edge get %s: unexpected status %d", path, resp.StatusCode)
	}

	if err := json.NewDecoder(resp.Body).Decode(dst); err != nil {
		return fmt.Errorf("edge get %s: decode: %w", path, err)
	}
	return nil
}

// Cameras returns the list of camera names known to the agent.
func (c *Client) Cameras(ctx context.Context) ([]string, error) {
	var out struct {
		Cameras []string `json:"cameras"`
	}
	if err := c.get(ctx, "/api/cameras", &out); err != nil {
		return nil, err
	}
	return out.Cameras, nil
}
