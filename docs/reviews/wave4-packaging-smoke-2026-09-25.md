# Wave 4 packaging smoke report — 2026-09-25

**Scope**: acceptance for Execution-Index item **#22c** — installed-unit
smoke matrix on both distros (Ubuntu 24.04 arm64 lab at
`bizarro@192.168.64.3` + Fedora 40/41).  Sister items #22a (Debian
packaging) and #22b (Fedora packaging) landed on `feat/packaging-wave4`;
this report captures the acceptance run.

Follows the acceptance list in
`docs/plans/gnome-integration-and-samba-server-2026-09-25.md` §5.2 +
§5.3 (packaging changes / live smoke lab).  Filed as a **PREPARED**
smoke report — the packaging landed, and this document is the runbook
+ evidence template the operator fills in on the aarch64 lab and on
a Fedora VM to close #22c.

---

## 0. Prerequisites

Both lab environments MUST be reset to a known state before running
the matrix; a residual `nostr-authd` seed or leftover Samba passdb
will mask real regressions.

```bash
# Ubuntu 24.04 arm64  (bizarro@192.168.64.3)
sudo systemctl --user stop nostr-session-relay.socket nostr-session-relay.service \
                            nostr-notify.service nostr-dav.service 2>/dev/null || true
sudo systemctl stop nostr-authd.service nostr-smbd.service 2>/dev/null || true
sudo apt purge -y \
    nostrc-samba-server nostr-dav nostr-notify nostrc-session-relay \
    nostr-authd libpam-nostr libnss-nostr nostr-homectl nostr-homed-smb \
    nostr-home-fuse nostr-home-sync libhanami0 libnostr-json1 \
    libnostrgo0 libnostr1 nostr-login 2>/dev/null || true
sudo rm -rf /var/lib/nostr-auth /etc/nostr-auth /run/nostr-auth
```

Fedora 40/41 mirror (adjust `dnf remove` accordingly).

---

## 1. Build + install

### 1.1 Debian (Ubuntu 24.04 arm64)

```bash
cd /path/to/nostrc-wave4
dpkg-buildpackage -us -uc -b -j4       # #22a acceptance
sudo dpkg -i ../*.deb
sudo apt-get -f install                # pull in libsoup-3, samba, etc.
```

- [ ] Build succeeds; no unresolved shlibs.
- [ ] `lintian` clean or every emitted tag documented in a
      `debian/*.lintian-overrides` file.
- [ ] `dh_missing` silent (no "not installed" warnings for Wave-4 units).

### 1.2 Fedora 40 or 41

```bash
rpmbuild -bs packaging/rpm/nostr-login.spec
mock -r fedora-41-aarch64 --rebuild ~/rpmbuild/SRPMS/nostrc-0.4.0-1.*.src.rpm
sudo dnf install ~/rpmbuild/RPMS/**/*.rpm
```

- [ ] `rpmbuild -bs` succeeds.
- [ ] `%check` passes — the pinned dep-purity gate exits 0 against
      the installed `%{buildroot}/usr/sbin/nostr-authd` +
      `%{buildroot}/usr/lib64/security/pam_nostr.so`.
- [ ] `rpmlint packaging/rpm/nostr-login.spec ~/rpmbuild/RPMS/**/*.rpm`
      clean or waivers documented in `packaging/rpm/rpmlint.toml`.

---

## 2. Dep-purity gate on the INSTALLED artifacts (#21b, re-run)

The `%check` block runs the gate against `%{buildroot}` at build
time; #22c re-runs it against `/usr/sbin/nostr-authd` +
`/usr/lib/*/security/pam_nostr.so` **after** installation, because
strip / RPATH-elision / dpkg-shlibdeps can change the closure a
final time.

```bash
sudo scripts/check-authd-dep-purity.sh \
    /usr/sbin/nostr-authd \
    /usr/lib/*/security/pam_nostr.so
echo "dep-purity exit = $?"
```

- [ ] Exit code 0 on Ubuntu 24.04 arm64.
- [ ] Exit code 0 on Fedora 40/41.
- [ ] No new SONAME on the `AUTHD_ALLOWED_SONAMES` list.
- [ ] No symbol matching the forbidden regex (hanami / porthome /
      nip55l-client / FUSE).

---

## 3. Unit acceptance matrix

Verify each new unit boots + does the one thing it exists to do.
Each row is a hard gate — a failure blocks the Wave-4 packaging tag.

### 3.1 `nostr-authd` + `pam_nostr.so`

- [ ] `sudo systemctl status nostr-authd` reports the daemon inert
      (correct — install must not auto-enable).
- [ ] `sudo nostr-homed-seed /var/lib/nostr-auth n_alice '<pass>'`
      succeeds.
- [ ] `sudo systemctl start nostr-authd` → active + `/run/nostr-auth/auth.sock`
      exists at mode 0666, owned by root:nostr-auth-greeter (per
      `docs/AUTH_PROTOCOL.md` and beads nostrc-o1ho).
- [ ] `printf 'pass\n' | sudo pamtester -v nostr-login n_alice authenticate`
      succeeds; `su - n_alice` succeeds after wiring pam-nostr into
      common-auth (Debian) or authselect custom/nostr (Fedora).

### 3.2 `nostr-session-relay.socket` (per §3.2)

- [ ] `systemctl --user enable --now nostr-session-relay.socket`
      succeeds.
- [ ] `stat $XDG_RUNTIME_DIR/nostr/relay.sock` shows mode **0600**
      owned by the current UID.
- [ ] `socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/nostr/relay.sock`
      accepts a `["REQ","test",{"limit":1}]` frame and returns EOSE
      without error.
