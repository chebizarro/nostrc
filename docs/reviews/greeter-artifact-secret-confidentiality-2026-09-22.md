# Greeter-artifact pairing-secret confidentiality (nostrc-3c7n / references nostrc-zcll.6)

**Date:** 2026-09-22
**Author:** broker agent (n_bizarro)
**Beads:** `nostrc-3c7n` (this fix), related `nostrc-zcll.6` (B5 GDM UX)
**Live rig:** `gnome-dev` (Ubuntu 24.04, gdm3 running as uid 124)

## Vulnerability

The broker (`nostr-authd`, `gnome/nostr-homed/src/auth/auth_conf.c`)
published its NIP-46 QR greeter artifact under `/run/nostr-auth/greeter/`
with the drop directory mode `0755 root:root` and each file (`current.json`,
`current.png`, `avatar.png`) mode `0644 root:root`. The `greeter-extension/`
README explicitly documented this as intentional: the reasoning was
that the pairing secret embedded in the URI (see
`docs/designs/nip46-qr-login-greeter.md` §8.1) is a one-time proof-of-
possession token whose downstream misuse is gated by the account-pubkey
binding check (§8.3), so world-read was "fine".

That reasoning misses a griefing / DoS attack. `current.json` embeds the
full `nostrconnect://` URI **including its `secret=…` pairing token**,
and `current.png` is the same URI rendered as a QR. Any local unprivileged
user (SSH in, another seat) can:

1. Slurp `/run/nostr-auth/greeter/current.json` while the victim's GDM
   QR panel is on screen.
2. Dial the manifest's `relay=` list with any Nostr key, complete the
   `nostrconnect://` handshake, and **burn the single-use `secret=`**.
3. The pubkey gate correctly refuses the attacker's proof (they are not
   the enrolled signer), but the secret is now spent — the victim's
   real phone signer sees a "session already claimed" error and their
   login/unlock fails.

The attack works from any local shell without root. It is a full DoS on
NIP-46 QR login for any user targeted this way, and the victim has no
way to distinguish it from a benign expiry.

## Fix

Publish the drop dir 0750, files 0640, group-owned `nostr-auth-greeter`.
Only members of that group (gdm at the greeter, the seated user via
`pam_group` during `unlock-dialog`) can read the manifest/QR.

Implementation:

- `src/auth/auth_conf.c` — new `write_atomic_locked()` that opens the
  `.tmp` file at mode `0`, `fchown`s to the greeter group, `fchmod`s to
  `0640`, and only then `rename(2)`s. The atomic swap never exposes a
  widened mode window to an inotify-watching attacker. `ensure_dir()`
  is hardened to re-`chmod`/`chgrp` the drop directory to `0750 root:
  nostr-auth-greeter` on every publish, so a stale-state cleanup or an
  older broker's `0755` cannot linger. Group name is resolved by name
  at write time (default `nostr-auth-greeter`); a lookup failure
  degrades to `0640 root:root` (readable only to root — the pairing
  secret stays confidential, at the cost of the greeter extension not
  being able to render for that publish) and logs one WARNING per
  boot.
- `src/auth/auth_broker.h` — new `nh_broker_greeter_artifact_set_group()`
  test seam.
- `systemd/nostr-authd.service.in` — `ExecStartPre=/usr/bin/install -d
  -m 0750 -o root -g nostr-auth-greeter /run/nostr-auth/greeter` so the
  subdir has the right posture even before the broker has a publish to
  make. The parent runtime dir stays `0711 root:root`.
- `packaging/sysusers.d/nostr-auth-greeter.conf` — `sysusers.d(5)`
  snippet provisioning the system group. Installed under
  `${prefix}/lib/sysusers.d/`.
- `packaging/pam/nostr-auth-greeter.group.conf.sample` — sample for
  `/etc/security/group.conf.d/`. Grants the seated user the
  supplementary group at PAM session-open time so the lock screen
  (`unlock-dialog`) inside their gnome-shell can read the drop. Debian/
  Ubuntu ship `pam_group.so` enabled in `/etc/pam.d/login` by default;
  other distros may need to add the line.
- `CMakeLists.txt` — installs the sysusers snippet + sample under
  `AUTH_INSTALL`.
- `greeter-extension/README.md` — producer contract updated: modes,
  ownership, `sysusers.d` provisioning, gdm/user-session group
  membership steps, and a security paragraph pointing at this review.
- `tests/unit/test_greeter_artifact_perms.c` — headless perms test
  (registered `homed_greeter_artifact_perms` under `LABELS
  "nostr-homed;portable;auth-runtime;security"`). Asserts:
  - drop dir is `0750` and group-owned by the pinned group;
  - `current.json`, `current.png`, `avatar.png` are `0640` and
    group-owned by the pinned group;
  - `S_IROTH | S_IWOTH | S_IXOTH` bits are strictly zero on every
    file (belt-and-braces vs an accidental `== 0644` mask edit);
  - the group-lookup-failure fallback still writes `0640` (never
    widens to `0644`).
  Live-rig mode via `NH_GREETER_PERMS_DIR` / `NH_GREETER_PERMS_GROUP`
  env vars so an operator can point the test at `/run/nostr-auth/greeter`
  after installing the broker.

## Verification on `gnome-dev`

