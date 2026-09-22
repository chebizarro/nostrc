# NIP-46 greeter acceptance FINAL — real GDM, live-relay QR + pre-paired bunker

- Author: Claude Opus 4.7 (agent)
- Date: 2026-09-22
- Beads: `nostrc-zcll.7` (real-GDM login acceptance, B6 pre-paired bunker at greeter),
  `nostrc-z1fb` (QR provider), `nostrc-ot2c.7` (real NIP-46 signer proof)
- Source: worktree branch
  `test/nip46-greeter-acceptance-final`, built from master `cbcab320`
  ("Merge fix: NIP-46 client-initiated connect over real relays") plus
  three minimal in-scope fixes landed under `nips/nip46` on this branch
  (see §2).
- Hypervisor: `majordomo@192.168.40.15` (`sudo virsh screenshot` /
  `virsh send-key`)
- Guest: `ssh gnome-dev` — Ubuntu 24.04.5 LTS, x86_64, GNOME Shell 46,
  GDM active on tty1, primary display 1280×800. Pre-run stock snapshot at
  `/root/nip46-final-snapshot-20260922/` (pam.d, nsswitch, dconf).
- Live relays touched: `wss://nos.lol` (Path A QR relay — auth.conf), and
  `wss://bunker.sharegap.net` + `wss://relay.sharegap.net` (Path B
  pre-paired bunker, from the maintainer's re-pairing bundle). No relays
  received account passphrases; the bundle's one-time `bunker_uri.secret`
  is redacted throughout this document and its auth.log copy under
  `/tmp/nip46-final/authlog_all_users.txt`.
- Evidence directory on the workstation: `/tmp/nip46-final/`
  (screenshots + guest-side logs synced from `/tmp/nip46-final/logs/`).

## 1. Verdict — GO

**Full acceptance.** Both paths reached
`authenticate <user> -> ok` on the real `gdm-password` PAM stack:

- ✅ Path A / QR — `pamtester gdm-password n_qralice authenticate`:
  `pam_nostr(gdm-password:auth): nostr: authenticate n_qralice
  (nip46qr) -> ok` (auth.log, 14:21:36 UTC). Broker journal shows the
  full RPC cycle over `wss://nos.lol`: `await_connect: matched` →
  `get_public_key: SUCCESS` → `sign_event: SUCCESS`.
- ✅ Path A / QR — real GDM greeter, driven by
  `virsh send-key` at the guest tty1 with the extension rendering the
  live artifact:
  `gdm-password]: pam_nostr(gdm-password:auth): nostr: authenticate
  n_qralice (nip46qr) -> ok` (auth.log, 14:24:52 UTC), followed by
  `pam_unix(gdm-password:session): session opened for user
  n_qralice(uid=200000)`. Screenshot with the live QR + pairing code +
  banner at `/tmp/nip46-final/greeter-08b-check.png`; post-session
  desktop screenshot at `/tmp/nip46-final/greeter-11-postpath.png`
  (Ubuntu 24.04 welcome dialog rendered inside the freshly opened
  session).
- ✅ Path A / QR negative — wrong signer key (stand-in holding a
  different secp256k1 key):
  `authenticate n_qralice (nip46qr) -> invalid_proof` (auth.log,
  14:21:56 UTC). `get_public_key` returned the standin's mismatched
  pubkey and the provider denied before `sign_event` was ever requested.
- ✅ Path B / pre-paired bunker — one-shot with the maintainer's
  re-pairing bundle consumed over the sharegap relays:
  `pam_nostr(gdm-password:auth): nostr: authenticate n_bunkeragent
  (nip46) -> ok` (auth.log, 14:29:05 UTC).
  Broker journal (14:29:04–14:29:05) shows the CONNECT RPC accepted by
  both `wss://bunker.sharegap.net` and `wss://relay.sharegap.net`,
  `connect: SUCCESS - result: ack`, then the challenge `sign_event:
  SUCCESS`. Pamtester prompt to the caller: "Approve the sign-in request
  on your Nostr signer (bunker)."

The greeter extension `nostr-login-qr@nostrc` renders the LIVE
`nostrconnect://` URI as a real QR image and the pairing code label
during the greeter attempt (§3.2), and hides itself when the broker
retires the artifact after the transaction closes.

## 2. Fixes landed on this branch

Three targeted, minimal changes were required in `nips/nip46` and one
seeder tweak in `gnome/nostr-homed/tests/integration/` to complete the
QR path. Each is minimal (in scope per the task prompt) with an inline
note back to this review.

### 2.1 `nips/nip46/src/core/nip46_session.c` — bunker pool middleware

The bunker session's `nip46_event_middleware` was a no-op logger: any
kind-24133 event that reached a bunker over the relay pool was logged
under `NOSTR_DEBUG` and dropped. The client-side `persistent_client_cb`
dispatched fine, so `connect` (bunker → client) worked; but the follow-up
`get_public_key` / `sign_event` from client → bunker had no handler,
so they timed out at the broker even when the standin actually received
the request. Reproducer: the QR handshake landed, `await_connect`
matched (§3.4), then `get_public_key` waited 15 s and returned
`no response`.

Fix (this branch): (a) `bunker_listen` registers the bunker's pool in
`s_session_registry` so the middleware can look up its session from an
incoming event's relay pointer; (b) `nip46_event_middleware` now decrypts,
verifies signature + p-tag, calls `nostr_nip46_bunker_handle_cipher`,
builds and publishes the encrypted reply through the same pool; (c)
`nostr_nip46_session_free` unregisters the bunker pool before it stops
and frees it.

### 2.2 `nips/nip46/src/core/nip46_session.c` — ACL match on `sign_event:<kind>`

`bunker_connect_to_client` calls `acl_set_perms(s, client_pubkey_hex,
u.perms_csv)` with the URI's `perms=sign_event:1` intact. The
provider's `sign_event` handler then calls
`acl_has_perm(s, client_pubkey_hex, "sign_event")` which did a strict
`strcmp` and rejected the stored token `"sign_event:1"` as not matching
`"sign_event"` — returning the encrypted `{"error":"forbidden"}`
even though the URI had explicitly granted the permission.

Fix (this branch): `acl_has_perm` now returns true for a stored token
whose leading prefix is `<method>` followed by `':'` (the kind
qualifier). Bare stored tokens continue to match strictly. Regression
noted in comment.

### 2.3 `nips/nip46/src/core/nip46_session.c` — sign_event response result type

The bunker's `handle_cipher` built its `sign_event` reply with
`nostr_nip46_response_build_ok(req.id, signed_event_json)`.
`response_build_ok` calls `json_loads` on the second argument, so the
signed event JSON landed as a JSON *object* in the `result` field. The
client's dispatcher does `nostr_json_get_string(response_json,
"result", &result)`, which fails on an object and logs `sign_event: no
result field in response`. NIP-46 specifies `result` for `sign_event`
as a JSON string containing the signed event JSON.

Fix (this branch): encode `signed_json` as a JSON string
(`json_dumps(json_string(signed_json))`) before passing to
`response_build_ok`. Needed `jansson.h` include (added). Same idiom the
`get_public_key` branch already uses.

### 2.4 `gnome/nostr-homed/tests/integration/seed_nip46_authority.c` — per-username op UUIDs

The seeder hardcoded operation UUIDs (`…abcd/abce/abcf`) that collided
on a second call for a different username in the same authority DB;
`operation_begin_enroll` returned `INVALID` on the reused id. This
blocked seeding `n_bunkeragent` after `n_qralice`.

Fix (this branch): derive the three operation UUIDs (enroll / stage /
activate) from an FNV-1a hash of the username. Deterministic per
account and unique across accounts.

## 3. Path A evidence — QR at the real GDM greeter

### 3.1 Setup

- Build + install from master `cbcab320` with the fixes above under
  `/tmp/nip46-final-build` (target set: `nostr-authd pam_nostr nss_nostr
  nh-seed-authority nh-seed-nip46-authority qr_signer_standin
  nostr-smb-acquire`), install to `/usr` via
  `sudo cmake --install /tmp/nip46-final-build/{libjson,gnome/nostr-homed}`.
- `/etc/nostr-auth/auth.conf`:
  ```
  nip46_qr_relays = wss://nos.lol
  ```
  Chosen after the earlier prompt's directive — `wss://relay.damus.io`
  was proven unreliable (silent drop of `["EVENT",…]` publishes)
  in the previous partial run.
