# NIP-46 greeter acceptance — real GDM, live-relay QR + pre-paired bunker

- Author: Claude Opus 4.7 (agent)
- Date: 2026-09-22
- Beads: `nostrc-zcll.7` (real-GDM login acceptance), `nostrc-z1fb` (QR
  provider), `nostrc-ot2c.7` (real NIP-46 signer proof)
- Source: worktree branch `test/nip46-greeter-acceptance`, built from
  master merge `1cd7b312` (Phases 2–4 of the QR provider, PAM, broker,
  greeter artifact).
- Hypervisor: `majordomo@192.168.40.15` (`sudo virsh screenshot` /
  `virsh send-key`)
- Guest: `ssh gnome-dev` — Ubuntu 24.04.5 LTS, GNOME Shell 46, GDM active
  on tty1, primary display 1280×800, `debugger` user (uid 1000), `gdm`
  user uid 124. Snapshot of pre-change PAM/NSS/dconf at
  `/root/nip46-accept-snapshot-20260922/` on the guest.
- Live relays touched: `wss://bunker.sharegap.net` (design D4 default,
  Cascadia Genesis Private Relay, NIP-42-gated per NIP-11 — no advertised
  NIP-46 support), `wss://relay.damus.io` (public relay used as a fall-back
  for the phone→broker leg after the Cascadia relay showed no NIP-46
  fanout to the broker's subscriber). No relays received account
  passphrases or bunker secrets.
- Evidence directory on the workstation: `/tmp/nip46-accept-evidence/`
  (screenshots + guest-side logs synced from `/tmp/nip46-accept/`).

## 1. Verdict

**Partial acceptance — greeter half proven, phone→broker half blocked
by an inbound-event dispatch stall in the broker's persistent NIP-46
client pool.**

- ✅ Path A (QR): the real GDM greeter, PAM stack, broker and greeter
  extension successfully render the LIVE `nostrconnect://` URI at the
  greeter; `zbarimg` decodes the on-screen QR to the exact URI the
  broker published; the greeter artifact
  (`/run/nostr-auth/greeter/{current.json,current.png}`) is written by
  the broker on `SELECT_PROVIDER` and retired on transaction close; PAM
  invokes the QR provider with `provider=nip46qr`; the headless stand-in
  signer parses the URI, dials the URI's relay(s), and publishes a valid
  signed NIP-46 `connect` event carrying the URI's `secret=` token (the
  relay responds `["OK",<id>,true,""]`).
- ❌ Path A: the broker's persistent NIP-46 client pool never
  dispatches inbound events to its middleware
  (`nip46_persistent_client_cb`), so `await_connect` times out after
  `nip46_qr_wait_ms` (78–90 s) even when the signer's connect event
  landed on the relay. PAM sees `authenticate <user> (nip46qr) ->
  invalid_proof` (broker's current collapse of
  `INTERACTION_REQUIRED` → `NH_AUTH_RESULT_INVALID_PROOF`, noted in
  design §4.1 as a broker-level policy imperfection).
- ⏸ Path B (pre-paired bunker): **NOT run.** The maintainer's
  single-use re-pairing bundle at
  `test-agent-20260921.reissue-20260922T100040Z.login`
  (agent pubkey `e2a4…7240`, one-time `bunker_uri.secret`) was not
  consumed because the broker-side receive stall above (§5) affects the
  same persistent-pool code path used by `provider_nip46`
  (`nostr_nip46_client_start` → `nip46_publish_response` → RPC response
  dispatch via the middleware). Attempting Path B would have burned the
  one-time secret against an environment that cannot receive the reply.
  A copy of the bundle was scp'd to the guest at `/tmp/nip46-accept/`
  under mode 0600, never consumed, and `shred -u`'d at teardown.

## 2. Fixes landed in this branch (in scope: `gnome/nostr-homed`, `nips/nip46`)

Three targeted, minimal changes needed to reach the state above.

### 2.1 `gnome/nostr-homed/systemd/nostr-authd.service.in`

