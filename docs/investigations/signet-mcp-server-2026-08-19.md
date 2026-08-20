# Investigation: MCP Server for Signet (nostrc)

## Summary
Assess what is needed to implement an MCP (Model Context Protocol) server for the `signet` project inside `nostrc`, whether signet/nostrc already has the basic components required (JSON handling, transports, server loops), or whether adopting a third-party C library like https://github.com/micl2e2/mcpc is the better path.

## Symptoms / Task
- Signet is a C NIP-46 Nostr bunker daemon (signetd) with transports: NIP-46 over relays, D-Bus (unix + TCP), NIP-5L line-delimited JSON unix socket, SSH agent socket, bootstrap HTTP server.
- Question: build MCP server natively (stdio and/or streamable-HTTP JSON-RPC 2.0) vs. use mcpc.

## Hypotheses
- H1: nostrc already has JSON infra (libjson/nson) and signet has line-delimited JSON socket servers (nip5l_transport.c) and small HTTP servers (health_server.c, bootstrap_server.c) — enough to hand-roll an MCP stdio/socket server with modest effort.
- H2: mcpc provides MCP protocol plumbing (JSON-RPC framing, tool registration, lifecycle) that would save effort, but adds a dependency and may not fit signet's GLib/meson/security posture (sodium_malloc, mlock, SQLCipher).
- H3: The natural MCP tool surface maps onto the existing management protocol (kind 25910 methods) and capability engine.

## Background / Prior Research
### mcpc (github.com/micl2e2/mcpc) — assessed 2026-08-19
- Pure C (C23 target, C11 min), GNU Make (+minimal CMake), MIT. Only dep: vendored Cesanta mjson.
- Transports: stdio/`FILE*` streams (`mcpc_server_new_iostrm`) + non-standard raw TCP. **No streamable HTTP, no SSE** (marked WIP).
- Protocol: hard-codes MCP `2024-11-05`; no version/capability negotiation (TODO). Partial old-spec implementation.
- Features: tools/list+call, prompts/list+get, completion/complete; resources/list only (**no resources/read**); notification support largely unimplemented despite advertised capability flags.
- API: manual-memory callback style — create server from streams, register tools + call callbacks, `mcpc_server_start()`.
- Maturity: 30 stars, 31 commits, no releases, last commit 2025-06-24 (~14 months dormant). Experimental.
- Ecosystem: no official C SDK exists; no mature pure-C alternative found (gopher-mcp exposes a C ABI but is C++).

## Investigator Findings

### Executive conclusion

Signet already has **useful primitives, but not an MCP server implementation**. It has production-used JSON construction/parsing, a real JSON-RPC-shaped management envelope with request correlation, a newline-delimited local socket, a GLib/GIO runtime, libmicrohttpd HTTP servers, authenticated local D-Bus dispatch, and a capability engine. Those pieces make a focused **native stdio MCP adapter** practical without adding another JSON or protocol dependency. They do not supply MCP lifecycle/version negotiation, tool schemas, notification semantics, bounded stdio framing, cancellation/concurrency policy, or Streamable HTTP sessions.

The recommended first implementation is a **separate C `signet-mcp` stdio executable** that exposes a fixed agent-scoped tool table and calls signetd over its Unix system-bus API. This follows the existing `signet-git-credential` sidecar pattern: stdin/stdout belong only to the host protocol, while signetd performs UID-to-agent binding, policy, ownership, secret handling, and audit (`signet/src/git_credential_main.c:3-18,87-106,135-140`). Administrative tools should be a separately exposed profile/binary and must use the existing signed, encrypted ContextVM management path; “local process,” a command-line mode, or a caller-supplied `agent_id` is not provisioner authorization.

Do **not** adopt mcpc 0.1.0 for the first production implementation. It has the right API shape, but its current upstream source has transport and maturity problems that would require auditing or replacing much of the code Signet needs it to save. It also has no Streamable HTTP implementation, so it does not reduce the largest optional scope.

### 1. JSON infrastructure and existing JSON-RPC shapes