- [ ] `systemctl --user status nostr-session-relay.service` shows
      `Active: active (running)` with a `Type=notify` READY=1
      handshake completed.
- [ ] Peercred admission: an inline test using a `SO_PEERCRED` probe
      from a second UID is refused with `EACCES` /
      pre-read connection close (§3.2 D3 contract).

### 3.3 `nostr-notify.service`

- [ ] `systemctl --user enable --now nostr-notify.service` succeeds.
- [ ] `gdbus introspect --session --dest org.nostr.NotifyDaemon --object-path /`
      lists the daemon's action group (NOT `org.gnostr.Client` — per
      Finding 4).
- [ ] Fixture DM (kind-1059 gift-wrap addressed to the current
      npub) fires a `notify-send`-visible `GNotification`; the body
      is OPAQUE (no sender pubkey leak, per Finding 14).
- [ ] With `nostr-session-relay.socket` down the daemon falls back
      to `home_relays` and still emits the notification (soft dep).

### 3.4 `nostr-dav.service`

- [ ] `systemctl --user enable --now nostr-dav.service` succeeds.
- [ ] `ss -lntp | grep 127.0.0.1:7680` reports the DAV listener on
      loopback (NOT wildcard).
- [ ] `nostr-dav`'s WebSocket relay transport (bead tu6y) is
      enabled by default: `journalctl --user -u nostr-dav -e | grep
      -i websocket` shows the ws:// dial at startup.
- [ ] Token bootstrap: with `gnostr-signer-daemon` present, the
      first `PROPFIND /calendars/` from `curl` returns 401 → then
      after the signer approves the token request, the token file
      appears under `$XDG_STATE_HOME/nostr-dav/token` at mode 0600
      and the same PROPFIND returns a valid multi-status.

### 3.5 `nostr-smbd.service`

- [ ] `sudo systemctl status nostr-smbd` reports the service inert
      after install (per plan §5.2 — DO NOT auto-enable).
- [ ] `testparm -s /etc/nostr-auth/smb.conf` parses clean.
- [ ] Manually seed one share via
      `sudo nostr-smb-acquire --share=home` and start with
      `sudo systemctl start nostr-smbd`.
- [ ] `smbclient -L 127.0.0.1 -U n_alice%<smb-pw>` lists the
      seeded share.
- [ ] Sample password change is journalled to
      `/var/lib/nostr-auth/smb.db` (issuance journal, not the
      password).

---

## 4. Smoke matrix (§5.3)

The plan's cross-cutting matrix — one script per component, results
appended below.

- [ ] **Signer** — `StoreKey` → `SignEvent` → `GetRelays` round-trip
      against `gnostr-signer-daemon` on the private bus succeeds.
      SignEvent returns the complete signed JSON (nip55l 0.2.0
      contract, plan §1.2 D1.a).
- [ ] **nostr-dav** — token bootstrap + `curl -X PROPFIND
      http://127.0.0.1:7680/calendars/` returns valid multi-status.
- [ ] **Session relay** — peercred + notifier fixture DM (§3.3
      above) end-to-end.
- [ ] **nostr-smb-mount** — `nostr-smb-mount --batch` against the
      lab Samba VM mounts + unmounts cleanly; `close-session` unmount
      ledger is intact.
- [ ] **NSS ordering** — `getent passwd 'DOMAIN\user'` behaves per
      §4.3 C2 (nostr NOTFOUND → winbind if joined, else documented
      skip).

---

## 5. Purge round-trip

```bash
# Debian
sudo apt purge -y nostrc-samba-server nostr-dav nostr-notify nostrc-session-relay
sudo find /etc/nostr-auth /var/lib/nostr-auth /run/nostr-auth 2>/dev/null
```

- [ ] No orphan files under `/etc/nostr-auth/`, `/var/lib/nostr-auth/`,
      `/run/nostr-auth/` after purge → reinstall → purge.
- [ ] Purge of the samba subpackage does NOT delete
      `/var/lib/nostr-auth/smbpasswd` (contains operator state; move
      out first if a wipe is desired).

Fedora `dnf remove` equivalent.

---

## 6. Results

| Section | Ubuntu 24.04 arm64 | Fedora 40 | Fedora 41 |
|---|---|---|---|
| 1. Build + install | ⏳ | ⏳ | ⏳ |
| 2. Dep-purity (installed) | ⏳ | ⏳ | ⏳ |
| 3.1 nostr-authd + pam_nostr | ⏳ | ⏳ | ⏳ |
| 3.2 session-relay socket | ⏳ | ⏳ | ⏳ |
| 3.3 nostr-notify | ⏳ | ⏳ | ⏳ |
| 3.4 nostr-dav | ⏳ | ⏳ | ⏳ |
| 3.5 nostr-smbd | ⏳ | ⏳ | ⏳ |
| 4. Smoke matrix | ⏳ | ⏳ | ⏳ |
| 5. Purge round-trip | ⏳ | ⏳ | ⏳ |

Fill in `✅` / `❌` / `⚠️` and paste evidence excerpts (journal
snippets, ss output, sha256 sums) inline as each row runs.  Any
`⚠️` needs a companion `bd` issue with the concrete symptom + repro.

---

## 7. Sign-off

- Packaging author: chebizarro@gmail.com (`feat/packaging-wave4`)
- Reviewer / operator: _pending_
- Date passed: _pending_

Once every row is green, close beads `nostrc-*` for #22c and land
the merge to `master`.  A failure in any row keeps the branch on
`feat/packaging-wave4` and files a fresh bead per component.
