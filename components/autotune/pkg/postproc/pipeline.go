package postproc

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"time"

	"github.com/nats-io/nats.go"

	natspkg "github.com/cvkitio/autotune/pkg/nats"
)

// Pipeline subscribes to events.raw.<site>.> on NATS JetStream and routes each
// event through an ordered chain of Stages.  Non-Pass verdicts are published to
// events.confirmed.<site>.<camera> or events.suppressed.<site>.<camera>.
type Pipeline struct {
	stages    []Stage
	js        nats.JetStreamContext
	conn      *nats.Conn
	site      string
	id        string // durable consumer name, e.g. "postproc-au01-0"
}

// Config holds the pipeline configuration.
type Config struct {
	// NATS connection settings.
	NATSURL  string
	// Site filters events to a single site (events.raw.<site).>).
	// Use "*" to process all sites.
	Site     string
	// ID is the durable consumer name; must be unique per postproc instance.
	ID       string
	// Stages are evaluated in order for each event.
	Stages   []Stage
}

// New creates a Pipeline and establishes the NATS connection.
func New(cfg Config) (*Pipeline, error) {
	conn, err := nats.Connect(cfg.NATSURL,
		nats.MaxReconnects(-1),
		nats.ReconnectWait(3*time.Second),
		nats.DisconnectErrHandler(func(_ *nats.Conn, err error) {
			if err != nil {
				slog.Warn("NATS postproc disconnected", "err", err)
			}
		}),
		nats.ReconnectHandler(func(_ *nats.Conn) {
			slog.Info("NATS postproc reconnected")
		}),
	)
	if err != nil {
		return nil, fmt.Errorf("nats connect: %w", err)
	}

	js, err := conn.JetStream()
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("nats jetstream: %w", err)
	}

	return &Pipeline{
		stages: cfg.Stages,
		js:     js,
		conn:   conn,
		site:   cfg.Site,
		id:     cfg.ID,
	}, nil
}

// Run subscribes to events.raw.<site>.> and processes events until ctx is done.
func (p *Pipeline) Run(ctx context.Context) error {
	subject := fmt.Sprintf("events.raw.%s.>", p.site)
	slog.Info("postproc subscribing", "subject", subject, "consumer", p.id)

	sub, err := p.js.Subscribe(subject,
		func(msg *nats.Msg) {
			if err := p.handle(ctx, msg); err != nil {
				slog.Error("postproc handle error", "err", err, "subject", msg.Subject)
				// Nak to allow redelivery after a short delay.
				_ = msg.NakWithDelay(5 * time.Second)
				return
			}
			_ = msg.Ack()
		},
		nats.Durable(p.id),
		nats.DeliverNew(),
		nats.AckExplicit(),
		nats.MaxAckPending(64),
	)
	if err != nil {
		return fmt.Errorf("nats subscribe %s: %w", subject, err)
	}
	defer sub.Unsubscribe() //nolint:errcheck

	<-ctx.Done()
	slog.Info("postproc draining")
	return p.conn.Drain()
}

// Close closes the NATS connection immediately (bypasses drain).
func (p *Pipeline) Close() {
	p.conn.Close()
}

// handle runs one NATS message through the stage chain and publishes the result.
func (p *Pipeline) handle(ctx context.Context, msg *nats.Msg) error {
	var evt natspkg.Event
	if err := json.Unmarshal(msg.Data, &evt); err != nil {
		// Drop unparseable messages — don't requeue forever.
		slog.Warn("postproc: unparseable event, dropping", "subject", msg.Subject)
		return nil
	}

	verdict := VerdictPass
	reason := ""
	stageName := ""

	for _, stage := range p.stages {
		v, r, err := stage.Process(ctx, &evt)
		if err != nil {
			slog.Error("postproc stage error", "stage", stage.Name(), "event_id", evt.EventID, "err", err)
			// Treat stage errors as Pass — don't block the pipeline.
			continue
		}
		if v != VerdictPass {
			verdict = v
			reason = r
			stageName = stage.Name()
			break
		}
	}

	switch verdict {
	case VerdictConfirm:
		return p.publishConfirmed(evt, stageName)
	case VerdictSuppress:
		return p.publishSuppressed(evt, stageName, reason)
	default:
		// No stage reached a decision; treat as confirmed by default.
		return p.publishConfirmed(evt, "default")
	}
}

func (p *Pipeline) publishConfirmed(evt natspkg.Event, by string) error {
	confirmed := natspkg.ConfirmedEvent{
		Event:       evt,
		ConfirmedBy: by,
	}
	payload, err := json.Marshal(confirmed)
	if err != nil {
		return fmt.Errorf("marshal confirmed: %w", err)
	}
	subject := fmt.Sprintf("events.confirmed.%s.%s", evt.Site, evt.Camera)
	if _, err := p.js.Publish(subject, payload); err != nil {
		return fmt.Errorf("publish confirmed: %w", err)
	}
	slog.Debug("postproc confirmed", "event_id", evt.EventID[:8], "camera", evt.Camera, "by", by)
	return nil
}

func (p *Pipeline) publishSuppressed(evt natspkg.Event, by, reason string) error {
	suppressed := natspkg.SuppressedEvent{
		Event:          evt,
		SuppressReason: reason,
		SuppressedBy:   by,
	}
	payload, err := json.Marshal(suppressed)
	if err != nil {
		return fmt.Errorf("marshal suppressed: %w", err)
	}
	subject := fmt.Sprintf("events.suppressed.%s.%s", evt.Site, evt.Camera)
	if _, err := p.js.Publish(subject, payload); err != nil {
		return fmt.Errorf("publish suppressed: %w", err)
	}
	slog.Info("postproc suppressed", "event_id", evt.EventID[:8], "camera", evt.Camera, "reason", reason, "by", by)
	return nil
}
