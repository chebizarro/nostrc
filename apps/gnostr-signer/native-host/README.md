# nostr-signer-webext-host

Native-messaging host (`org.nostr.signer_bridge`) for the NIP-07 + WebLN
browser extension in `browser-extension/nip07/` (bead `nostrc-jjyp`). It
forwards `window.nostr` calls to the desktop signer `org.nostr.Signer` on the
session bus, with the page origin as `app_id`, and `window.webln` calls to
the wallet agent `org.nostr.Wallet1` through its origin-taking `*For`
methods, so budgets and approvals are per site. It holds no key material
and no wallet secret.

Install steps, security model, wire protocol and error codes are in
`browser-extension/nip07/README.md` in the source tree. This file is shipped
as `/usr/share/doc/nostr-signer-webext-host/README.md`.

## Layout

| file | role |
|------|------|
| `native_messaging.[ch]` | 4-byte LE length + JSON framing on explicit fds |
| `nm_policy.[ch]` | origin → app_id, unsigned-event validation/canonicalization, npub → hex, getRelays shape, NUL guard |
| `nm_errors.[ch]` | error vocabulary + `org.nostr.Signer` / bus error mapping |
| `nm_router.[ch]` | request parsing, id/in-flight limits, provider dispatch, reply envelopes |
| `nm_provider_nip07.c` | NIP-07 → `org.nostr.Signer` (async D-Bus) |
| `nm_provider_webln.c` | WebLN → `org.nostr.Wallet1` `GetInfoFor`/`GetBalanceFor`/`MakeInvoiceFor`/`PayInvoiceFor` (async D-Bus), `webln.status` probe |
| `nm_webln.[ch]` | sats ↔ msat + range checks, `makeInvoice` argument resolution, BOLT-11 shape, GetInfo → WebLN shape, `Wallet1` error mapping |
| `main_native_host.c` | stdin reader thread, main loop, stdout isolation |
| `manifests/*/org.nostr.signer_bridge.json.in` | host manifests (configured with the libexec path + pinned extension ids) |
| `install.sh` | per-user manifest installer (Firefox, Flatpak Firefox, Chromium, Chrome) |

## Build and test

```sh
cmake -S apps/gnostr-signer/native-host -B build-nmh -G Ninja -DBUILD_TESTING=ON
cmake --build build-nmh && ctest --test-dir build-nmh
```

The end-to-end tests are only built from the top-level tree:

* `test_nmh_e2e` (needs the `nostr-signer-daemon` target,
  `-DENABLE_NOSTR_SIGNER_WEBEXT_HOST=ON -DBUILD_TESTING=ON`) runs the real
  signer daemon on a private `GTestDBus` bus with a throw-away key and drives
  the real host binary through pipes.
* `test_nmh_webln_e2e` (also `-DENABLE_NOSTR_WALLET_AGENT=ON`) runs the real
  `nostr-wallet-agent` headless with an in-memory pairing store on a private
  bus, trusting the build-tree host through the agent's test-build-only
  `NOSTR_WALLET_AGENT_ORIGIN_BRIDGES`, and checks: `webln.status`, the
  origin assertion accepted for the host (and the page origin as the agent's
  principal, from its log), refused (`Denied`) for any other caller, error
  mapping, host-side validation, and `wallet_unavailable` once the agent is
  gone. No wallet is paired (pairing always needs the agent's dialog).

## WebLN

| method | origin | → `org.nostr.Wallet1` | result |
|---|---|---|---|
| `webln.status` | not needed | `Introspect` (has `*For`?) + `Properties.Get(Paired)` — never prompts | `{available, paired[, reason]}` |
| `webln.enable` | required | same probe | `{enabled: true}` / `not_paired` / `wallet_unavailable` / `unsupported` |
| `webln.getInfo` | required | `GetInfoFor(origin)` | `{node, methods, supports, version}`; unpaired → `not_paired` |
| `webln.getBalance` | required | `GetBalanceFor(origin)` | `{balance: sats, currency: "sats"}` |
| `webln.makeInvoice` | required | `MakeInvoiceFor(origin, sats×1000, defaultMemo, 0)` | `{paymentRequest, rHash}` |
| `webln.sendPayment` | required | `PayInvoiceFor(origin, paymentRequest, 0)` | `{preimage}` |
| `webln.keysend` / `signMessage` / `verifyMessage` / `lnurl` | required | — | `unsupported` |

The agent accepts the `*For` origin only from this binary as installed
(`<libexecdir>/nostr-signer-webext-host`, compared by path and inode of
`/proc/<pid>/exe`), so the host must be the one process holding its bus
connection: it exits on stdin EOF and never forks or execs. An agent
without the `*For` methods is reported as unavailable, so the extension does
not offer WebLN rather than charging every site to the browser's budget.
Amounts over 4 294 967 sats (the agent's `uint32` msat) are `too_large`.
Wallet error mapping: `NotPaired` → `not_paired`, `Denied` → `rejected`,
`BudgetExceeded` → `budget_exceeded`, `WalletError` → `wallet_error`
(`[NOT_IMPLEMENTED]` → `unsupported`), `RelayError`/no agent →
`wallet_unavailable`, `Timeout` → `timeout` (payments: outcome unknown).

Manual smoke test against a running wallet agent (no wallet paired →
`available: true, paired: false`, then `not_paired`):

```sh
for f in '{"id":"1","method":"webln.status"}' \
         '{"id":"2","method":"webln.enable","origin":"https://shop.example"}' \
         '{"id":"3","method":"webln.makeInvoice","origin":"https://shop.example","params":{"amount":21}}'; do
  printf '%s' "$f" | python3 -c 'import sys,struct;d=sys.stdin.buffer.read();sys.stdout.buffer.write(struct.pack("<I",len(d))+d)'
done | /usr/libexec/nostr-signer-webext-host |
  python3 -c 'import sys,struct
b=sys.stdin.buffer
while (h:=b.read(4)): print(b.read(struct.unpack("<I",h)[0]).decode())'
```

Manual smoke test against a running signer:

```sh
printf '{"id":"1","method":"getPublicKey","origin":"https://snort.social"}' |
  python3 -c 'import sys,struct;d=sys.stdin.buffer.read();sys.stdout.buffer.write(struct.pack("<I",len(d))+d)' |
  /usr/libexec/nostr-signer-webext-host | tail -c +5; echo
```

Environment: `NOSTR_SIGNER_BRIDGE_IDENTITY` (signer identity selector),
`NOSTR_SIGNER_BRIDGE_CALL_TIMEOUT_MS` (30000),
`NOSTR_SIGNER_BRIDGE_APPROVAL_TIMEOUT_MS` (120000),
`NOSTR_SIGNER_BRIDGE_WALLET_TIMEOUT_MS` (190000, WebLN wallet calls),
`NOSTR_SIGNER_BRIDGE_DEBUG=1`.