- **Signet is JSON-GLib-first.** Nineteen source files include either JSON-GLib or libnostr's `<json.h>`; core protocol/server files directly include and use `<json-glib/json-glib.h>` (`signet/src/nip5l_transport.c:25-30`, `signet/src/mgmt_protocol.c:34`, `signet/src/health_server.c:18-21`, `signet/src/bootstrap_server.c:28-32`). Management parsing uses `JsonParser`/`JsonObject` (`signet/src/mgmt_protocol.c:146-168`) and replies use `JsonBuilder`/`JsonGenerator` (`signet/src/mgmt_protocol.c:313-360`). This is sufficient for a native MCP envelope and JSON Schemas.
- **libjson is a Jansson-backed libnostr interop layer, not an RPC framework.** It builds `nostr_json` around Jansson (`libjson/CMakeLists.txt:4-12,51-63`) and exposes convenience getters that parse the input on every call (`libnostr/include/json.h:39-69`). For example, `nostr_json_get_string()` loads a Jansson root and copies a string (`libjson/src/json.c:1025-1038`), while `nostr_json_get_array_string()` does the same for an array element (`libjson/src/json.c:1531-1547`). NIP-5L uses those helpers for `method` and positional `params` (`signet/src/nip5l_transport.c:152-182,209-220`) while using JSON-GLib for more complex results (`signet/src/nip5l_transport.c:108-140`).
- **The advertised Meson JSON backend is not a complete abstraction.** Meson can select `json-glib` or `jansson` (`signet/meson.build:65-73`; `signet/meson.options:34-39`), but Signet sources directly use JSON-GLib types. A Jansson-only build cannot transparently compile those call sites. An MCP implementation should standardize on the already-used JSON-GLib API rather than deepen the split.
- **NSON is not relevant to MCP.** Its entire public API is `nson_marshal`/`nson_unmarshal` for a fixed `nson_Event` layout (`nson/nson.h:7-35`), and its parser indexes fixed event fields (`nson/nson.c:16-64`). No `signet/src` file includes or references NSON. It is neither a general JSON DOM nor JSON-RPC infrastructure.
- **There is already real JSON-RPC-shaped management code.** `signetctl` creates `{"jsonrpc":"2.0","id", "method", "params"}` intents (`signet/src/signetctl_main.c:145-165`) and correlates decrypted replies by both authenticated sender and matching string ID (`signet/src/signetctl_main.c:400-445`). The daemon maps 17 ContextVM method strings into normalized management operations (`signet/src/mgmt_protocol.c:611-630`), copies `method`, `params`, and `id` into its legacy request model (`signet/src/mgmt_protocol.c:632-735`), and converts legacy acknowledgements into JSON-RPC 2.0 `result` or `error` with `id` (`signet/src/mgmt_protocol.c:467-503`).
- **That management adapter is not a general MCP JSON-RPC core.** The legacy request parser reads `request_id` as a string (`signet/src/mgmt_protocol.c:166-168`) and the acknowledgement builder emits it as a string (`signet/src/mgmt_protocol.c:313-329`); failures are mostly collapsed into server error `-32000` (`signet/src/mgmt_protocol.c:486-496`). MCP still needs validation of `jsonrpc`, preservation of string/integer/null IDs, no response for notifications, standard parse/invalid-request/method/params errors, and explicit concurrency/cancellation behavior.
- **An existing Go MCP command is only scaffolding.** It scans one JSON value per line and handles `initialize`, `tools/list`, and `tools/call`, but lists six schema-less tools and returns one static message for every call (`signet/sdks/go/cmd/signet-mcp/main.go:15-46`). Its `MgmtClient` only serializes and publishes raw JSON; it does not sign/wrap kind-25910 intents or receive/decrypt/correlate acknowledgements (`signet/sdks/go/signetclient/contextvm.go:8-45`). It demonstrates intended naming, not an operational server or client.

### 2. Framing and dispatch reuse

