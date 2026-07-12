package signetclient

import (
	"context"
	"encoding/json"
	"errors"
)

type MgmtClient struct {
	Relay             Relay
	BunkerPubKey      string
	ProvisionerPubKey string
}
type JSONRPCRequest struct {
	JSONRPC string         `json:"jsonrpc"`
	ID      string         `json:"id"`
	Method  string         `json:"method"`
	Params  map[string]any `json:"params,omitempty"`
}

func (m *MgmtClient) Provision(ctx context.Context, agent string, deliver bool, bootstrapPubKey string) error {
	return m.call(ctx, "agent/provision", map[string]any{"agent_id": agent, "deliver": deliver, "bootstrap_pubkey": bootstrapPubKey})
}
func (m *MgmtClient) Revoke(ctx context.Context, agent string) error {
	return m.call(ctx, "agent/revoke", map[string]any{"agent_id": agent})
}
func (m *MgmtClient) Rotate(ctx context.Context, agent string) error {
	return m.call(ctx, "agent/rotate-key", map[string]any{"agent_id": agent})
}
func (m *MgmtClient) SetPolicy(ctx context.Context, agent string, policy map[string]any) error {
	return m.call(ctx, "agent/set-policy", map[string]any{"agent_id": agent, "policy": policy})
}
func (m *MgmtClient) ReissueConnect(ctx context.Context, agent string) error {
	return m.call(ctx, "agent/reissue-connect", map[string]any{"agent_id": agent})
}
func (m *MgmtClient) Status(ctx context.Context) error { return m.call(ctx, "agent/get-status", nil) }
func (m *MgmtClient) List(ctx context.Context) error   { return m.call(ctx, "agent/list", nil) }

func (m *MgmtClient) call(ctx context.Context, method string, params map[string]any) error {
	if m.Relay == nil {
		return errors.New("relay transport not configured")
	}
	b, _ := json.Marshal(JSONRPCRequest{JSONRPC: "2.0", ID: randomID(), Method: method, Params: params})
	return m.Relay.Publish(ctx, b)
}