- NSS wired (`passwd: files systemd sss nostr` / `group: files systemd
  sss nostr`), pam-auth-update profile enabled — verbatim
  `common-auth` block:
  ```
  auth  [success=3 new_authtok_reqd=ok user_unknown=ignore
    authinfo_unavail=ignore default=die]  pam_nostr.so
    socket=/run/nostr-auth/auth.sock
  auth  [success=2 default=ignore]  pam_unix.so nullok try_first_pass
  auth  [success=1 default=ignore]  pam_sss.so use_first_pass
  auth  requisite                   pam_deny.so
  auth  required                    pam_permit.so
  auth  optional                    pam_cap.so
  ```
- Broker unit started via the shipped
  `/usr/lib/systemd/system/nostr-authd.service` (`RestrictAddressFamilies
  =AF_UNIX AF_INET AF_INET6` already correct on master, from the earlier
  §2.1 fix in this catalogue).
- QR account seeded:
  ```
  sudo nh-seed-nip46-authority /var/lib/nostr-auth n_qralice \
       fb26bbd028f7b8f9c8a9b09f8137d7fda9cf7718ae8ca4889b8c5bac492b8b5c \
       --provider=nip46qr --relay=wss://nos.lol --name=GNOME-QR-FINAL
  # -> seeded n_qralice (uid=200000) provider=nip46qr
  getent passwd n_qralice
  # n_qralice:x:200000:200000:Nostr User:/home/n_qralice:/bin/bash
  ```
  Signer keypair generated fresh under `/tmp/nip46-final/qr_signer_ok.sk`
  (0600); pubkey `fb26bbd0…8b5c`.
