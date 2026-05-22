package edge

import (
	"encoding/json"
	"fmt"
	"time"

	"github.com/nats-io/nats.go"
)

// ScanFromNATS replays historical events for a site+camera from NATS JetStream
// using an ordered ephemeral consumer starting at `since`.
// Returns when no new messages arrive within idleTimeout (signals end-of-stream).
func ScanFromNATS(natsURL, site, camera string, since time.Time, idleTimeout time.Duration) ([]RawEvent, error) {
	if idleTimeout == 0 {
		idleTimeout = 500 * time.Millisecond
	}

	conn, err := nats.Connect(natsURL,
		nats.MaxReconnects(3),
		nats.Timeout(10*time.Second),
	)
	if err != nil {
		return nil, fmt.Errorf("nats connect %s: %w", natsURL, err)
	}
	defer conn.Close()

	js, err := conn.JetStream()
	if err != nil {
		return nil, fmt.Errorf("nats jetstream: %w", err)
	}

	subject := fmt.Sprintf("events.raw.%s.%s", site, camera)

	sub, err := js.SubscribeSync(subject,
		nats.OrderedConsumer(),
		nats.StartTime(since),
	)
	if err != nil {
		return nil, fmt.Errorf("nats subscribe %s: %w", subject, err)
	}
	defer sub.Unsubscribe() //nolint:errcheck

	var events []RawEvent
	for {
		msg, err := sub.NextMsg(idleTimeout)
		if err == nats.ErrTimeout {
			// No message arrived within idleTimeout — assume end-of-stream.
			break
		}
		if err != nil {
			return nil, fmt.Errorf("nats next msg: %w", err)
		}

		var evt RawEvent
		if err := json.Unmarshal(msg.Data, &evt); err != nil {
			// Skip unparseable messages rather than aborting the whole scan.
			continue
		}
		events = append(events, evt)
	}

	return events, nil
}