- **NIP-5L supplies the closest stdio framing pattern.** It configures `\n` as the line terminator (`signet/src/nip5l_transport.c:543-545`), blocks in `g_io_channel_read_line()` and removes the LF (`signet/src/nip5l_transport.c:449-467`), then writes one response plus LF and flushes (`signet/src/nip5l_transport.c:469-478`). The same high-level pattern fits MCP stdio.
- **NIP-5L is not JSON-RPC 2.0.** It accepts `method` and positional `params`, but never parses an `id`; its response helpers produce only `result` or `error` (`signet/src/nip5l_transport.c:83-104,149-160`). The `NIP5L_MAX_LINE 65536` declaration is not enforced (`signet/src/nip5l_transport.c:39-41,449-480`). Its method dispatch is a long authenticated `if/else` chain, not a reusable registry (`signet/src/nip5l_transport.c:176-442`).
- **The reusable unit is operation dispatch, not the wire envelope.** Management's method-to-enum normalization and shared `signet_mgmt_handler_handle_request_ex()` path are valuable (`signet/src/mgmt_protocol.c:600-735,789-794`). For MCP, use a small fixed table containing tool name, description, input schema, required Signet capability, and callback. Do not route arbitrary MCP method names into the NIP-5L chain.
- A production stdio transport must add: a configurable hard line limit; rejection/draining of oversized records; UTF-8/JSON validation; EOF and incomplete-record handling; stderr-only logging; exact one-response-per-request and no-response-per-notification semantics; bounded output/backpressure; startup/shutdown behavior; and tests against selected MCP clients/protocol revisions.
- A separate executable is safer than embedding stdio in `signetd`: daemon logs and audit output cannot contaminate MCP stdout, host EOF cleanly terminates the adapter, and the adapter can use synchronous GDBus calls initially. If embedded later, stdin should be a GLib source or a dedicated reader that marshals work onto one owned `GMainContext`.

### 3. HTTP feasibility

- Both HTTP servers use **libmicrohttpd**, not hand-rolled sockets, libsoup, or GLib HTTP. Health includes `<microhttpd.h>` and stores an `MHD_Daemon` (`signet/src/health_server.c:18-28`); bootstrap does the same (`signet/src/bootstrap_server.c:28-39`). Meson requires `libmicrohttpd` (`signet/meson.build:34-47`) and CMake discovers it through pkg-config (`signet/CMakeLists.txt:44-53`).
- Each calls `MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG, ...)`, so each server owns MHD's internal polling thread rather than integrating with the GLib loop (`signet/src/health_server.c:341-359`; `signet/src/bootstrap_server.c:537-563`). Bootstrap already demonstrates bounded POST accumulation with an 8 KiB limit (`signet/src/bootstrap_server.c:34-45`).
- These are **HTTP primitives, not Streamable HTTP infrastructure**. Existing code has no MCP content negotiation, JSON-versus-SSE response choice, session IDs, optional GET stream, reconnection/resumption, Origin validation, MCP authentication, concurrent request correlation, cancellation, or long-lived response/backpressure design.
- The configured host is parsed but only the port is passed to MHD (`signet/src/health_server.c:341-357`; `signet/src/bootstrap_server.c:544-561`), so the apparent `127.0.0.1` health bind in `signetd_main.c:1010-1025` is not actually supplied as a bind address. A new network transport must explicitly bind loopback or a configured address.
- Bootstrap's TLS decision trusts `X-Forwarded-Proto` (`signet/src/bootstrap_server.c:61-79`) and assumes proxy termination; that is transport assurance, not authorization. Do not reuse it as MCP auth.
- A dedicated MHD endpoint is feasible, but Streamable HTTP should be a second phase after the supported MCP revision and exact POST/GET/SSE subset are fixed. It should not be bolted onto the public bootstrap server because that couples different authorization and lifecycle surfaces.

### 4. Event loop and transport lifecycle

- `signetd` runs a GLib `GMainLoop` and a 250 ms GLib timer on the main thread (`signet/src/signetd_main.c:1046-1073`).
- NIP-5L accepts through `GSocketService` on the owning GLib context, then creates one detached `GThread` per accepted connection (`signet/src/nip5l_transport.c:498-560,611-648`). Stop marks the service as stopping, shuts every channel, waits on a condition until detached workers drain, and unlinks the socket (`signet/src/nip5l_transport.c:650-674`).
- SSH agent uses a raw Unix listener with one joinable accept thread and detached client workers (`signet/src/ssh_agent.c:368-414,442-470`); stop closes the listener, joins the accept thread, shuts clients, and waits for them to drain (`signet/src/ssh_agent.c:474-501`).
- The daemon wires transports independently: bootstrap (`signet/src/signetd_main.c:855-883`), Unix/TCP D-Bus (`signet/src/signetd_main.c:885-945`), NIP-5L (`signet/src/signetd_main.c:947-975`), SSH agent (`signet/src/signetd_main.c:977-1008`), and health (`signet/src/signetd_main.c:1010-1025`), then stops them in reverse order (`signet/src/signetd_main.c:1081-1125`). An embedded transport could follow this constructor/start/stop/free pattern, but a stdio child executable avoids adding another daemon thread/context and has a clearer lifetime.

