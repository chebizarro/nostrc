package signetclient

import (
	"context"
	"encoding/json"
	"testing"

	cascadia "git.sharegap.net/cascadia/cascadia-go"
)

type fakeRelay struct {
	published []byte
	filter    Filter
	responses chan Event
}

func (f *fakeRelay) Publish(ctx context.Context, eventJSON []byte) error {
	f.published = append([]byte(nil), eventJSON...)
	var req Request
	if err := json.Unmarshal(eventJSON, &req); err == nil {
		f.responses <- Event{Kind: KindNIP46, PubKey: "bunker-pubkey", Content: `{"id":"` + req.ID + `","result":"signed-event"}`}
	}
	return nil
}

func (f *fakeRelay) Request(ctx context.Context, filter Filter) (<-chan Event, error) {
	f.filter = filter
	return f.responses, nil
}

func TestSignEventPublishesNIP46SignEventRequest(t *testing.T) {
	relay := &fakeRelay{responses: make(chan Event, 1)}
	client, err := New("bunker://bunker-pubkey?secret=connect-secret", "client-pubkey", "client-secret", relay)
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}

	unsigned := `{"kind":1,"content":"sig acceptance","tags":[],"created_at":1720000000}`
	got, err := client.SignEvent(context.Background(), unsigned)
	if err != nil {
		t.Fatalf("SignEvent() error = %v", err)
	}
	if got != "signed-event" {
		t.Fatalf("SignEvent() result = %q, want signed-event", got)
	}

	var req Request
	if err := json.Unmarshal(relay.published, &req); err != nil {
		t.Fatalf("published request is not JSON: %v", err)
	}
	if req.Method != "sign_event" {
		t.Fatalf("published method = %q, want sign_event", req.Method)
	}
	if len(req.Params) != 1 || req.Params[0] != unsigned {
		t.Fatalf("published params = %#v, want unsigned event", req.Params)
	}
	if len(relay.filter.Kinds) != 1 || relay.filter.Kinds[0] != KindNIP46 {
		t.Fatalf("response filter kinds = %#v, want [%d]", relay.filter.Kinds, KindNIP46)
	}
	if len(relay.filter.Authors) != 1 || relay.filter.Authors[0] != "bunker-pubkey" {
		t.Fatalf("response filter authors = %#v, want bunker pubkey", relay.filter.Authors)
	}
}

func TestCascadiaKindAliasesUseGeneratedConstants(t *testing.T) {
	if KindContextVM != cascadia.CAS_INTENT || KindGiftWrap != cascadia.NIP59_GIFT_WRAP || KindCascadiaAudit != cascadia.CAS_AUDIT {
		t.Fatalf("unexpected kind aliases: contextvm=%d giftwrap=%d audit=%d", KindContextVM, KindGiftWrap, KindCascadiaAudit)
	}
}