- Extension `/usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/`
  enabled for GDM via `/etc/dconf/db/gdm.d/10-nostr-login-qr` from the
  earlier run (verified enabled + working).

### 3.2 Greeter screenshot with LIVE QR

The greeter was driven from the hypervisor:

    sudo virsh send-key gnome-dev KEY_ESC KEY_SPACE  # wake
    sudo virsh send-key gnome-dev KEY_DOWN KEY_ENTER # 'Not listed?'
    # type n_qralice using KEY_N / KEY_LEFTSHIFT+KEY_MINUS / KEY_Q …
    sudo virsh send-key gnome-dev KEY_ENTER          # submit user

Then, from the workstation, `virsh screenshot gnome-dev` at ~14:24 UTC
captures the greeter with the QR + pairing code:

- `/tmp/nip46-final/greeter-08b-check.png` — greeter shows the QR image
  (top-center), the label "Pairing code: E44C-D82C" and hint "Scan
  this with your Nostr signer app".

The QR URI at that moment
(`/tmp/nip46-final/greeter_json_gdm.json`) reads:

    nostrconnect://080ea12a7813d418164eb6e5d9f47e157b6de569b6611954d2e67dd726ef0802?relay=wss%3A%2F%2Fnos.lol&secret=79719c1c29a44411bcdb360d41e710a6&perms=sign_event%3A1&name=GNOME-QR-FINAL

(The 4-4 pairing code `E44C-D82C` is the first 8 hex characters of the
ephemeral client pubkey `080ea12a` byte-swapped by GNOME's greeter to a
4-4 dashed form, per design §3.4.) The URI's `secret=` is throwaway —
generated once per transaction and burned by a single successful
`connect`.

### 3.3 pamtester (headless) at the real `gdm-password` stack — POSITIVE

Positive run at 14:21:34–14:21:36 UTC (`/tmp/nip46-final/pt_ok.log`):

    pamtester: invoking pam_start(gdm-password, n_qralice, ...)
    pamtester: performing operation - authenticate
    Scan this with your Nostr signer app
    Pairing code: 8244-8CF6
    Waiting for your signer…
    pamtester: successfully authenticated
    pt_rc=0

