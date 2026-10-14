# Remote application pairing (Preview)

Open **Connect an application…** in the existing window menu. G10 will mount the same sheets on the Connect page. Native GNOME applications need no pairing: their first signer call requests consent.

Choose an explicit stored identity; this never changes another application's identity or the daemon's default for older applications. Paste a `nostrconnect://` link or scan it using the camera, clipboard image, or image file. The parsed client fingerprint, unverified name, permission hints, and exact relays are previewed without contacting those relays. **Review connection** creates the daemon's shared `connect` review. Deny, Not now, and Approve retain their normal meanings. Permissions in the link are hints, never grants. New clients default to Low trust; an explicit trust choice for an unadmitted client is staged until pairing is approved.

A `bunker://` link from another signer is not an identity import or a delegation of custody. Grotto explains this instead of silently connecting as a client of that signer.

## Creating a link and configuring relays

**Create a pairing link / Relay settings** opens the explicit identity chooser and daemon-backed relay settings. Apply a complete relay configuration to enable or disable remote signing without restarting the daemon. There is no public-relay fallback. `wss://` is required except `ws://` on loopback. Onion/Tor/Orbot is not supported; connections expose the machine's network address and timing to the confirmed relays.

If the transport credential is missing, use **Set up / Reset remote signing endpoint…** and confirm the warning. It creates a separate daemon-owned credential and revokes all remote clients and their permissions. It does not change identity keys. Missing credentials are never regenerated silently.

A new `bunker://` offer is shown only in LISTENING, with explicit signer-wide relays. It contains 32 random URL-safe token bytes, expires after five minutes, and closes/cancels with its sheet. At most four offers are outstanding. A token reserves one authenticated client and the selected identity; it is consumed only after approval. Consumption or a terminal outcome wipes its secret and releases its capacity slot. Denial and loss of LISTENING invalidate the offer; the QR sheet closes when the listener is no longer usable. Sharing the link only requests a review, not signing authority. Copy is explicit; closing clears the clipboard only if Grotto still owns that exact content provider. Do not put pairing links into logs or configuration files.

Enable remote signing explicitly in **Relay settings** before accepting a `nostrconnect://` link. While disabled, AcceptConnectURI returns a typed NotReady error and does not create a review, change `enabled`, or reconnect any client's relays. Pairing approval never changes this global kill switch. An enabled listener uses the approved client's relays only for that client's acknowledgement; ordinary replies go to the relay that delivered their authenticated request, never the union of listened relays. Approved client metadata first persists as `paired=false`, `pairing-pending-ack=true`. Only relay `OK true` for its scoped acknowledgement changes that record to paired. Publish rejection, timeout, shutdown or an unusable relay leaves an explicitly incomplete pending record, not signing admission. Restart preserves that distinction; complete a fresh pairing review to retry an incomplete exchange. The signer sends an encrypted **connect response whose result is the supplied client secret**, not a signer-initiated connect request. This follows [NIP-46's client-initiated connection contract](https://github.com/nostr-protocol/nips/blob/master/46.md#direct-connection-initiated-by-the-client).

## Durable configuration and headless use

The daemon owns `$XDG_CONFIG_HOME/grotto/bunker.ini` (v1). The first load may read legacy GSettings if this file does not exist. Once present, this file is authoritative, including an explicit disabled state. ConfigureBunker validates a full candidate, writes it atomically with mode 0600, then applies relay differences. Runtime status and desired configuration can diverge on credential/network failure; use the runtime state, not `enabled`, as readiness evidence. If a live relay diff fails, the listener stops instead of continuing to contact relays removed from the durable configuration.

```ini
[meta]
version=1

[bunker]
enabled=true
relays=wss://relay.example.org/;
allowed-methods=get_public_key;sign_event;nip04_encrypt;nip04_decrypt;nip44_encrypt;nip44_decrypt;ping;get_relays;
```

The URL above is a placeholder, not a default. Use relays you have explicitly selected. `clients.ini` v1 stores canonical `nip46:<client-hex>` principals, selected identities, pairing admission, trust tier, and each client's approved `relays` list. The additive `pairing-pending-ack` field distinguishes explicit consent from completed pairing; old v1 records default it to false. A pending record also carries a non-secret `pairing-pending-id` digest so an older exchange's acknowledgement cannot finish a newer exchange, even for the same client and identity. Its record sections are the SHA-256 of the principal; `[clients] version=1` identifies the schema. Provision only actual owner-approved clients while the daemon is stopped, preserving the existing transport credential and scoped permissions. Tokens and pause state are never persisted. Higher/invalid schema versions fail closed. The legacy `bunker-allowed-pubkeys` list admits a client only to manual connect review; it does not establish pairing or any signing permission.

Client relays are resubscribed after restart and counted in readiness separately from signer-wide desired relays. `get_relays` and newly created bunker offers advertise the signer-wide list, not another client's private relay list. Reconfiguration preserves unchanged sockets and subscriptions; removed delivery relays cancel their pending continuations. Disable, pause, revoke, endpoint reset, pairing cancellation, and shutdown fence pending pairing work.

For headless operation, use `grotto-daemon --bunker --no-approval-ui` with pre-approved persisted clients and scoped permissions. Anything requiring new consent fails closed. There is no unattended token-admission or allow-all mode.

## systemd network policy: operator check

The base user unit intentionally restricts traffic to localhost. Install the supplied `grotto-daemon.service.d/bunker.conf.example` as `~/.config/systemd/user/grotto-daemon.service.d/bunker.conf`; inspect its `ExecStart=` reset and `IPAddressDeny=` reset. For headless use, replace the executable line with `ExecStart=/usr/bin/grotto-daemon --bunker --no-approval-ui` (or use the separate headless example). This network permission covers both signer-wide and client-supplied relays.

On a disposable native Linux session, with a disposable signer profile and credential:

1. Inspect `systemctl --user cat grotto-daemon.service`; confirm the base `IPAddressDeny=any`, no `ExecReload`, and one final ExecStart.
2. Install the chosen drop-in; run `systemctl --user daemon-reload` and restart the unit once to apply systemd policy. This installation restart is not needed for subsequent ConfigureBunker changes.
3. Inspect `systemctl --user show grotto-daemon.service -p ExecStart -p IPAddressDeny -p MainPID` and record the PID.
4. Pair through a selected remote relay and wait for EOSE-backed LISTENING. Change/add/remove relays through the GUI and confirm the PID stays unchanged, unchanged subscriptions remain usable, outage reports OFFLINE/DEGRADED, and reconnect returns to usable state.
5. Run the headless variant with a pre-approved client; verify a scoped operation works and a new client's pairing fails promptly without starting the approval UI.
6. Remove the disposable drop-in/profile/credential through the supported management flow. Never alter a developer's default/search-list keychain configuration.

This is the native operator acceptance procedure, not a claim that a macOS build or container has exercised a running systemd user service. The GNOME/LTS camera and installed-unit matrix remains part of G12 promotion evidence. `signer/qr-scanner` exercises the actual appsink/zbar path with a fixture image and bus-delivered missing/denied/busy failures, not physical camera permission grants.
