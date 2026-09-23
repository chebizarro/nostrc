# Installed-system nostr PAM/NSS login

This document walks through installing the AUTH_INSTALL runtime, wiring PAM
and nsswitch persistently, and proving a **real** PAM-stack authentication of
a nostr account through the installed `pam_nostr.so` + `nostr-authd` broker.

The headless local-vault broker proof (`test_broker_login`) already runs green
as part of `ctest`.  This document instead exercises the **installed** runtime
end-to-end — the same code path a GNOME/GDM graphical login would take — so
that the loader search paths, systemd unit, socket modes, PAM stack and NSS
projection are all provably correct on a real system.

Beads: `nostrc-zcll.5` (B4 — broker-backed PAM), `nostrc-zcll.6` (B5 —
provider-choice UX), `nostrc-zcll.7` (B6 — installed-system proof).

## 1 — Build the AUTH_INSTALL flavour

The install target is gated by `NOSTR_HOMED_ENABLE_AUTH_INSTALL=ON`, which
requires `AUTH_RUNTIME`, `PAM` and `NSS` to also be on.  For the NSS module
to land on glibc's search path, configure with `CMAKE_INSTALL_PREFIX=/usr` —
`GNUInstallDirs` then resolves `CMAKE_INSTALL_LIBDIR` to the multiarch libdir
(e.g. `/usr/lib/aarch64-linux-gnu`).  A prefix of `/usr/local` will silently
put `libnss_nostr.so.2` where glibc will not look.

```sh
cmake -S <src> -B /tmp/pam-login-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DENABLE_NOSTR_HOMED=ON \
  -DBUILD_NOSTR_GTK=OFF -DBUILD_APPS=OFF -DBUILD_TESTING=OFF \
  -DBUILD_LIBHANAMI=OFF -DBUILD_TESTING_FRAMEWORK=OFF \
  -DSIGNET_ENABLE=OFF -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF \
  -DWITH_NIP77_NOSTRDB=OFF \
  -DNOSTR_HOMED_ENABLE_AUTH_CORE=ON \
  -DNOSTR_HOMED_ENABLE_IDENTITY_CORE=ON \
  -DNOSTR_HOMED_ENABLE_NSS=ON \
  -DNOSTR_HOMED_ENABLE_AUTH_RUNTIME=ON \
  -DNOSTR_HOMED_ENABLE_PAM=ON \
  -DNOSTR_HOMED_ENABLE_SMB=ON \
  -DNOSTR_HOMED_ENABLE_AUTH_INSTALL=ON

cmake --build /tmp/pam-login-build -j"$(nproc)" \
  --target nostr-authd pam_nostr nss_nostr nh-seed-authority nostr-smb-acquire

# The install rules for nostr-homed live under gnome/nostr-homed.  libnostr_json
# is a separate cmake subtree; both must be installed.
sudo cmake --install /tmp/pam-login-build/libjson
sudo cmake --install /tmp/pam-login-build/gnome/nostr-homed
sudo ldconfig
```

The install stages:

    /usr/sbin/nostr-authd
    /usr/lib/<multiarch>/security/pam_nostr.so
    /usr/lib/<multiarch>/libnss_nostr.so.2
    /usr/lib/systemd/system/nostr-authd.service
    /usr/bin/nostr-smb-acquire, /usr/bin/nostr-smb-mount
    /usr/share/nostr-homed/{auth,nss}.conf.sample

On SMB-enabled builds the unit ExecStart already carries the 4-arg form
(auth.sock + state dir + user.sock + smb.db).  `RuntimeDirectoryMode=0711`
lets unprivileged callers reach `user.sock` (mode 0666) without listing the
directory; `auth.sock` (mode 0600) remains root-only.

## 2 — Seed an account and start the broker

```sh
sudo mkdir -p /var/lib/nostr-auth && sudo chmod 0700 /var/lib/nostr-auth
sudo nh-seed-authority /var/lib/nostr-auth n_alice '<passphrase>'
sudo systemctl daemon-reload
sudo systemctl start nostr-authd.service     # or enable for boot
```

