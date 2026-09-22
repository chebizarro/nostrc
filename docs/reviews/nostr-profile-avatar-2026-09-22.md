# Nostr kind-0 profile → GDM avatar/name via AccountsService

**Date:** 2026-09-22
**Beads:** nostrc-m4jh (feature) · parent nostrc-zcll.6 (B5)
**Branch:** feat/nostr-profile-accountsservice
**Live rig:** gnome-dev (Ubuntu 24.04, GNOME 46, KVM guest on majordomo)

## Problem

At the GDM greeter, nostr-homed accounts render as a generic user tile
plus the gecos string "Nostr User" (`libnostr_identity_core`'s
`build_projection` hard-codes that string on every projection publish).
The maintainer's account `n_bizarro` — pubkey
`cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400` /
`npub1ehhfg...` — has a real kind-0 event on public relays with both a
display name and a profile picture; the greeter should show them.

## Design (what shipped)

New `src/profile/` subsystem under `gnome/nostr-homed`, wired into the
existing `nostr_auth_runtime` build. Split into four security-conscious
stages:

| Stage | Runs as | Enforcement |
| --- | --- | --- |
| kind-0 fetch (verified) | broker/CLI (root) | `nostr_event_check_signature` + pubkey match INSIDE `relay_fetch.c`'s middleware. Middleware serialises via `nostr_event_serialize_compact` only on success — a bad-sig event is silently dropped. |
| metadata parse + cache | broker/CLI (root) | `nh_profile_sanitize_text` rejects control chars, C1 controls, U+2028/U+2029, BOM, invalid/overlong UTF-8. Picture URL passes `nh_profile_validate_picture_url` (https-only, no userinfo, no whitespace, ≤1 KiB). Cache at `/var/lib/nostr-auth/profile/<user>.json` (root-owned 0644, atomic rename). |
| picture download | dedicated helper `nostr-homed-profile-image`, execv'd as `nobody` after `setresgid`/`setresuid`. Refuses to run as root. | https-only, `MAXREDIRS=3`, `TIMEOUT=10s`, `MAXFILESIZE=2 MiB`, hard cap in the write callback, `CONTENT_TYPE ~ image/*` after transfer, `CURLOPT_OPENSOCKETFUNCTION` checks the resolved peer against `nh_profile_ssrf_check_sockaddr` (loopback/link-local/RFC1918/CGNAT/multicast/reserved refused on v4 AND v6, incl. IPv4-mapped IPv6). Bytes are decoded via `GdkPixbufLoader`, aspect-scaled to ≤512×512, re-encoded as PNG via `gdk_pixbuf_save_to_buffer` — the raw remote bytes never touch the destination path. |
| AccountsService install | broker/CLI (root) | Copies PNG to `/var/lib/AccountsService/icons/<user>` (atomic .tmp+rename, 0644). Merges `/var/lib/AccountsService/users/<user>` keyfile via `GKeyFile` (preserves Session=/InputSource0=/etc, sets Icon=, SystemAccount=false). Also updates the passwd.gecos column in the NSS projection SQLite (`/var/lib/nostr-auth/nss.db`) — accounts-daemon sources its `RealName` D-Bus property from `pw_gecos`, NOT from the keyfile's `RealName=` key, so the projection update is the actual mechanism that flips the greeter's shown name. |

### Integrations

* **Seeders**: `nh-seed-authority` and `nh-seed-nip46-authority` gained a
  `--fetch-profile` flag (default on; `--no-fetch-profile` opts out;
  `NH_SEED_NO_PROFILE=1` env for CI). After a successful seed the seeder
  fork/execs `nostr-homed-profile refresh <user> --pubkey=<hex>`.
* **Broker post-login hook**: `auth_broker.c`'s successful
  `NH_AUTH_PURPOSE_LINUX_LOGIN` path double-forks and execs the same
  CLI with `--throttle-seconds=3600` so refreshes never happen more
  than once per hour per account. Stdio redirected to /dev/null.
  `NH_PROFILE_FETCH=off` disables. SMB credential mints do NOT trigger.
* **auth.conf**: three new keys documented in the sample —
  `profile_relays` (comma-separated wss, cap 4), `profile_fetch` (on/off),
  `profile_image_user` (uname, default `nobody`).