### 5. Tool surface and authorization

**Agent-scoped default tools**

A minimal useful surface maps to the existing local D-Bus interfaces (`signet/src/dbus_unix.c:30-104`):

- `signet.get_public_key`, `signet.sign_event`
- `signet.encrypt`, `signet.decrypt`
- `signet.get_token`, `signet.get_session`
- optionally `signet.list_credentials`
- optional passkey tools when built: get info, make credential, get assertion; export/import should remain disabled by default.

The capability engine maps signing, encryption, token/session, and passkey methods to explicit capability strings (`signet/include/signet/capability.h:20-148`; `signet/src/capability.c:230-265`), then enforces capability membership, signing kind restrictions, and per-agent/per-capability rate limiting (`signet/src/capability.c:120-152,183-203,267-288`). Policy lookup supports an explicit wildcard assignment but otherwise fails closed (`signet/src/capability.c:113-152`).

There is a current policy gap: D-Bus dispatch exposes `ListCredentials` (`signet/src/dbus_common.c:699-708`) but `signet_method_to_capability()` has no `ListCredentials` mapping (`signet/src/capability.c:230-265`), so it is denied whenever a policy registry is present. Do not expose that MCP tool until an explicit capability decision/mapping exists.

**Administrative tools**

The natural admin set is the 17 ContextVM operations already mapped in `signet/src/mgmt_protocol.c:611-630`: agent provision/revoke/policy/status/list/rotation/adoption/connect reissue/client list/client revoke and credential create/import/list/inspect/rotate/revoke/delete. These should be namespaced separately (for example, `signet.admin.agent_provision`) and not mixed into the default agent manifest.

**Security boundary**

- Unix D-Bus asks the bus daemon for the sender UID, resolves it server-side to `agent_id`, and rejects unresolved callers (`signet/src/dbus_unix.c:126-187`). Every resolved call then passes through daemon dispatch and policy evaluation (`signet/src/dbus_common.c:660-678`). The MCP client must not supply or override this identity.
- Stdio itself has no per-request peer authentication: the adapter inherits the launching process's OS authority. Using Unix D-Bus makes signetd authenticate the adapter's UID and enforce policy on every operation. Separate binaries/manifests reduce accidental exposure but are not authorization by themselves.
- Both D-Bus and NIP-5L condition policy enforcement on a non-NULL registry (`signet/src/dbus_common.c:670-675`; `signet/src/nip5l_transport.c:186-190`). The normal daemon passes `cap_registry` into both (`signet/src/signetd_main.c:891-899,953-964`), but a new adapter or test harness should treat a missing policy configuration as an error rather than recreate the bypass.
- Management authorization is cryptographic, not “local admin.” The inner sender must be a configured provisioner; only `agent/reissue-connect` also permits the signed target agent (`signet/include/signet/mgmt_protocol.h:3-30`). The handler verifies provisioner/target identity, requires NIP-44 decryption, applies deny/replay handling, and returns encrypted/gift-wrapped replies (`signet/src/mgmt_protocol.c:1030-1174`). Admin MCP should invoke a complete client for that path, not call the internal handler directly or accept a provisioner pubkey as a tool argument.
- Tokens, decrypted plaintext, connect secrets, and imported/exported credentials must never enter MCP logs or error data. Existing management audit deliberately records only stable redacted fields (`signet/src/mgmt_protocol.c:795-804`).

### 6. Build changes

For a native stdio executable:

1. Add a build option such as `SIGNET_ENABLE_MCP`/`signet_enable_mcp`, preferably default OFF until interoperability and security tests exist. Current CMake options live at `signet/CMakeLists.txt:10-18`; Meson options are in `signet/meson.options:5-52`.
2. Add `src/signet_mcp_main.c` plus small protocol/tool-table modules and unit tests. A D-Bus-only sidecar can depend only on GLib/GIO/JSON-GLib (and sodium only if it decodes/wipes secret buffers), avoiding linkage to all of `signet_core`. If shared Signet helpers are required, link `signet_core`.
3. CMake: add a conditional `add_executable(signet-mcp ...)`, target dependencies, and include it in `install(TARGETS ... RUNTIME DESTINATION bin)` beside current executables (`signet/CMakeLists.txt:169-186`).
4. Meson: add a conditional `executable('signet-mcp', ... install: true)` beside `signet-git-credential` (`signet/meson.build:259-295`) and register protocol tests in both test build files.
5. Release gates should cover initialization/version negotiation, `notifications/initialized`, tool schemas/list/call, string and integer IDs, notification no-reply, malformed/oversized frames, stdout purity, EOF/SIGTERM with outstanding D-Bus calls, policy denial, sensitive-output redaction, and at least two target MCP clients.

