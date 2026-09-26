# Samba/AD desktop login candidate

**Status:** configuration contract only; no support or acceptance claim.

This path is conventional Samba winbind authentication. It does not translate a
Nostr signature into an AD password, certificate, Kerberos credential, or TGT,
and it does not enroll domain identities in the Nostr authority.

The inert samples are `config/domain-route.conf.sample` and
`config/smb.conf.winbind.sample`. Validate them with:

```sh
python3 packaging/domain/validate_domain_profile.py \
  --route config/domain-route.conf.sample \
  --smb config/smb.conf.winbind.sample \
  --manifest packaging/domain/domain-profile.manifest.json.sample
```

The reserved routes are mutually exclusive: `n_` is Nostr-only,
`DOMAIN\\user` is winbind-only, and unqualified names retain the distribution
local policy. `winbind use default domain = no`, non-overlapping idmap ranges,
`/home/%D/%U`, mode `0700`, and offline domain login disabled are mandatory.
Domain authentication failure cannot fall through to another route.

The samples are installed under the documentation data directory only when the
`domain_config` feature is explicitly enabled. They are never installed as
`/etc/samba/smb.conf`, `nsswitch.conf`, or a PAM policy. Activation requires a
future distribution-specific package after D4 proves join/trust, actual GDM PAM
dispatch, Kerberos-versus-samlogon behavior, credential-cache lifecycle, and
route isolation on pinned packages.

Required real-lab evidence remains: `net ads testjoin`, `wbinfo -t`, fully
qualified `getent`/`id`, GDM login, cache ownership and cleanup, DC-unavailable
behavior, same-short-name isolation, safe home creation, and preservation of
Nostr-local and break-glass login during winbind failure. None is available in
the current environment.

## Manual join recipe

Plan §4.3 C1 (gnome-integration-and-samba-server 2026-09-25): this is the
documented, operator-driven join recipe. There is **no** automated join
machinery in this package and no Nostr→Kerberos conversion. Everything below
is executed by an administrator with root on the target host, after the
`packaging/pam/nostr-winbind-policy.md` posture has been reviewed.

Terms used below:

- **DOMAIN** — short NetBIOS name of the AD domain (uppercase).
- **REALM** — the DNS realm (e.g. `CORP.EXAMPLE.COM`, uppercase).
- **DC** — a domain controller reachable from the host.

### 1. Prerequisites (DNS and time)

1. `/etc/resolv.conf` MUST resolve `REALM` and each DC's A/AAAA record.
   Prefer configuring resolved / NetworkManager to point at the DC as the
   authoritative resolver for `REALM`; do NOT ship a hand-edited
   `/etc/resolv.conf` in the package.
2. Time skew MUST be under five minutes vs the DC. Use `chronyd` or
   `systemd-timesyncd` against the DC or the site's authoritative NTP
   source, then verify:

   ```sh
   chronyc tracking     # or: timedatectl show-timesync
   ```

3. Verify DC reachability BEFORE join:

   ```sh
   host -t SRV _kerberos._udp.REALM
   host -t SRV _ldap._tcp.dc._msdcs.REALM
   ```

### 2. Package pins

Install the pinned Samba + winbind + libpam-winbind + libpam-runtime set
recorded in the activation manifest. `packaging/domain/validate_domain_profile.py`
must accept the manifest (`--require-activation-ready`) BEFORE any PAM file is
touched — an unknown or locally modified layout is a hard refusal by design.

### 3. `smb.conf`

Deploy `config/smb.conf.winbind.sample` verbatim (with `workgroup`, `realm`,
and the `idmap config DOMAIN` stanzas materialised for your `DOMAIN`). The
sample MUST validate through `validate_domain_profile.py`.

Critical settings that come out of the sample and MUST NOT be dropped:

- `winbind use default domain = no` — qualified names only.
- Non-overlapping idmap ranges. Deploy the ranges BEFORE any user creates
  files, or file ownership will drift.