`nh-seed-authority` writes both the encrypted vault (in `authority.db`, mode
0600) and the read-only NSS projection (`nss.db`).  A one-time provisioning
step also creates `/home/n_alice`.

## 3 — Wire NSS

```sh
echo 'projection_path=/var/lib/nostr-auth/nss.db' \
  | sudo tee /etc/nss_nostr.conf
sudo chmod 0644 /etc/nss_nostr.conf /var/lib/nostr-auth/nss.db
sudo chmod 0755 /var/lib/nostr-auth       # traverse only; authority.db is 0600

# Append `nostr` to the passwd/group lines of /etc/nsswitch.conf (idempotent):
sudo sed -i '/^passwd:/ { /nostr/! s/$/ nostr/ }' /etc/nsswitch.conf
sudo sed -i '/^group:/ { /nostr/! s/$/ nostr/ }' /etc/nsswitch.conf
```

Verify resolution:

```sh
getent passwd n_alice
getent passwd 200000
id n_alice
getent passwd nh_nonexistent    # must be empty (NOTFOUND, not a DB error)
```

## 4 — Install the pam.d service and prove the stack

The isolated proof service (does not touch system login stacks) lives at
`packaging/pam/nostr-login.sample`.  Copy it to `/etc/pam.d/nostr-login` and
drive `pamtester(1)`.

```sh
sudo install -m 0644 \
  gnome/nostr-homed/packaging/pam/nostr-login.sample /etc/pam.d/nostr-login

# Correct passphrase -> PAM success:
printf 'correct horse battery staple\n' \
  | sudo pamtester -v nostr-login n_alice authenticate

# Wrong passphrase -> PAM auth failure (retries exhausted):
printf 'nope nope nope nope\n' \
  | sudo pamtester -v nostr-login n_alice authenticate

# Unknown user -> PAM_USER_UNKNOWN:
printf 'anything\n' | sudo pamtester -v nostr-login nh_ghost authenticate

# Active-account acct_mgmt -> PAM success:
sudo pamtester -v nostr-login n_alice acct_mgmt
```

Every attempt lands in `/var/log/auth.log` and journalctl with the broker
verdict:

    pam_nostr(nostr-login:auth): nostr: authenticate n_alice (local) -> ok
    pam_nostr(nostr-login:auth): nostr: authenticate n_alice (local) -> invalid_proof
    pam_nostr(nostr-login:auth): nostr: begin_login nh_ghost -> unknown_account

`pamtester` links against the same `libpam.so` GDM does — a passing pamtester
run is the substantive installed-system proof of the broker + PAM stack.

## 5 — Real GDM graphical login (manual step)

A real graphical login cannot be driven headlessly.  The sample GDM stack at
`packaging/pam/gdm-password.sample` stacks `pam_nostr.so` above the stock
`common-auth` include with
`[success=done new_authtok_reqd=done user_unknown=ignore authinfo_unavail=ignore default=die]`.
This is the deliberate **strict-deny** posture (beads nostrc-o1ho) — nostr is
authoritative for nostr accounts, while non-nostr users and broker outages fall
through safely:

- broker + correct proof for a nostr account -> `success=done`; GDM proceeds to
  session and `common-auth` is NOT consulted;
- broker DENIAL for a **known** nostr account (wrong passphrase / denied /
  rate-limited / disabled -> `PAM_AUTH_ERR` / `PAM_MAXTRIES` /
  `PAM_ACCT_EXPIRED`) -> `default=die` refuses the login immediately. Crucially
  it does **not** fall through to `common-auth`, so a failed nostr proof can
  never be downgraded to (or bypassed by) a local Unix password;
- account **unknown** to the broker -> `user_unknown=ignore`: an ordinary local
  user falls through to `common-auth` and logs in with Unix auth as usual;
- broker **unreachable** / transport error -> `authinfo_unavail=ignore`: falls
  through so a stopped broker never bricks local login (a nostr-only account
  then simply fails at `common-auth`).

Manual verification steps (from a graphical console, NOT over SSH — the risk
of locking yourself out is real):

1. Back up the shipped `gdm-password`:
   `sudo cp /etc/pam.d/gdm-password /etc/pam.d/gdm-password.bak`
