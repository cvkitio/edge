package edge

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"strconv"
	"time"
)

// RawEvent mirrors the JSONL event schema from the edge event log.
// Field names match the edge spec W4 JSON keys exactly.
type RawEvent struct {
	EventID         string    `json:"event_id"`
	Site            string    `json:"site,omitempty"`
	Camera          string    `json:"camera"`
	CamID           uint16    `json:"cam_id"`
	Ts              time.Time `json:"ts"`
	TsMonoNS        uint64    `json:"ts_mono_ns"`
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

// EventsOptions controls the GET /api/cameras/{name}/events request.
type EventsOptions struct {
	From  time.Time
	To    time.Time
	Limit int // 0 = server default (10 000)
}

// GetEvents fetches raw events for a camera from the edge event log.
// Returns events in chronological order.
func (c *Client) GetEvents(ctx context.Context, camera string, opts EventsOptions) ([]RawEvent, error) {
	q := url.Values{}
	if !opts.From.IsZero() {
		q.Set("from", opts.From.UTC().Format(time.RFC3339))
	}
	if !opts.To.IsZero() {
		q.Set("to", opts.To.UTC().Format(time.RFC3339))
	}
	if opts.Limit > 0 {
		q.Set("limit", strconv.Itoa(opts.Limit))
	}

	path := "/api/cameras/" + camera + "/events"
	if len(q) > 0 {
		path += "?" + q.Encode()
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.baseURL+path, nil)
	if err != nil {
		return nil, fmt.Errorf("edge events %s: build request: %w", camera, err)
	}

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("edge events %s: %w", camera, err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("edge events %s: unexpected status %d", camera, resp.StatusCode)
	}

	var events []RawEvent
	scanner := bufio.NewScanner(resp.Body)
	scanner.Buffer(make([]byte, 1024*1024), 1024*1024)
	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}
		var evt RawEvent
		if err := json.Unmarshal(line, &evt); err != nil {
			return nil, fmt.Errorf("edge events %s: decode line: %w", camera, err)
		}
		events = append(events, evt)
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("edge events %s: scan: %w", camera, err)
	}
	return events, nil
}