Extend `RestrictAddressFamilies` from the shipped `AF_UNIX`-only to
`AF_UNIX AF_INET AF_INET6`. Without this, the broker's libwebsockets
client cannot open the TCP/TLS socket to the signer relay: it silently
fails to establish a WebSocket connection, and every `client_start`
returns success from
`nostr_relay_is_connected(relay)`-based signalling while never
actually receiving traffic — the same visible symptom as the §5
persistent-pool receive stall, but with a distinct root cause. In-code
comment cites this review.

Evidence before the fix (broker journal from run `ok2`):

    [nostr_subscription_fire] sending: ["REQ","10",{"kinds":[24133],
      "since":1790077683,"#p":["ba21d52a…"]}]
    [nip46] client_start: persistent pool started with 1 relay(s)
    (78 s of silence — no envelope parses, no CONNECTED transition)
    [nip46] await_connect: timed out after 78000 ms

After the fix (broker journal from run `ok3`, same relay + same wire
frame from the stand-in):

    [nip46] relay_state: wss://relay.damus.io CONNECTED
    [pool] redial connected: wss://relay.damus.io
    [nostr_subscription_fire] sending: ["REQ","1",{…"#p":["cd4a7220…"]}]
    [nip46] client_start: persistent pool started with 1 relay(s)

The `CONNECTED` transition line is the change: before the fix, the pool
never emitted a state callback because the underlying `socket(AF_INET,
…)` was denied by systemd. `await_connect` still times out (§5).

### 2.2 `gnome/nostr-homed/tests/integration/seed_nip46_authority.c` (new)

