# Standalone Samba server (Nostr credential authority)

> Wave 3 Hotel — plan §4.2 B1/B2/B3/B4 (2026-09-25).  Beads
> `nostrc-69pw` (this work) and follow-up `nostrc-b6h1` (VFS plugin
> for v2).

This document is the mediation contract between the four moving
parts that make Nostr-authed Samba file sharing work on a
standalone (non-domain) host:

- **`nostr-authd`** — the credential authority.  Mints short-lived
  SMB passwords, installs them into the dedicated tdbsam passdb via
  `smbpasswd`/`pdbedit`, and journals every issuance in
  `/var/lib/nostr-auth/smb.db`.  Runs as `root` on the system bus.
- **`nostr-smbd.service`** — a dedicated `smbd` bound to
  `/etc/nostr-auth/smb.conf`.  Reads the passdb the authority
  populates; serves the share path an operator materialized.
- **`nostr-home-fuse.service`** — the porthome/FUSE mount that
  materializes the file tree `smbd` reads (`/var/lib/nostr-auth/
  share-root` by default).  A regular filesystem to `smbd`; a
  Nostr/Blossom sync target to porthome.
- **`nostr-smb-share`** — the dedicated unprivileged system user
  provisioned by `packaging/sysusers.d/nostr-smb-share.conf`.  Owns
  the FUSE mountpoint; `smbd` `force user`'s to it inside the share
  stanza so file writes land as the same uid the sync helper
  expects.

The point of the split: every failure mode is isolated to exactly
one of the four services.  A crash in `nostr-home-fuse` errors the
share but leaves `smbd` alive.  A crash in `smbd` leaves the FUSE
mount and the credential journal alone.  A hostile input to the
authority never reaches `smbd` — `smbd` only reads the tdbsam file
the authority produced.

## Files shipped

| File | Purpose |
|---|---|
| `config/smb-credentiald.conf.sample` | Authority policy (expiry, `smb_conf` path).  Forward-compat name — v1 hosts the authority inside `nostr-authd`; v2 lifts it into a separate daemon. |
| `config/smb.conf.standalone.sample` | Dedicated Samba config (`security = user`, `tdbsam`, SMB2/3 only, signing mandatory, no guest, no printing). |
| `config/servers.d/example.conf` | Server-identity policy read by `nostr-authd`: which shares this host exports, which users are allowed, expiry overrides. |
| `systemd/nostr-smbd.service.in` | The dedicated smbd instance.  Installed to the system unit dir; `%CMAKE_INSTALL_FULL_DOCDIR%` is templated at configure time. |
| `packaging/sysusers.d/nostr-smb-share.conf` | Provisions the `nostr-smb-share` user + group. |
| `docs/SAMBA_STANDALONE.md` | THIS document. |

## Mediation contract

### 1. Passdb writes go THROUGH the authority

The authority `nh_smb_authority_*` (source under
`gnome/nostr-homed/src/smb/`) is the only writer of
`/var/lib/nostr-auth/smbpasswd`.  Adapter `passdb_tdbsam` shells
out to Samba's own tools with explicit config selectors:

- `smbpasswd -c /etc/nostr-auth/smb.conf -a -s USERNAME` (add + silent stdin)
- `smbpasswd -c /etc/nostr-auth/smb.conf -d USERNAME` (disable)
- `pdbedit  -s /etc/nostr-auth/smb.conf -x -u USERNAME` (remove)

The two flags are **different** despite looking similar:

