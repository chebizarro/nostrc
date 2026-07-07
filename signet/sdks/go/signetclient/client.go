package signetclient

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"strings"
	"time"

	cascadia "git.sharegap.net/cascadia/cascadia-go"
)

const (
	KindNIP46     = 24133
	KindContextVM = cascadia.CAS_INTENT
	KindGiftWrap  = cascadia.NIP59_GIFT_WRAP

	// KindCascadiaAudit is the canonical CAS_AUDIT kind. cascadia-go v0.2.0
	// does not yet export CAS_AUDIT, so keep this alias local until the
	// generated binding includes it.
	KindCascadiaAudit = 4903
)

type Relay interface {
	Publish(ctx context.Context, eventJSON []byte) error
	Request(ctx context.Context, filter Filter) (<-chan Event, error)
}

type Filter struct {
	Kinds   []int               `json:"kinds,omitempty"`
	Authors []string            `json:"authors,omitempty"`
	Tags    map[string][]string `json:"#tags,omitempty"`
}

type Event struct {
	ID, PubKey, Content string
	Kind                int
	CreatedAt           int64
	Tags                [][]string
}

type Client struct {
	Relay        Relay
	BunkerPubKey string
	ClientPubKey string
	ClientSecret string
	Secret       string
	Timeout      time.Duration
}

type Request struct {
	ID     string   `json:"id"`
	Method string   `json:"method"`
	Params []string `json:"params,omitempty"`
}

type Response struct {
	ID     string `json:"id"`
	Result string `json:"result,omitempty"`
	Error  string `json:"error,omitempty"`
}

func New(bunkerURI, clientPubKey, clientSecret string, relay Relay) (*Client, error) {
	if !strings.HasPrefix(bunkerURI, "bunker://") {
		return nil, fmt.Errorf("invalid bunker URI")
	}
	u, err := url.Parse(bunkerURI)
	if err != nil {
		return nil, err
	}
	c := &Client{Relay: relay, BunkerPubKey: u.Host, ClientPubKey: clientPubKey, ClientSecret: clientSecret, Secret: u.Query().Get("secret"), Timeout: 15 * time.Second}
	if c.BunkerPubKey == "" {
		return nil, errors.New("missing bunker pubkey")
	}
	return c, nil
}

func (c *Client) Connect(ctx context.Context) error {
	_, err := c.call(ctx, "connect", c.ClientPubKey, c.Secret)
	return err
}
func (c *Client) Ping(ctx context.Context) error { _, err := c.call(ctx, "ping"); return err }
func (c *Client) GetPublicKey(ctx context.Context) (string, error) {
	return c.call(ctx, "get_public_key")
}
func (c *Client) SignEvent(ctx context.Context, unsignedEventJSON string) (string, error) {
	return c.call(ctx, "sign_event", unsignedEventJSON)
}
func (c *Client) NIP44Encrypt(ctx context.Context, peerPubKey, plaintext string) (string, error) {
	return c.call(ctx, "nip44_encrypt", peerPubKey, plaintext)
}
func (c *Client) NIP44Decrypt(ctx context.Context, peerPubKey, ciphertext string) (string, error) {
	return c.call(ctx, "nip44_decrypt", peerPubKey, ciphertext)
}

func (c *Client) call(ctx context.Context, method string, params ...string) (string, error) {
	if c.Relay == nil {
		return "", errors.New("relay transport not configured")
	}
	id := randomID()
	body, _ := json.Marshal(Request{ID: id, Method: method, Params: params})
	if err := c.Relay.Publish(ctx, body); err != nil {
		return "", err
	}
	ctx, cancel := context.WithTimeout(ctx, c.Timeout)
	defer cancel()
	ch, err := c.Relay.Request(ctx, Filter{Kinds: []int{KindNIP46}, Authors: []string{c.BunkerPubKey}})
	if err != nil {
		return "", err
	}
	for {
		select {
		case <-ctx.Done():
			return "", ctx.Err()
		case ev := <-ch:
			var r Response
			if json.Unmarshal([]byte(ev.Content), &r) == nil && r.ID == id {
				if r.Error != "" {
					return "", errors.New(r.Error)
				}
				return r.Result, nil
			}
		}
	}
}

func randomID() string { var b [8]byte; _, _ = rand.Read(b[:]); return hex.EncodeToString(b[:]) }