`nh-seed-authority` only enrols the local-vault provider; the
acceptance run needs an account whose ONLY enabled provider is
`nip46qr` (or `nip46`), so the PAM module silently selects it without
prompting. New sibling seeder `nh-seed-nip46-authority` takes:

    nh-seed-nip46-authority <dir> <username> <pubkey_hex> \
        --provider=nip46qr [--relay=wss://…] [--name=<display>]
    nh-seed-nip46-authority <dir> <username> <pubkey_hex> \
        --provider=nip46 --bunker-uri=<bunker://…> \
        --client-sk-file=<path> [--home-root=<path>]

Same enrolment flow as `test_broker_login_nip46_qr.c::seed()`, wrapped
in a CLI. Built alongside `nh-seed-authority` when
`NOSTR_HOMED_ENABLE_AUTH_RUNTIME=ON`; not installed (test helper
only). Wired into CMake at
`gnome/nostr-homed/CMakeLists.txt`.

### 2.3 `gnome/nostr-homed/tests/integration/qr_signer_standin.c` (new)

Headless "phone signer" that consumes a `nostrconnect://` URI, sets its
identity to the supplied 32-byte hex private key, and calls
`nostr_nip46_bunker_connect_to_client` twice with a delay so the pool's
WebSocket has time to establish before publish (the library primitive
is fire-and-forget: an unconnected relay drops the connect event
silently — first attempt seeds the pool, second attempt after the WS
lands publishes for real). Then keeps the bunker session alive for
`--wait-sec` seconds so subsequent `get_public_key` / `sign_event`
RPCs would be answered by the default handler
(sign-with-session-secret). Design §7.1 stand-in.

## 3. Path A evidence — greeter side, LIVE URI

The design's proof-of-render loop:

    broker publishes /run/nostr-auth/greeter/current.{json,png} ──▶
      gnome-shell extension nostr-login-qr@nostrc reads them ──▶
        renders a real PNG in a floating panel on the greeter ──▶
          zbarimg from the framebuffer must decode BACK to the URI.

All four steps confirmed live on `gnome-dev` at 2026-09-22 12:06 UTC.

### 3.1 Greeter shows the LIVE QR from a real login transaction

Screenshot (`/tmp/nip46-accept-evidence/greeter-qr2.png`, captured via
`virsh screenshot gnome-dev`) shows:

- Pairing code `B796-04C5`.
- Hint `Scan this with your Nostr signer app`.
- QR image approximately 320×320, top-centre of the greeter, above the
  user list.
- The greeter's stock user card (`GNOME Debug Operator` + `Not
  listed?`) remains present underneath.

### 3.2 `zbarimg` decodes the framebuffer QR → EXACTLY the URI the broker published

    $ zbarimg --raw -q /tmp/nip46-accept/greeter-qr2.png
    nostrconnect://b79604c5918eaf0de997c52578c5d486a098107b572c7af3f23e5ca67e43dba3?relay=wss%3A%2F%2Frelay.damus.io&secret=9c5c8eb0bb5c2c43cfd00d49b408066f&perms=sign_event%3A1&name=GNOME%20%28QR%29

Byte-identical to `/tmp/nip46-accept/uri-shot.txt`, which is the URI
extracted from `/run/nostr-auth/greeter/current.json` during the same
live transaction. The pairing code shown in the greeter (`B796-04C5`)
matches the first 8 hex characters of the ephemeral client pubkey
(`b79604c5…`), upper-cased and hyphenated, per design §3.4.

The stand-in's URI parse produced the same output:

    uri=nostrconnect://b79604c5…?relay=wss%3A%2F%2Frelay.damus.io&secret=<REDACTED>&perms=sign_event%3A1&name=GNOME%20%28QR%29
    client_pubkey=b79604c5918eaf0de997c52578c5d486a098107b572c7af3f23e5ca67e43dba3
    relays=1
      relay[0]=wss://relay.damus.io
    perms=sign_event:1
    name=GNOME (QR)

### 3.3 Broker publish/retire lifecycle

Publish (strace of the broker child during the wait):

    openat(AT_FDCWD, "/run/nostr-auth/greeter/current.json.tmp", …) = 15
    rename("/run/nostr-auth/greeter/current.json.tmp", "…/current.json") = 0
    openat(AT_FDCWD, "/run/nostr-auth/greeter/current.png.tmp",  …) = 15
    rename("/run/nostr-auth/greeter/current.png.tmp",  "…/current.png")  = 0

Retire (same strace, at `submit_unlock` timeout / connection close):

    unlink("/run/nostr-auth/greeter/current.json") = 0
    unlink("/run/nostr-auth/greeter/current.png")  = 0

Post-tx greeter screenshot
(`/tmp/nip46-accept-evidence/greeter-post2.png`) shows the QR panel
gone; the login dialog is fully functional again. The greeter dir on
disk is empty after teardown.

### 3.4 pamtester at the real `gdm-password` stack does route to the QR provider

Same wire as GDM (`/etc/pam.d/gdm-password` `@include`s
`common-auth`, which the shipped `nostr` pam-configs profile installs
`pam_nostr.so` into with `default=die` strict-deny):

    pamtester -v gdm-password n_qralice authenticate
    pamtester: invoking pam_start(gdm-password, n_qralice, ...)
    pamtester: performing operation - authenticate
    pamtester: Authentication failure
    Scan this with your Nostr signer app
    Pairing code: B796-04C5
    Waiting for your signer…

`auth.log`:

    pam_nostr(gdm-password:auth): nostr: authenticate n_qralice (nip46qr) -> invalid_proof

The PAM module correctly emits the design's short-form graphical-greeter
message (URI intentionally NOT emitted on stdout because
`is_text_console()` returns 0 for `gdm-password`; the URI lives in
`current.json` for the extension to render).

## 4. Path A evidence — phone side, LIVE signer against the LIVE relay

`qr_signer_standin --uri … --nsec-file <alice.sk> --wait-sec 45`
consumes the URI extracted from `current.json` and drives
`nostr_nip46_bunker_connect_to_client`. Journal excerpt (from
`/tmp/nip46-accept-evidence/guest/standin-shot.out`):

    [nostr_subscription_fire] sending: ["REQ","1",{"kinds":[24133],
      "#p":["79be667e…"]}]
    [nip46] bunker_listen: listening on 1 relay(s) for pubkey 79be667e…
    [nostr_relay_publish] sending to wss://relay.damus.io: ["EVENT",{
      "id":"…","pubkey":"79be667e…","created_at":<now>,"kind":24133,
      "tags":[["p","b79604c5…"]],"content":"<NIP-04 ciphertext>",
      "sig":"…"}]
    [nip46] published response to relay: wss://relay.damus.io
    [nip46] publish_response: published to 1 relay(s)
    standin: connect_to_client attempt 1 ok
    [compact] parse envelope: ["OK","<event id>",true,""]
    (2 s pause, then republish attempt)
    [nostr_relay_publish] sending to wss://relay.damus.io: ["EVENT",…]
    [nip46] published response to relay: wss://relay.damus.io
    WebSocket connection established
    WebSocket connection established
    standin: connect_to_client republish ok
    standin: connect published; listening for 45 s
    [compact] parse envelope: ["OK","<event id>",true,""]

Both publishes were accepted by the relay (`["OK", …, true, ""]`). The
event is authored by `79be667e…` (alice = well-known test private key
1's xonly pubkey, equal to the account's stored pubkey), tags the
broker-generated ephemeral client pubkey `b79604c5…`, and encrypts a
NIP-46 `connect` request whose params[1] is the URI's `secret=` token.
This is exactly the signer-initiated `connect` shape design §4.1 step
4 expects the broker to receive.

## 5. Path A blocker — broker's persistent client pool does not dispatch inbound events

The broker sends the subscription, the relay accepts published events,
but the broker's `nip46_persistent_client_cb` is never invoked. Broker
journal (post-`AF_INET` sandbox fix, run `ok3`):

    12:01:14 [nip46] relay_state: wss://relay.damus.io CONNECTED
    12:01:14 [pool] redial connected: wss://relay.damus.io
    12:01:14 [nostr_subscription_fire] sending: ["REQ","1",{"kinds":[24133],
      "since":1790078414,"#p":["cd4a7220…"]}]
    12:01:14 [nip46] client_start: persistent pool started with 1 relay(s)
    (78 s of silence — no `[compact] parse envelope: …` lines, no
     `[nip46] persistent_cb: received response from …` lines)
    12:02:32 [nip46] await_connect: timed out after 78000 ms

The `[compact] parse envelope` debug line is emitted by
`libnostr/src/envelope.c:332` whenever an envelope (EOSE, OK, EVENT,
AUTH, …) is parsed by the client's read loop. Its complete absence in
the broker's journal — including EOSE, which every relay sends
immediately after a REQ — means the broker's WebSocket read path never
delivered a message to the parser. Even the stand-in's own OK's for
its self-published events (which are echoed by the relay) never
reached the broker's subscription, despite the two clients' filters
overlapping on `#p=<client_pk>`.

Cross-check: the stand-in (which is a `nostr_simple_pool` too, on the
same relay, same libnostr build, same libwebsockets) received its
EOSE, OK and pong frames without issue and logged the compact-parse
lines. The stall is specific to `nostr_nip46_client_start`'s
`client_pool` on the broker side.

The persistent client subscription lifecycle
(`nip46_session.c:1050-1148`) is well-formed:
`ensure_relay` → state-callback registration → `session_registry_add`
before `simple_pool_start` → wait for a CONNECTED signal on the
connect channel → set filters and subscribe → return success. The
CONNECTED transition fires (§5 shows `[nip46] relay_state: … CONNECTED`
in the broker journal after the sandbox fix), so the pool is not
starting cold. The middleware is set:
`nostr_simple_pool_set_event_middleware(s->client_pool,
nip46_persistent_client_cb);`. What is not clear is why the pool's
read loop never delivers messages to that middleware.

Fixing this is deeper than the "minimal, in `gnome/nostr-homed` or
`nips/nip46`" scope this session was given, and the maintainer's
single-use bundle should not be spent against a broker in this state.
Filing as a follow-up (see §8).

## 6. Path B — not run (bundle preserved)

The re-pairing bundle for agent `e2a4…7240` was copied to the guest
under 0600, `bundle.json`. Path B was NOT executed because
`provider_nip46` uses the same
`nostr_nip46_client_start` → persistent pool → RPC-reply dispatch
plumbing that is stalled in §5 — its `connect_rpc` inside
`prepare()` would time out identically, silently consuming the
bundle's one-time secret without producing an
`authenticate … (nip46) -> ok` line. The bundle's `bunker_uri` and its
one-time `secret=` remain unused. The guest copy of the bundle was
`shred -u`'d at teardown; the workstation copy is unchanged.

## 7. Restore + verification

- `pam-auth-update --package --remove nostr` reverts
  `/etc/pam.d/common-auth` and `/etc/pam.d/common-account`.
- Snapshot restore of `/etc/nsswitch.conf` from
  `/root/nip46-accept-snapshot-20260922/nsswitch.conf`.
- `systemctl stop nostr-authd.service; systemctl disable
  nostr-authd.service` and removal of `/etc/systemd/system/nostr-authd.
  service.d/{debug,network}.conf` drop-ins.
- Removal of `/usr/sbin/{nostr-authd,nostr-homed-seed}`,
  `/usr/lib/x86_64-linux-gnu/security/pam_nostr.so`,
  `/usr/lib/x86_64-linux-gnu/libnss_nostr.so.2`,
  `/usr/share/pam-configs/nostr`, `/etc/nss_nostr.conf`, the shipped
  systemd unit, the SMB helpers, `libnostr-json.so*` and pkgconfig, and
  the state/run trees (`/var/lib/nostr-auth`, `/run/nostr-auth`,
  `/home/n_qralice`).
- Bundle copy shredded on the guest.

Post-restore checks:

    $ grep -c pam_nostr /etc/pam.d/common-auth /etc/pam.d/common-account
    /etc/pam.d/common-auth:0
    /etc/pam.d/common-account:0
    $ grep -E "^(passwd|group):" /etc/nsswitch.conf
    passwd:         files systemd sss
    group:          files systemd sss
    $ systemctl is-active gdm
    active
    $ systemctl status nostr-authd 2>&1 | head -1
    Unit nostr-authd.service could not be found.
    $ ls /run/nostr-auth
    gone
    $ pamtester -v gdm-password debugger authenticate <<<"…"
    Password: pamtester: Authentication failure   # pam_unix reached ✓

Final greeter screenshot at
`/tmp/nip46-accept-evidence/greeter-restored.png` shows the stock
Ubuntu GDM login dialog. The greeter extension remains installed at
`/usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/` (per the
task); with `/run/nostr-auth/greeter/` gone, it hides itself, and the
dconf keyfile still enables it for GDM. To disable without
uninstalling: `sudo rm /etc/dconf/db/gdm.d/10-nostr-login-qr &&
sudo dconf update && sudo systemctl restart gdm`.

## 8. Follow-ups

1. **[BLOCKER]** Broker's persistent NIP-46 client pool does not
   invoke `nip46_persistent_client_cb` for inbound relay events —
   preventing every relay-backed NIP-46 login flow (QR *and*
   pre-paired bunker) from ever completing at the real broker.
   Reproducible headlessly with the shipped
   `nh-seed-nip46-authority` + `qr_signer_standin` + `pamtester
   gdm-password` triple against `wss://relay.damus.io` on Ubuntu
   24.04 with the systemd unit (with the AF_INET fix applied). Every
   confirmed check upstream of dispatch (URI build, subscription
   filter, relay accept, standin publish, event tag/authorship,
   signature) checks out; the read loop simply does not deliver
   messages to the middleware.
2. Broker's `INTERACTION_REQUIRED → NH_AUTH_RESULT_INVALID_PROOF`
   collapse (design §4.1 note) surfaces the QR-timeout as
   `invalid_proof` to PAM, indistinguishable from a signing-key
   mismatch or a bad passphrase; ok for now but muddies §1's
   distinguishable-outcome principle. Small policy change; not this
   review's scope.
3. Re-run Path B against the same broker once (1) is fixed —
   maintainer's single-use bundle remains unspent.