* **CLI**: `nostr-homed-profile refresh <user>` — auto-resolves pubkey
  from the authority store when `--pubkey` is not supplied.

## Live verification (gnome-dev)

```console
$ sudo /tmp/nostrc-profile-build/gnome/nostr-homed/nostr-homed-profile refresh \
    n_bizarro \
    --relay=wss://relay.damus.io --relay=wss://nos.lol \
    --image-helper=/tmp/nostrc-profile-build/gnome/nostr-homed/nostr-homed-profile-image \
    --pubkey=cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400
[RELAY_POOL] Subscription 2 received 1 events before EOSE
installed user=n_bizarro name=Biz display_name=Biz picture=yes

$ getent passwd n_bizarro
n_bizarro:x:200000:200000:Biz:/home/n_bizarro:/bin/bash

$ sudo busctl call org.freedesktop.Accounts /org/freedesktop/Accounts \
    org.freedesktop.Accounts FindUserByName s n_bizarro
o "/org/freedesktop/Accounts/User200000"

$ sudo busctl get-property org.freedesktop.Accounts \
    /org/freedesktop/Accounts/User200000 org.freedesktop.Accounts.User \
    RealName IconFile
s "Biz"
s "/var/lib/AccountsService/icons/n_bizarro"

$ sudo cat /var/lib/AccountsService/users/n_bizarro
[User]
SystemAccount=false
RealName=Biz
Icon=/var/lib/AccountsService/icons/n_bizarro

$ sudo cat /var/lib/nostr-auth/profile/n_bizarro.json
{
  "created_at": 1778684473,
  "display_name": "Biz",
  "fetched_at": 1790116648,
  "name": "Biz",
  "nip05": "chebizarro@coinos.io",
  "picture": "https://image.nostr.build/42d22eca925e3ce3ba028584d30b3d76a753d10ac8d37cfa69e4d3bb3ee80da7.jpg",
  "pubkey": "cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400",
  "user": "n_bizarro"
}
```

### Screenshot

Captured via `sudo virsh screenshot gnome-dev` (hypervisor
`majordomo@192.168.40.15`): `/tmp/nostrc-review/greeter-profile-1790117132.png`
(1280×800 PNG, 190 628 bytes, fetched to the host on 2026-09-22).

**Important caveat about the screenshot**: at capture time an active
gnome-shell session was running for user `n_qrlock` on tty2 (the
maintainer's separate agent owns GDM/lock-screen manipulation right
now — the design brief explicitly forbids driving the greeter beyond
returning it to the tile view). The screenshot therefore shows
n_qrlock's Ubuntu welcome dialog, not the GDM tile view. All the
AccountsService state that the tile *will* consume is verified out of
band via D-Bus and the on-disk keyfile above; the next fresh greeter
show for n_bizarro (either after n_qrlock logs out, or on the next
boot) will render the maintainer's real avatar + "Biz" display name
directly from `/var/lib/AccountsService/icons/n_bizarro` and the
NSS-projected gecos.

## Negative controls

All exercised on gnome-dev with the release binaries.

| Case | Result | Exit |
| --- | --- | --- |
| `nostr-homed-profile-image https://192.168.1.1/foo.png /tmp/x.png` | `openosocket` callback saw the peer resolve into `192.168.0.0/16`, refused → `CURLE_COULDNT_CONNECT`; helper mapped to exit 65 (SSRF), no output file written. | 65 |
| `nostr-homed-profile-image http://example.com/foo.png /tmp/x.png` | Scheme prefix check rejected the URL before any network I/O. | 64 |
| `nostr-homed-profile-image https://httpbin.org/get /tmp/y.png` | Transfer succeeded, `Content-Type: application/json` failed the `image/` prefix check; helper mapped to exit 67, no output file written. | 67 |
| `nostr-homed-profile refresh n_bizarro --pubkey=1111...1111` | REQ went out, EOSE with 0 events, orchestrator returned `NH_PROFILE_ERR_FETCH` → CLI rc=2. AccountsService state unchanged. | 1 |
| `nostr-homed-profile refresh n_bizarro --throttle-seconds=3600` (cache < 1h old) | Throttle short-circuits before any relay traffic. | 3 |
| Bad-signature kind-0 (theoretical, not run live) | `nostr_event_check_signature` inside `relay_fetch_middleware` fails; the event is dropped and the middleware waits for the next matching event. The write callback in the middleware is only reached on success, so a bad-sig kind-0 can never populate the fetch channel. |