2. Install the sample:
   `sudo install -m 0644 gnome/nostr-homed/packaging/pam/gdm-password.sample \
       /etc/pam.d/gdm-password`
3. `sudo systemctl restart gdm`
4. At the greeter, click *Not listed?*, type `n_alice`, then supply the vault
   passphrase.  On success GDM enters the seeded home; on wrong passphrase
   the greeter reports failure and the login is refused (a wrong proof
   yields `PAM_AUTH_ERR`, or `PAM_MAXTRIES` once the retry budget is spent; either
   way `default=die` stops the stack without falling through to Unix).
5. Restore: `sudo cp /etc/pam.d/gdm-password.bak /etc/pam.d/gdm-password`
   and `sudo systemctl restart gdm`.

Because pam_nostr's messages come through the standard PAM conversation, the
GNOME greeter renders them as prompts — no greeter-side plugin is required.
When the interactive local/remote provider choice needs to appear, GDM's
greeter uses the same `PAM_PROMPT_ECHO_ON` conversation to surface it.

## 5b — Close the live NIP-46 external-signer proof (C7)

When the sample GDM stack is driven with the NIP-46 provider (a real
bunker instead of the local vault), the pre-login challenge is a
`sign_event` for kind `NH_AUTH_CHALLENGE_KIND` = 1. If the bunker's
signet policy store has no `allow_kinds` rule permitting kind 1 for
the agent bound to our pre-paired client transport key, the broker
reports `invalid_proof` with `reason_code=policy.default_deny` and
GDM refuses the login even though connect + `get_public_key`
succeed.

Grant the kind turnkey with `signet/tools/grant-login-kind.sh`
(see `signet/docs/GRANT_LOGIN_KIND.md` for the full walkthrough,
the exact `signetctl set-policy` invocation, and the single
operator-supplied secret — `SIGNET_PROVISIONER_NSEC_FILE`). Then
confirm the positive path with:

```sh
NH_NIP46_LIVE=1 <build>/gnome/nostr-homed/test_broker_login_nip46_live
```

Beads: `nostrc-ot2c.7` (C7), `nostrc-7t61` (C7-operator ACL grant).

## 5c — NIP-05 login identifier (B5-NIP-05, nostrc-bit0)

The broker accepts a NIP-05 address (`local@domain`) at the GDM
"Not listed?" prompt as an alternative to the local username: it
resolves the address to a pubkey, looks the pubkey up in the identity
authority, and — if the pubkey is enrolled — canonicalises `PAM_USER`
to the local account's username before continuing the QR (NIP-46
`nostrconnect://`) flow.

### Alias policy (v1)

NIP-05 is treated **strictly as an identifier hint**.  Authentication
is unchanged: the signed challenge is bound to the account pubkey, so
a forged NIP-05 that happens to resolve to somebody else's pubkey
still cannot sign for them.

Enrolment is **alias-only**: an address must resolve to a pubkey that
is *already* enrolled.  If the pubkey is unknown the broker returns
`UNKNOWN_ACCOUNT` (mapped to `PAM_USER_UNKNOWN`) — no local account is
ever created just because a stranger typed a valid NIP-05 at the
greeter.  Just-in-time enrolment (an allowlist-gated auto-provision)
is tracked as follow-up bead `nostrc-037i` and is off in v1.

### Resolver isolation

The `.well-known/nostr.json` fetch runs in an unprivileged helper:

```
/usr/libexec/nostr-homed/nostr-homed-nip05 <local@domain>
```

The helper drops to `nobody` before `curl_easy_perform`; refuses to run
as root; enforces `https://` only (no `http`, no redirects to non-https);
caps the body at 64 KiB, the timeout at 10 s, and redirects at 3; and —
crucially — checks every resolved peer sockaddr against the profile's
SSRF gate (`nh_profile_ssrf_check_sockaddr`, shared with the avatar
helper) so RFC1918 / loopback / link-local / CGNAT / ULA destinations
are refused *before* the first byte is sent.  Exit codes are 64 (arg),
65 (SSRF refused), 66 (transport), 67 (content-type), 68 (JSON parse),
69 (name not found or not hex), 70 (helper still running as root), 71
(internal/OOM).

