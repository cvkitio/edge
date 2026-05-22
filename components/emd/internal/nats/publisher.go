// Package nats provides NATS JetStream publishing for the emd-agent.
// It publishes motion events to the "EVENTS" stream on subject
// events.raw.<site>.<camera> after each clip is successfully written.
package nats

import (
	"encoding/json"
	"fmt"
	"log"
	"sync"
	"time"

	"github.com/nats-io/nats.go"
)

// Event is the raw motion event published by the edge agent.
// It mirrors the autotune schema (components/autotune/pkg/nats/schema.go)
// with the addition of TriggerOffsetMS and ZTimelineURL.
type Event struct {
	EventID         string    `json:"event_id"`
	Site            string    `json:"site"`
	Camera          string    `json:"camera"`
	CamID           uint16    `json:"cam_id"`
	Ts              time.Time `json:"ts"`
	TsMonoNS        uint64    `json:"ts_mono_ns"`
	Type            string    `json:"type"` // motion | scene_change | idr_burst
	ZScore          float64   `json:"z_score"`
	IntraRatio      float64   `json:"intra_ratio"`
	Bytes           uint64    `json:"bytes"`
	BPFSlow         float64   `json:"bpf_slow"`
	BPFEwma         float64   `json:"bpf_ewma"`
	BPFVar          float64   `json:"bpf_var"`
	SinceKF         uint32    `json:"since_kf"`
	Reason          string    `json:"reason"`
	FSMBefore       uint8     `json:"fsm_before"`
	FSMAfter        uint8     `json:"fsm_after"`
	PTSStart        uint64    `json:"pts_start"`
	PTSEnd          uint64    `json:"pts_end"`
	Codec           string    `json:"codec"`
	FPS             float64   `json:"fps"`
	ClipID          string    `json:"clip_id"`
	ClipURL         string    `json:"clip_url,omitempty"`
	ThumbsURL       string    `json:"thumbs_url,omitempty"`
	TriggerOffsetMS uint32    `json:"trigger_offset_ms"` // ms offset into clip where motion triggered
	TargetClassMask uint8     `json:"target_class_mask"`
	AgentVersion    string    `json:"agent_version"`
	// ZTimelineURL links to the per-frame z-score timeline JSON for this clip.
	// GET this URL to receive an array of {offset_ms, z_score} objects.
	// Empty if public_url is not configured.
	ZTimelineURL    string    `json:"z_timeline_url,omitempty"`
}

// Publisher wraps a NATS connection and JetStream context for event publishing.
type Publisher struct {
	conn *nats.Conn
	mu   sync.RWMutex
	js   nats.JetStreamContext // nil until connected
}

// New connects to NATS and returns a Publisher.
// With RetryOnFailedConnect the connection is attempted in the background, so
// this never blocks even when the NATS sidecar hasn't started yet.
// Returns nil, nil if url is empty (NATS is optional).
func New(url string) (*Publisher, error) {
	if url == "" {
		return nil, nil
	}

	p := &Publisher{}

	// initJetStream is called once the TCP connection is established.
	// It obtains the JetStream context and ensures the EVENTS stream exists.
	initJetStream := func(conn *nats.Conn) {
		js, err := conn.JetStream()
		if err != nil {
			log.Printf("NATS: JetStream init failed: %v", err)
			return
		}
		if _, err := js.StreamInfo("EVENTS"); err != nil {
			_, err = js.AddStream(&nats.StreamConfig{
				Name:     "EVENTS",
				Subjects: []string{"events.>"},
				Storage:  nats.FileStorage,
			})
			if err != nil {
				log.Printf("NATS: create stream EVENTS: %v", err)
				return
			}
			log.Printf("NATS: created stream EVENTS")
		}
		p.mu.Lock()
		p.js = js
		p.mu.Unlock()
		log.Printf("NATS: JetStream ready")
	}

	opts := []nats.Option{
		nats.MaxReconnects(-1),
		nats.RetryOnFailedConnect(true),
		nats.ReconnectWait(5 * time.Second),
		nats.DisconnectErrHandler(func(_ *nats.Conn, err error) {
			if err != nil {
				log.Printf("NATS: disconnected: %v", err)
			}
		}),
		nats.ConnectHandler(func(conn *nats.Conn) {
			log.Printf("NATS: connected to %s", conn.ConnectedUrl())
			initJetStream(conn)
		}),
		nats.ReconnectHandler(func(conn *nats.Conn) {
			log.Printf("NATS: reconnected to %s", conn.ConnectedUrl())
		}),
	}

	conn, err := nats.Connect(url, opts...)
	if err != nil {
		return nil, fmt.Errorf("nats connect %s: %w", url, err)
	}

	p.conn = conn
	return p, nil
}

// Publish publishes an event to events.raw.<site>.<camera> via JetStream.
// Safe to call on a nil receiver or before JetStream is ready — returns nil
// in both cases (events are silently dropped until the connection is up).
func (p *Publisher) Publish(evt Event) error {
	if p == nil {
		return nil
	}
	p.mu.RLock()
	js := p.js
	p.mu.RUnlock()
	if js == nil {
		return nil // still connecting; drop silently
	}

	payload, err := json.Marshal(evt)
	if err != nil {
		return fmt.Errorf("nats marshal event: %w", err)
	}

	subject := fmt.Sprintf("events.raw.%s.%s", evt.Site, evt.Camera)
	if _, err := js.Publish(subject, payload); err != nil {
		return fmt.Errorf("nats publish to %s: %w", subject, err)
	}

	return nil
}

// Close drains in-flight messages then closes the connection.
// Safe to call on a nil receiver.
func (p *Publisher) Close() {
	if p == nil {
		return
	}
	if err := p.conn.Drain(); err != nil {
		log.Printf("NATS: drain error: %v", err)
	}
	p.conn.Close()
}