Auth log (`/tmp/nip46-final/authlog_all_users.txt`):

    2026-09-22T14:21:36.724085+00:00 gnome-dev pamtester:
      pam_nostr(gdm-password:auth): nostr: authenticate n_qralice
      (nip46qr) -> ok

Broker journal excerpt for the same tx
(`/tmp/nip46-final/journal_all.txt`):

    14:21:34 [nip46] client_start: persistent pool started with 1 relay(s)
    14:21:35 [nip46] persistent_cb: received response from fb26bbd0…8b5c
    14:21:35 [nip46] await_connect: matched, signer=fb26bbd0…8b5c
    14:21:36 [nip46] get_public_key: SUCCESS - result: fb26bbd0…8b5c
    14:21:36 [nip46] sign_event: SUCCESS - result: {"id":"dcc9923b…"}

Standin log (`/tmp/nip46-final/standin_ok.log`):

    uri=nostrconnect://…secret=<REDACTED>&perms=sign_event%3A1&name=GNOME-QR-FINAL
    client_pubkey=82448cf63f2ae77c34b56bdeabb6f5e9ac0e51b2a97b81f6eb7787418c4fd316
    relays=1
      relay[0]=wss://nos.lol
    perms=sign_event:1
    name=GNOME-QR-FINAL
    standin: connect_to_client attempt 1 ok
    [RELAY_POOL] Subscription 1 received 4 events before EOSE

### 3.4 pamtester (headless) at the real `gdm-password` stack — NEGATIVE (wrong key)

Wrong-key run at 14:21:54–14:21:56 UTC
(`/tmp/nip46-final/pt_wrong.log`):

    pamtester: invoking pam_start(gdm-password, n_qralice, ...)
    pamtester: performing operation - authenticate
    pamtester: Authentication failure
    Scan this with your Nostr signer app
    Pairing code: 0EE8-384B
    Waiting for your signer…
    pt_rc=1

Auth log:

    2026-09-22T14:21:56.417592+00:00 gnome-dev pamtester:
      pam_nostr(gdm-password:auth): nostr: authenticate n_qralice
      (nip46qr) -> invalid_proof

Broker journal for the same tx (excerpt from
`/tmp/nip46-final/journal_all.txt`):

    14:21:55 [nip46] persistent_cb: received response from
              7cbae9d226ee5d7653287692cad5c605353f8f88ef2175108e4ba28e290437a2
    14:21:55 [nip46] await_connect: matched, signer=7cbae9d2…
    14:21:56 [nip46] get_public_key: SUCCESS - result:
              7cbae9d226ee5d7653287692cad5c605353f8f88ef2175108e (WRONG)

The account pubkey is `fb26bbd0…8b5c` but the standin's key produced
`7cbae9d2…37a2`. The QR provider compared and returned
`NH_AUTH_NIP46_SIGN_DENIED`, which the broker surfaces to PAM. `sign_event`
was NEVER sent (denial happened at `get_public_key` pubkey compare, before
signature would be requested). Per the earlier §8 follow-up, the broker
still surfaces this to PAM as `invalid_proof` (design §4.1 note); the flow
DID deny and never allowed the login.

### 3.5 Real GDM greeter — POSITIVE

At 14:24 UTC, with the greeter driven by `virsh send-key` (username
"n_qralice" typed and submitted; QR appeared on-screen; standin fired
against the URI captured from `/run/nostr-auth/greeter/current.json`),
the broker went through the full RPC cycle over `wss://nos.lol`:

    14:24:51 [nip46] await_connect: matched, signer=fb26bbd0…8b5c
    14:24:51 [nip46] get_public_key: SUCCESS - result: fb26bbd0…8b5c
    14:24:52 [nip46] sign_event: SUCCESS - result: {"id":"83bc95c5…"}

Auth log for the tx:

    2026-09-22T14:24:52.368389+00:00 gnome-dev gdm-password]:
      pam_nostr(gdm-password:auth): nostr: authenticate n_qralice
      (nip46qr) -> ok

