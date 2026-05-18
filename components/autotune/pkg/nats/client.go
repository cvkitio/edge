package nats

import (
	"fmt"
	"log/slog"
	"time"

	"github.com/nats-io/nats.go"
)

// ClientConfig holds NATS connection configuration.
type ClientConfig struct {
	URL       string
	CredsFile string // NATS JWT credentials file (optional)
	NKeyFile  string // NKey seed file (optional)
}

// Client wraps a NATS connection with reconnect semantics.
type Client struct {
	conn *nats.Conn
	js   nats.JetStreamContext
}

// Connect establishes a NATS connection with automatic reconnect.
func Connect(cfg ClientConfig) (*Client, error) {
	opts := []nats.Option{
		nats.MaxReconnects(-1),
		nats.ReconnectWait(2 * time.Second),
		nats.PingInterval(20 * time.Second),
		nats.DisconnectErrHandler(func(_ *nats.Conn, err error) {
			if err != nil {
				slog.Warn("NATS disconnected", "err", err)
			}
		}),
		nats.ReconnectHandler(func(_ *nats.Conn) {
			slog.Info("NATS reconnected")
		}),
	}

	if cfg.CredsFile != "" {
		opts = append(opts, nats.UserCredentials(cfg.CredsFile))
	} else if cfg.NKeyFile != "" {
		opt, err := nats.NkeyOptionFromSeed(cfg.NKeyFile)
		if err != nil {
			return nil, fmt.Errorf("nats nkey: %w", err)
		}
		opts = append(opts, opt)
	}

	conn, err := nats.Connect(cfg.URL, opts...)
	if err != nil {
		return nil, fmt.Errorf("nats connect %s: %w", cfg.URL, err)
	}

	js, err := conn.JetStream()
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("nats jetstream: %w", err)
	}

	slog.Info("NATS connected", "url", cfg.URL)
	return &Client{conn: conn, js: js}, nil
}

// Conn returns the underlying *nats.Conn.
func (c *Client) Conn() *nats.Conn { return c.conn }

// JS returns the JetStream context.
func (c *Client) JS() nats.JetStreamContext { return c.js }

// Drain drains in-flight messages and closes the connection (best-effort, 2s).
func (c *Client) Drain() {
	if err := c.conn.Drain(); err != nil {
		slog.Warn("NATS drain error", "err", err)
	}
}

// Close closes the connection immediately.
func (c *Client) Close() {
	c.conn.Close()
}