The broker itself never opens a socket for NIP-05 resolution.  It also
carries an in-memory `(address → pubkey, relay hints)` cache
(positive TTL default 10 min; negative TTL 60 s) and rate-limits
resolution attempts per address using the same throttle that governs
`SUBMIT_UNLOCK` failures — a greeter attacker cannot turn the login
screen into an unbounded SSRF probe or DoS amplifier against a
third-party issuer.

### `auth.conf` keys

Both keys live in the flat `/etc/nostr-auth/auth.conf` alongside
`nip46_qr_relays`.  Missing keys leave the compiled-in defaults;
malformed values are silently ignored per the design's non-fatal-
config rule.

| Key | Default | Notes |
|---|---|---|
| `nip05_resolve` | `on` | `on` / `off`.  When `off` the broker treats an `@`-containing username exactly like any other username (typically `UNKNOWN_ACCOUNT`). |
| `nip05_cache_ttl` | `600` (seconds) | Positive-hit TTL for the in-memory `(address → pubkey)` cache.  The negative TTL (SSRF / transport / name failures) is fixed at 60 s. |
| `nip05_image_user` | `` (empty) | Uname the helper drops to.  Falls back to `profile_image_user`, then `nobody`. |

Environment override: `NH_NIP05_HELPER=/absolute/path/to/nostr-homed-nip05`
tells the broker where to find the helper (default: search `PATH`).
The shipped systemd drop-in
`/etc/systemd/system/nostr-authd.service.d/20-nip05.conf` sets it to
`/usr/libexec/nostr-homed/nostr-homed-nip05`.

### Wire protocol changes (additive, protocol v1)

The `BEGIN_LOGIN` reply gains two optional fields when the caller
supplied a NIP-05 identifier:

- `"account_username": "n_bizarro"` — the canonical local username
  the identifier resolved to; `pam_nostr` calls
  `pam_set_item(PAM_USER, ...)` with this value so downstream PAM
  modules see the local uid.
- `"identifier": "chebizarro@coinos.io"` — the address as typed
  (normalized).  Republished in the greeter artifact's `account`
  block so the extension can show the pretty NIP-05 label.

Both fields are absent for a canonical-username login.  Old clients
ignore unknown additive fields per §5.3 (D8).

### Greeter artifact addition

`nh_broker_greeter_artifact_write` optionally emits an `account` block
into `/run/nostr-auth/greeter/current.json` and copies the
`/var/lib/AccountsService/icons/<user>` icon as `avatar.png` next to
the manifest — see
`gnome/nostr-homed/greeter-extension/README.md#account-block-b5-nip-05-nostrc-bit0`
for the consumer contract.

### PAM syslog trail

Every login records the mapping when the identifier differs from the
canonical username, e.g.:

```
gdm-password][…]: pam_nostr(gdm-password:auth): nostr: identifier chebizarro@coinos.io -> n_bizarro
nostr-authd[…]: nostr: nip05 chebizarro@coinos.io -> n_bizarro (cached)
```

Never the fetched body, never the pubkey (already logged at enrolment).

## 6 — Uninstall / lab teardown

The install layout is entirely under `/usr` + `/etc` + `/var/lib/nostr-auth`.
Removal is idempotent:

```sh
sudo systemctl stop nostr-authd.service 2>/dev/null || true
sudo rm -f /etc/pam.d/nostr-login /etc/nss_nostr.conf \
           /usr/lib/systemd/system/nostr-authd.service \
           /usr/lib/*/security/pam_nostr.so \
           /usr/lib/*/libnss_nostr.so.2 \
           /usr/sbin/nostr-authd /usr/bin/nostr-smb-acquire /usr/bin/nostr-smb-mount
sudo rm -rf /var/lib/nostr-auth /run/nostr-auth
# restore nsswitch (drop the trailing ` nostr` on passwd/group):
sudo sed -i 's/ nostr$//' /etc/nsswitch.conf
sudo systemctl daemon-reload; sudo ldconfig
```