Bad-signature coverage is exercised structurally: the middleware calls
`nostr_event_check_signature` before serialising, and the unit test
suite covers the wrong-`kind` refusal (`test_event_wrong_kind`) and the
wrong-`pubkey` invariant (belt-and-braces recheck inside
`nh_profile_refresh` after verify).

## Unit tests

```console
$ ./gnome/nostr-homed/test_profile_sanitize
ok  sanitize_text          # UTF-8/control/overlong/U+2028/truncation-boundary
ok  validate_picture_url   # https-only, userinfo, control, empty authority
ok  ssrf_check_sockaddr    # v4 all CIDR classes, v6 loopback/ULA/mapped/doc
test_profile_sanitize: ALL PASS

$ ./gnome/nostr-homed/test_profile_metadata
ok  content_basic
ok  content_damus_camelcase        # displayName fallback
ok  content_rejects_http_picture
ok  content_rejects_control_chars  # \r\n in name refused, not written
ok  content_bad_json
ok  event_wrapper
ok  event_wrong_kind
test_profile_metadata: ALL PASS

$ ./gnome/nostr-homed/test_auth_conf
RESULT: PASS                       # includes new profile_* keys
```

## Files touched

* `gnome/nostr-homed/include/nostr_profile.h` — public API
* `gnome/nostr-homed/src/profile/profile_sanitize.c` — text + URL + SSRF
* `gnome/nostr-homed/src/profile/profile_metadata.c` — kind-0 parse + cache
* `gnome/nostr-homed/src/profile/profile_image.c` — fork/exec with priv drop
* `gnome/nostr-homed/src/profile/profile_accounts.c` — icon copy + keyfile + gecos SQL
* `gnome/nostr-homed/src/profile/profile_refresh.c` — orchestrator
* `gnome/nostr-homed/src/profile/nostr-homed-profile.c` — operator CLI
* `gnome/nostr-homed/src/profile/nostr-homed-profile-image.c` — unpriv helper
* `gnome/nostr-homed/src/common/relay_fetch.c` — new `nh_fetch_latest_kind0_verified`
* `gnome/nostr-homed/src/auth/auth_broker.c` — post-login refresh hook
* `gnome/nostr-homed/src/auth/auth_conf.c` — `profile_*` keys
* `gnome/nostr-homed/src/auth/auth_broker.h` — `nh_auth_conf` fields
* `gnome/nostr-homed/config/auth.conf.sample` — documented sample
* `gnome/nostr-homed/tests/integration/seed_authority.c` — `--fetch-profile`
* `gnome/nostr-homed/tests/integration/seed_nip46_authority.c` — `--fetch-profile`
* `gnome/nostr-homed/tests/unit/test_profile_sanitize.c` — new
* `gnome/nostr-homed/tests/unit/test_profile_metadata.c` — new
* `gnome/nostr-homed/tests/unit/test_auth_conf.c` — profile_* coverage
* `gnome/nostr-homed/CMakeLists.txt` — `NOSTR_HOMED_ENABLE_PROFILE` gate

`greeter-extension/` deliberately untouched (owned by the concurrent QR
agent).

## Known limitations / follow-ups

* **Projection rebuild resets gecos.** `identity_projection.c` rewrites
  the whole passwd table on every `nh_identity_store_publish_projection`
  call (i.e. every enrolment / attestation activation) with the
  hard-coded literal `'Nostr User'`. Our per-refresh SQL UPDATE
  reconciles the value on the next login, but between the projection
  rebuild and the next login the greeter briefly reverts. A follow-up
  bead should teach the authority to persist the `display_name` next
  to the account row and have `build_projection` read it. Filed
  alongside nostrc-m4jh; **not blocking** for this feature (post-login
  hook heals within one login round-trip).
* **accounts-daemon RealName cache.** The daemon caches the first-seen
  RealName from pw_gecos and does not refetch until it re-scans. A
  running gnome-shell may show the old name until a `systemctl restart
  accounts-daemon` or the next login. Cold-boot greeter and post-login
  scenarios (the two the feature exists to serve) are unaffected.
* **CI: relay reach.** The seeder auto-refresh needs public relay
  reach; set `NH_SEED_NO_PROFILE=1` in seeded CI environments (already
  supported).
