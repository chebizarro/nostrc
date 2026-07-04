package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
)

type req struct {
	ID     any             `json:"id"`
	Method string          `json:"method"`
	Params json.RawMessage `json:"params"`
}

func main() {
	s := bufio.NewScanner(os.Stdin)
	for s.Scan() {
		var r req
		if json.Unmarshal(s.Bytes(), &r) != nil {
			continue
		}
		handle(r)
	}
}

func handle(r req) {
	switch r.Method {
	case "initialize":
		reply(r.ID, map[string]any{"protocolVersion": "2024-11-05", "serverInfo": map[string]any{"name": "signet-mcp", "version": "0.1.0"}})
	case "tools/list":
		reply(r.ID, map[string]any{"tools": []map[string]any{{"name": "signet.provision"}, {"name": "signet.revoke"}, {"name": "signet.rotate"}, {"name": "signet.set_policy"}, {"name": "signet.status"}, {"name": "signet.list"}}})
	case "tools/call":
		reply(r.ID, map[string]any{"content": []map[string]string{{"type": "text", "text": "Signet MCP wrapper configured; host integration supplies the 25910 Relay transport."}}})
	default:
		replyErr(r.ID, -32601, "method not found")
	}
}

func reply(id any, result any) {
	b, _ := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": id, "result": result})
	fmt.Println(string(b))
}
func replyErr(id any, code int, msg string) {
	b, _ := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": id, "error": map[string]any{"code": code, "message": msg}})
	fmt.Println(string(b))
}