- `template homedir = /home/%D/%U` with mode `0700`.
- `winbind offline logon = no` — offline domain login is out of scope until
  separately accepted.

### 4. Deploy idmap range BEFORE first login

The idmap DB is created lazily on first lookup. Deploying `smb.conf` with a
new range and then joining is safe; deploying a range after users have already
authenticated risks re-mapping existing UIDs. If unsure, verify with:

```sh
wbinfo --user-info=DOMAIN\\Administrator
```

and confirm the UID is inside the range declared in `smb.conf`.

### 5. Join

```sh
net ads join -U 'Administrator'
```

The command prompts once for the domain administrator's password. On success
it materialises `/etc/krb5.keytab` and updates the local secrets DB — no
follow-up steps are required, and the package does NOT ship keytab material.

### 6. Enable winbind

```sh
systemctl enable --now winbind.service
```

### 7. Splice NSS ordering

Splice the `passwd`/`group`/`shadow` lines from
`config/nsswitch.conf.snippet.sample` into `/etc/nsswitch.conf`. The required
ordering is:

```
passwd: files winbind nostr
group:  files winbind nostr
shadow: files
```

`winbind` MUST precede `nostr`. The package MUST NOT overwrite
`/etc/nsswitch.conf`; this is an operator-driven splice. See
`packaging/pam/nostr-winbind-policy.md` — the NSS-ordering section — for the
rationale.

### 8. PAM

Do NOT edit any PAM file by hand. Follow the pinned PAM graph from the
superseded 09-19 plan (`docs/plans/nostr-linux-samba-login-2026-09-19.md`
§3.12), reproduced under `packaging/pam/nostr-winbind-policy.md`. The
package's PAM profile activates through `pam-auth-update` (Debian) using the
posture the winbind-policy document pins.

### Verification checklist

After the eight steps above, all of the following MUST succeed:

- `net ads testjoin` — machine credential valid against the DC.
- `wbinfo -t` — trust secret intact.
- `getent passwd 'DOMAIN\Administrator'` — winbind answers, NOT nostr.
- `getent passwd 'n_alice'` — nostr answers only if `n_alice` is enrolled;
  otherwise NOTFOUND. Bit-exact vs a non-joined host.
- `id 'DOMAIN\Administrator'` — UID inside the idmap range declared in
  `smb.conf`.
- GDM domain login to `DOMAIN\alice` succeeds and creates `/home/DOMAIN/alice`
  mode `0700`.
- Killing winbind (`systemctl stop winbind`) does NOT break `n_alice` login.
- No fall-through: a failed domain authentication returns to the login screen;
  it must never resolve to a local Unix or Nostr account.

## Keytab pipeline — PUNT

Plan §4.3 C3 (gnome-integration-and-samba-server 2026-09-25): this package
does NOT ship a keytab issuance or renewal pipeline. `net ads join` (step 5
above) produces the initial `/etc/krb5.keytab`; refreshing that keytab on
credential rotation, and issuing user TGTs, are **explicitly out of scope**.

Rationale:

1. A keytab issuance pipeline is Kerberos credential materialisation — the
   09-19 plan's PKINIT analysis already established that any Nostr-authorised
   Kerberos bridge is "a separate identity-federation program", requiring CA
   and KDC machinery this plan does not deliver.
2. The domain lab (`nostrc-rb0e.1`) is currently blocked; shipping an
   untested keytab pipeline would repeat exactly the failure mode the 09-19
   critique flagged for automated join — "notes ≠ ground truth".
3. Scope is intentionally reduced to **machine credential refresh only**
   (not user TGTs) once the follow-up lands, so credential rotation stays a
   host operation, not a login-time hop.

A follow-up bead is filed for the machine-credential refresh work only, gated
on `nostrc-rb0e.1` unblocking. Until it lands, operators MUST run the
distribution's own machine-credential refresh (typically `samba-tool domain
join` reissuance or `net ads changetrustpw` from a cron / systemd timer) as
part of their host provisioning story.