```
### greeter dir posture (broker restart, no publish yet)
drwxr-x--- root:nostr-auth-greeter /run/nostr-auth/greeter/

### after synthetic publish (test_greeter_artifact_perms live-rig mode)
750 root:nostr-auth-greeter /run/nostr-auth/greeter
640 root:nostr-auth-greeter /run/nostr-auth/greeter/current.json
640 root:nostr-auth-greeter /run/nostr-auth/greeter/current.png
640 root:nostr-auth-greeter /run/nostr-auth/greeter/avatar.png

### group membership
nostr-auth-greeter:x:983:gdm,nh_allowtest
uid=124(gdm) gid=125(gdm) groups=125(gdm),983(nostr-auth-greeter)

### DENY — non-member local user
$ sudo -u nh_denytest cat /run/nostr-auth/greeter/current.json
cat: /run/nostr-auth/greeter/current.json: Permission denied
$ sudo -u nh_denytest ls /run/nostr-auth/greeter/
ls: cannot open directory '/run/nostr-auth/greeter/': Permission denied

### ALLOW — supplementary-group member
$ sudo -u nh_allowtest head -c 220 /run/nostr-auth/greeter/current.json
{"tx_id":"tx-perm","png":"current.png","uri":"nostrconnect://deadbeef?relay=
wss%3A%2F%2Fnos.lol&secret=cafef00dcafef00dcafef00dcafef00d&name=GNOME",
"pairing_code":"A1B2-C3D4","hint":"Scan with your Nostr signer","expires
```

Steps executed:

1. `rsync -a` worktree → `/tmp/greeter-secret-src` on `gnome-dev`.
2. `cmake -S /tmp/greeter-secret-src -B /tmp/greeter-secret-build
   -DLIBNOSTR_WITH_NOSTRDB=OFF -DENABLE_NOSTR_HOMED=ON
   -DNOSTR_HOMED_ENABLE_AUTH_INSTALL=ON -DNOSTR_HOMED_BUILD_TESTS=ON
   -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=RelWithDebInfo
   -GNinja`.
3. `ninja nostr-authd test_greeter_artifact_perms test_broker_login_nip46_qr
   test_auth_conf` — clean.
4. `ctest -L auth-runtime -j4 --output-on-failure` — 9/10 green.
   `homed_nip05` fails on the "plain url" URL-builder assertion
   (`nh_nip05_wellknown_url` in `src/nip05/nip05_validate.c`); it is
   **pre-existing on the branch this fix builds against** and
   unrelated to the confidentiality change (that file is untouched).
5. Installed the new broker + unit + sysusers snippet:

   ```
   sudo systemctl stop nostr-authd
   sudo cp .../nostr-authd /usr/sbin/nostr-authd
   sudo cp .../nostr-authd.service /usr/lib/systemd/system/
   sudo install -m 0644 -D .../nostr-auth-greeter.conf \
     /usr/lib/sysusers.d/nostr-auth-greeter.conf
   sudo systemd-sysusers
   getent group nostr-auth-greeter  # → nostr-auth-greeter:x:983:
   sudo systemctl daemon-reload
   sudo systemctl start nostr-authd
   sudo gpasswd -a gdm nostr-auth-greeter
   ```

6. Live-rig headless test — `test_greeter_artifact_perms` pointed at
   `/run/nostr-auth/greeter` with `NH_GREETER_PERMS_GROUP=nostr-auth-greeter`,
   `NH_GREETER_PERMS_KEEP=1`. `ok test_greeter_artifact_perms`, followed
   by `stat`/`ls` proving the modes above.
7. Deny/allow permission check — created `nh_denytest` (no supplementary
   groups) and `nh_allowtest` (member of `nostr-auth-greeter`). `cat`
   denied for the former, succeeds for the latter. Both throwaway
   accounts were `userdel`'d after the check.

## Not verified live

- Extension render screenshot: `gdm-screenshot` refuses on this rig
  (no active graphical session under gdm; `gnome-shell --gdm-mode` not
  running in a listable D-Bus namespace). The extension's read path is
  a stat + `load_contents` against `/run/nostr-auth/greeter/`, both of
  which are pure POSIX perm checks — a `gdm` user with the new
  supplementary group has read+traverse on the drop by construction
  (see the `id gdm` / `stat` output above), so the render path is
  posture-covered rather than pixel-covered. The extension-agent's
  own work under `greeter-extension/` is coordinated via the contract
  in `greeter-extension/README.md` (updated in this changeset).
- Restart of gdm — deliberately skipped per task constraint.

## Rollout notes for packaging

1. **debian postinst / rpm %post** must `getent group nostr-auth-greeter
   || sysusers` (the shipped `sysusers.d(5)` snippet already handles
   the first-boot case; postinst needs to trigger it if the package
   installs after `systemd-sysusers.service`'s last run).
2. **postinst** must also `gpasswd -a gdm nostr-auth-greeter` (Debian/
   Ubuntu) or `usermod -a -G nostr-auth-greeter gdm` (Fedora). The
   group takes effect on the next `systemctl restart gdm` — packagers
   should NOT restart gdm from postinst (it would kick out live
   sessions); the change goes live on the next reboot.
3. **`unlock-dialog` path** — dropping the shipped
   `${docdir}/examples/pam/nostr-auth-greeter.group.conf.sample` into
   `/etc/security/group.conf.d/50-nostr-auth-greeter.conf` is a
   packaging option (recommended for single-user desktops, tighten
   the `users` field for multi-user hosts).

## Follow-up beads

- Track packaging integration (postinst / %post lines above) as a
  follow-up under `nostrc-rb0e` (packaging epic).
- No further code follow-up: the fix is self-contained inside
  `gnome/nostr-homed/`, covered by a headless test, and the sysusers +
  pam sample ship alongside the broker.