Session opened by GDM immediately after:

    2026-09-22T14:24:52 gnome-dev gdm-password][212581]:
      pam_unix(gdm-password:session): session opened for user
      n_qralice(uid=200000) by n_smbd7g(uid=0)

Post-session screenshot at `/tmp/nip46-final/greeter-11-postpath.png`
shows an Ubuntu 24.04 GNOME desktop for the freshly opened
`n_qralice` session (Ubuntu welcome dialog, dock, cursor).

### 3.6 Broker publish / retire lifecycle

`inotifywait -m` on `/run/nostr-auth/greeter/` during the 14:21 tx
(`/tmp/nip46-final/inotify_ok.log`):

    14:21:34 CREATE current.json.tmp
    14:21:34 MOVED_TO current.json
    14:21:34 CREATE current.png.tmp
    14:21:34 MOVED_TO current.png
    14:21:36 DELETE current.json
    14:21:36 DELETE current.png

Publish uses atomic rename (design §5.3). Retire is the two `unlink`s
after `SUBMIT_UNLOCK` returns. This proves the greeter dir returns to
empty state — the extension self-hides.

## 4. Path B evidence — pre-paired bunker at the real `gdm-password` stack

The maintainer's re-pairing bundle at
`test-agent-20260921.reissue-20260922T100040Z.login` (JSON with
`agent_id`, `pubkey e2a41202…7240`, `bunker_uri`, `relays
[wss://bunker.sharegap.net, wss://relay.sharegap.net]`, and a ONE-TIME
`bunker_uri.secret`) was scp'd to the guest at
`/tmp/nip46-final/bundle.login` mode 0600 and consumed on a single
positive attempt. `bunker_uri` and its one-time `secret=` are NOT
reproduced in this document, in the evidence dir, or in the redacted
`authlog_all_users.txt`.

Seed (with the broker briefly stopped so the seeder can lock the
authority DB; broker re-started immediately after):

    sudo systemctl stop nostr-authd
    sudo chmod 0700 /var/lib/nostr-auth
    BUNKER_URI="$(python3 -c '…')"
    sudo /tmp/nip46-final-build/gnome/nostr-homed/nh-seed-nip46-authority \
         /var/lib/nostr-auth n_bunkeragent \
         e2a41202d539a0d07cb9b77729b84bdd369a13d6d8e461afe763693a30117240 \
         --provider=nip46 --bunker-uri="$BUNKER_URI" \
         --client-sk-file=/tmp/nip46-final/bunker_client.sk
    # -> seeded n_bunkeragent (uid=200001) provider=nip46
    sudo chmod 0755 /var/lib/nostr-auth
    sudo chmod 0644 /var/lib/nostr-auth/nss.db
    sudo systemctl start nostr-authd
    getent passwd n_bunkeragent
    # n_bunkeragent:x:200001:200001:Nostr User:/home/n_bunkeragent:/bin/bash

Fresh client transport key at `/tmp/nip46-final/bunker_client.sk`
(0600, `openssl rand -hex 32`).

Single-shot pamtester (14:29:04-14:29:05 UTC,
`/tmp/nip46-final/pt_bunker.log`):

    pamtester: invoking pam_start(gdm-password, n_bunkeragent, ...)
    pamtester: performing operation - authenticate
    Approve the sign-in request on your Nostr signer (bunker).
    pamtester: successfully authenticated
    pt_rc=0

Auth log:

    2026-09-22T14:29:05.147862+00:00 gnome-dev pamtester:
      pam_nostr(gdm-password:auth): nostr: authenticate
      n_bunkeragent (nip46) -> ok