- `smbpasswd`'s `-c <path>` selects the config file.  Its `-s`
  option is silent-stdin mode (used for the add path, where the
  new password is piped in over stdin — see
  [smbpasswd(8)](https://www.samba.org/samba/docs/current/man-html/smbpasswd.8.html)).
- `pdbedit`'s `-s <path>` selects the config file.  There is no
  `-c` flag on `pdbedit`, per
  [pdbedit(8)](https://www.samba.org/samba/docs/4.9/man-html/pdbedit.8.html).

The invocations look confusingly parallel; they are not.  The test
`test_passdb_explicit_config.sh` asserts the exact argv for each
path.

### 2. Startup reconciliation

On `nh_smb_authority_open_ex()` the authority enumerates the passdb via
`pdbedit -L -s <smb.conf.standalone>`. On the first open of an empty SMB
journal, it records any operator-preseeded passdb usernames in a separate
`adopted_passdb` table and writes a one-time bootstrap marker. It logs the
number adopted; it does not invent Nostr credential metadata for them.

On every later open, the passdb usernames must match the union of active
issuance rows and adopted usernames in `/var/lib/nostr-auth/smb.db`. A
mismatch refuses readiness (`NH_SMB_RECONCILE_REQUIRED`); there is no
automatic repair after bootstrap. When a Nostr-issued credential replaces an
adopted account, that account leaves `adopted_passdb`. An operator must
investigate and reconcile post-bootstrap drift before restarting authd.

### 3. Rotation semantics

`nh_smb_credential_issue()` for a known user retires any active
credential (`NH_SMB_REVOKE_ROTATED`) and mints a fresh one.  Only
one credential is active per user at a time; the journal keeps the
historical trail for audit.

Sweep passes (`nostr-authctl smb-sweep`, plan §4.1 A3) walk expired
rows and revoke through the passdb adapter.  Failure of the passdb
CLI is logged but does not corrupt the journal.

### 4. Active-session semantics

`smbd` sees whatever the passdb contained at each session
authentication.  When the authority revokes a user (rotation,
sweep, or admin action), existing sessions **stay authenticated
until the client re-negotiates**.  If your threat model requires
tearing down live sessions on revoke, run `smbcontrol nostr-smbd
close-share <name>` from the same revoke path (documented but not
automated in v1 because closing a share races with active writes).

## Operator setup

1. Install packages that ship these files.  On Debian-derived
   systems the shipped Debian packaging (see
   `docs/BUILD_PACKAGING.md`) lands them in the right places; on
   any other host, copy by hand:
   - `smb-credentiald.conf.sample` → `/etc/nostr-auth/smb-credentiald.conf`
   - `smb.conf.standalone.sample` → `/etc/nostr-auth/smb.conf`
   - `servers.d/example.conf` → `/etc/nostr-auth/servers.d/<yourhost>.conf`
   - `nostr-smbd.service` → `/usr/lib/systemd/system/`
   - `nostr-smb-share.conf` → `/usr/lib/sysusers.d/`

2. Provision the dedicated system user and group:

   ```
   systemd-sysusers /usr/lib/sysusers.d/nostr-smb-share.conf
   ```

3. Fill in the server pubkey in `<yourhost>.conf`.  You get it
   from `nostr-authctl server show` — the same key `nostr-authd`
   already uses on the auth socket.

4. Verify the Samba config parses:

   ```
   testparm -s /etc/nostr-auth/smb.conf
   ```

   `testparm` MUST exit 0 with no `ERROR` lines.  CI runs this
   against the shipped sample; the same command works on the
   deployed file.

5. Start the FUSE mount **first**, then `smbd`:

   ```
   systemctl start nostr-authd.service       # authority
   systemctl start nostr-home-fuse.service   # materialize the share tree
   systemctl start nostr-smbd.service        # serve it
   ```

   The unit order is enforced by `After=`/`Wants=` in the
   `.service` file, so `systemctl start nostr-smbd.service` on a
   cold boot pulls the other two up in the right order.  During
   normal operation each service can be restarted independently.

6. Flip the sample share from `available = no` to `available = yes`
   in `/etc/nostr-auth/smb.conf` and `systemctl reload
   nostr-smbd.service`.

7. From a Windows or GNOME Files client, connect to
   `\\<yourhost>\nostr-home` with the credential minted by the
   client-side `nostr-smb-mount --acquire` (Wave 2 #15).

## Failure isolation

| Failure | Symptom | Recovery |
|---|---|---|
| `nostr-home-fuse` dies | Share returns ENOENT / EIO on file ops; smbd itself stays up | `systemctl restart nostr-home-fuse` — clients reconnect automatically |
| `nostr-smbd` crashes | Clients disconnect; the tdbsam passdb + FUSE mount untouched | `systemctl restart nostr-smbd`; live sessions re-auth against the same passdb |
| `nostr-authd` restart | In-flight issuance requests fail with retryable error; existing credentials remain valid until expiry | `systemctl restart nostr-authd`; sweep-on-open handles stale expiries |
| passdb drift (`RECONCILE_REQUIRED`) | `nostr-authd` refuses readiness at startup | Run `nostr-authctl smb-journal --dump` and reconcile by hand; the authority never auto-repairs |
| Sample share left `available = no` | `smbclient -L` shows the share but connecting reports "not available" | Flip to `available = yes` and `systemctl reload nostr-smbd` |

## Future

The following are RESERVED names / interfaces so a future
increment can land them without a rename churn:

- **`nostr-smb-credentiald.service`** — the eventual daemon
  produced by the rename-and-lift of the authority code out of
  `nostr-authd`.  Same code paths (`nh_smb_authority_*`), new
  `main()`, new unit, own socket.  No artifact ships in v1; the
  name is documented here so packaging and any operator recipes
  can plan around it.  Tracking: plan §4.2 B4 + `docs/plans/nostr-linux-samba-login-2026-09-19.md`
  §1331-1336.  When the split lands, v1's `smb-credentiald.conf`
  (this same file) is read unchanged by the new daemon.

- **VFS plugin (`vfs objects = nostr_porthome`)** — v2 alternative
  to share-declaration.  Instead of materializing the whole tree
  on-disk, the plugin on-demand-fetches Blossom-chunked files
  inside smbd.  Filed as bead `nostrc-b6h1` with per-share
  opt-in, smbd crash-budget tests, and pinned-Samba-ABI build
  matrix as acceptance criteria.

- **DC-adjacent join** — Nostr → Kerberos machine-credential
  refresh, punted per plan §4.3 C3.  A joined host still uses
  `smb.conf.winbind.sample`, not this standalone config.

## References

- Plan: `docs/plans/gnome-integration-and-samba-server-2026-09-25.md` §4.2
- Prior plan: `docs/plans/nostr-linux-samba-login-2026-09-19.md` §1331-1336
- Auth protocol: `docs/AUTH_PROTOCOL.md`
- Header for the authority code: `gnome/nostr-homed/src/smb/smb_credential.h`
- Tdbsam adapter: `gnome/nostr-homed/src/smb/passdb_tdbsam.c`