For mcpc, dependency plumbing is possible but not free: pin an audited commit, add it only to the MCP target, and use a Meson wrap/subproject or installed `mcpc.pc`; do not append it to the global `signet_deps` list consumed by every executable (`signet/meson.build:165-177,259-292`). CMake could use a vendored `add_subdirectory` or an imported/pkg-config target. Signet already has conditional dependency precedent in SQLCipher and libcbor (`signet/meson.build:37-48,173-180`; `signet/CMakeLists.txt:34-43,83-97`).

### 7. mcpc assessment (upstream master `4025fdd`, inspected 2026-08-19)

What mcpc would provide:

- The README claims server tools/resources/prompts/completion and stdio support, with HTTP still work-in-progress ([README feature table](https://github.com/micl2e2/mcpc#model-context-protocol-features)).
- Its API exposes an I/O-stream server, capability flags, tool/resource/prompt registration, callbacks, start, and close ([API reference](https://github.com/micl2e2/mcpc/blob/master/misc/api.md#server)).
- It embeds mjson and is MIT-licensed, so it would not add a separate runtime JSON package. Its pkg-config metadata reports version 0.1.0 ([`mcpc.pc`](https://github.com/micl2e2/mcpc/blob/master/src/mcpc.pc)); GitHub shows no releases or tags and the latest master commit is from 2025-06-24 ([commit history](https://github.com/micl2e2/mcpc/commits/master/), [releases](https://github.com/micl2e2/mcpc/tags)).

Why it is not the better current choice:

- HTTP is explicitly unfinished, so it offers no Streamable HTTP savings ([README](https://github.com/micl2e2/mcpc#model-context-protocol-features)).
- The initialization response hardcodes protocol `2024-11-05` rather than negotiating the client's offered version ([`serlz.c:375-385`](https://github.com/micl2e2/mcpc/blob/master/src/serlz.c#L375-L385)). Supporting an older revision is not inherently invalid, but the intended client/version matrix would need compatibility tests.
- Dispatch recognizes initialization, tools list/call, resource list, prompt list/get, and completion, but no `ping`, cancellation, or general notification handling beyond initialized; ID validation is marked TODO ([`server.c:1504-1598`](https://github.com/micl2e2/mcpc/blob/master/src/server.c#L1504-L1598)).
- Stdio input is brace-counted rather than MCP's bounded one-JSON-message-per-line contract and doubles an allocation without a maximum (`server.c:473-549`). Its quote/escape state only remembers the immediately preceding byte, which is fragile for escaped backslash runs.
- The I/O-stream implementation retrieves both input and output from connection zero (`server.c:477-485`), and response output later ignores the supplied stream and writes directly to global `stdout` with a TODO (`server.c:1604-1611`). That is a concrete integration/testability defect for Signet.
- Its CMake subproject currently contains MSVC-only flags and links `ws2_32` unconditionally ([`src/CMakeLists.txt:1-28`](https://github.com/micl2e2/mcpc/blob/master/src/CMakeLists.txt#L1-L28)); Unix/macOS builds rely on its custom Makefiles. Signet would need Meson integration plus portable CMake fixes.
- It introduces a second JSON representation/memory model beside JSON-GLib and Jansson. Tool callbacks would still need all Signet D-Bus/ContextVM authorization, conversion, secret wiping, error mapping, and tests.

### 8. Build-versus-buy estimate and recommendation

Assumptions: a fixed tool table, stdio first, Unix D-Bus for agent operations, current signed management path retained for admin, and interoperability against a named protocol revision/client set.

| Scope | Native Signet implementation | mcpc-based |
|---|---:|---:|
| Prototype agent stdio, fixed tools | 3-5 engineer-days | 2-4 days proof-of-concept |
| Production agent stdio (limits, lifecycle, schemas, packaging, security/interoperability tests) | 1.5-3 engineer-weeks | about 2-3 weeks after mcpc audit, I/O replacement/patches, build integration, and the same Signet adapters/tests |
| Provisioner/admin surface | add 1-2 weeks if the existing signed client path is completed/reused; more if identity/policy changes | same Signet-specific work; mcpc does not help authorization/relay transport |
| Limited Streamable HTTP | 3-5 weeks | no current mcpc support |
| Hardened HTTP (auth, sessions/SSE/resumption, proxy/origin and abuse controls) | 5-8 weeks | no current mcpc support |

**Recommendation:** build the narrow native C stdio adapter and reuse Signet's D-Bus and capability boundary. Treat the existing Go command as a test fixture or replace it; do not build production behavior on its incomplete management client. Defer Streamable HTTP until stdio is conformant and a concrete deployment/auth model exists. Re-evaluate mcpc only after it has a tagged release, negotiated current protocol support, fixed/bounded stdio I/O, portable CMake/Meson consumption, and passing compatibility/security tests. At present, mcpc's audit and adaptation cost is comparable to implementing the small protocol subset Signet actually needs, while native code avoids a second JSON stack and better matches the project's GLib/security lifecycle.

## Investigation Log

### Verification (orchestrator spot-checks, 2026-08-19)
Confirmed by direct read: JSON-GLib include in `signet/src/nip5l_transport.c:28`; JSON-RPC 2.0 ack converter `signet_mgmt_ack_to_jsonrpc()` in `signet/src/mgmt_protocol.c:467+`; libmicrohttpd in `signet/src/health_server.c:21`; Go stdio MCP scaffold at `signet/sdks/go/cmd/signet-mcp/main.go` (newline-scanned stdin loop); `signet-git-credential` sidecar executable at `signet/meson.build:289`.

## Root Cause / Conclusions

1. **Signet has the building blocks but not an MCP implementation.** It already has: JSON-GLib parse/build throughout the daemon; a JSON-RPC-2.0-shaped management envelope with id correlation (mgmt_protocol.c, signetctl_main.c); newline-delimited JSON socket framing (nip5l_transport.c); libmicrohttpd HTTP servers; GLib main loop + per-transport start/stop lifecycle; a capability engine with per-agent policy and rate limiting; authenticated Unix D-Bus (SO_PEERCRED UID→agent) as a ready-made security boundary; and a sidecar-executable precedent (`signet-git-credential`). A Go `signet-mcp` stdio scaffold already exists in `sdks/go` but is non-functional demo code.
2. **What's missing is only the MCP layer itself**: initialize/version negotiation, tool schemas, tools/list+tools/call dispatch table, notification semantics, bounded stdio framing, and (optionally, later) Streamable HTTP sessions.
3. **mcpc is not the better path today.** It is experimental (v0.1.0, no releases, dormant since 2025-06), pinned to the obsolete 2024-11-05 spec with no version negotiation, stdio-only (no Streamable HTTP), has concrete stdio framing/output defects (brace-counting reader, writes to global stdout ignoring the configured stream), MSVC-oriented CMake, and would introduce a third JSON stack (mjson) beside JSON-GLib and Jansson. The audit+patch cost roughly equals writing the small MCP subset natively.

## Recommendations

1. **Build native**: a small C `signet-mcp` stdio sidecar executable (GLib/GIO + JSON-GLib), following the `signet-git-credential` pattern — stdin/stdout carry only MCP JSON-RPC; all operations go to signetd over the authenticated Unix D-Bus interfaces so UID→agent binding, capability policy, and audit are enforced server-side. Fixed tool table: get_public_key, sign_event, encrypt/decrypt, get_token, get_session (defer list_credentials — capability mapping gap in capability.c).
2. **Keep admin separate**: expose the 17 ContextVM management ops (agent/provision etc.) only via a separately-gated admin profile that drives the existing signed/gift-wrapped kind-25910 path; never accept identity as a tool argument.
3. **Defer Streamable HTTP** until stdio is conformant; libmicrohttpd is available as a base but MCP session/SSE/auth semantics are all new work (3–8 weeks).
4. **Build integration**: conditional `signet-mcp` executable behind a default-OFF option in both meson.build and CMakeLists.txt; treat the Go scaffold as a test fixture or remove it.
5. **Effort**: ~3–5 days prototype, 1.5–3 weeks production-grade stdio server. Re-evaluate mcpc only if it gains releases, current-spec negotiation, fixed bounded I/O, and portable builds.