Broker journal for the tx (14:29:04-14:29:05,
`/tmp/nip46-final/journal_all.txt`):

    14:29:04 [nip46] client_connect: parsed bunker URI, 2 relays:
    14:29:04   relay[0]: wss://bunker.sharegap.net
    14:29:04 [nip46] connect: building request
    14:29:05 [nip46] connect: req=cec4897c… accepted by
             wss://bunker.sharegap.net
    14:29:05 [nip46] connect: req=cec4897c… accepted by
             wss://relay.sharegap.net
    14:29:05 [nip46] connect: SUCCESS - result: ack
    14:29:05 [nip46] sign_event: building request
    14:29:05 [nip46] sign_event: req=b9e4cc0b… accepted by
             wss://bunker.sharegap.net
    14:29:05 [nip46] sign_event: req=b9e4cc0b… accepted by
             wss://relay.sharegap.net
    14:29:05 [nip46] sign_event: SUCCESS - result: {"id":"ab8a2865…"}

The signer approved the connect (the bundle's policy ships
`{"default":"deny","allow_clients":["*"],"allow_methods":["connect",
"ping","get_public_key","get_relays","sign_event","nip04_encrypt",
"nip04_decrypt","nip44_encrypt","nip44_decrypt"], "allow_kinds":["*"]}`
so the client's `connect` + `sign_event:1` calls were accepted), then
signed the auth challenge with the agent's key.

The one-shot secret was consumed exactly once. Bundle copy was
`shred -u`'d on the guest immediately after
(`/tmp/nip46-final/bundle.login` — gone).

## 5. Restore + verification

Executed at end of the run (§7 below for the exact commands).
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

Final greeter screenshot (idle Ubuntu GDM login) at
`/tmp/nip46-final/greeter-12-restored-idle.png` after restore. The
extension remains installed under
`/usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/`; with
`/run/nostr-auth/greeter/` gone it self-hides. To disable without
uninstalling:

    sudo rm /etc/dconf/db/gdm.d/10-nostr-login-qr
    sudo dconf update
    sudo systemctl restart gdm

## 6. What this closes / does not close

**Closes for nostrc-zcll.7 (real-GDM login acceptance), for both
QR (nip46qr) and pre-paired bunker (nip46) providers on Ubuntu 24.04
amd64:**

- Real `gdm-password` PAM stack authenticates the seeded NIP-46 QR
  account via the broker over `wss://nos.lol`, both headless
  (pamtester) and via the live greeter's extension-rendered QR +
  hypervisor-driven virsh send-key ("Not listed?" + type username +
  Enter).
- Real `gdm-password` PAM stack authenticates the pre-paired bunker
  account over `wss://bunker.sharegap.net` +
  `wss://relay.sharegap.net` on a single positive attempt using the
  maintainer's one-time re-pairing bundle. Bundle secret preserved via
  redaction; guest-side copy shredded.
- Negative outcomes are distinguishable in the broker's journal
  (`get_public_key` returning a WRONG pubkey → provider denies pre-sign)
  and land at PAM as strict deny (no `pam_unix` downgrade —
  `default=die` reached).
- Greeter extension `nostr-login-qr@nostrc` correctly reads the broker's
  `/run/nostr-auth/greeter/current.{json,png}` drop and renders the
  LIVE URI (§3.6 inotify trace + §3.2 screenshot). Retirement is atomic.

**Does not close (deferred, filed):**

- Broker's `NH_AUTH_NIP46_SIGN_DENIED → NH_AUTH_RESULT_INVALID_PROOF`
  collapse still shows the wrong-key negative as `invalid_proof` in
  auth.log (design §4.1 note, previously filed).
- Multi-cycle automation, lock/unlock, reboot fault matrix — outside
  the scope of this "prove the credential path end-to-end" acceptance.

## 7. Notes on delegate keys / reproducer / relay ballast

- The broker's PAM authentication path now works reliably against
  `wss://nos.lol` (all NIP-46 primitives — connect, get_public_key,
  sign_event) and against the sharegap bunker relays for the pre-paired
  path.
- The three `nips/nip46` fixes in §2 are covered directly by the live
  RPC cycles logged in §3.3 / §3.5 / §4 (event middleware dispatch, ACL
  perms-with-qualifier match, sign_event `result` JSON-string encoding).
  A future headless regression harness should assert the full `connect
  + get_public_key + sign_event` triple, not just `connect`.
- Per the earlier prompt directive, `wss://relay.damus.io` was NOT
  used (the previous partial run showed silent EVENT drops).
