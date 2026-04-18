# nostr-homed systemd & D-Bus Topology

## Decision: User Bus Everywhere (Option B)

All D-Bus communication uses the **user/session bus**. There is no
system-bus name for nostr-homed.

## Bus Layout

```
┌─ user session (after pam_systemd) ────────────────────────┐
│                                                            │
│  org.nostr.Signer      (gnostr-signer, user service)       │
│  org.nostr.Homed1      (nostr-homectl --daemon, user svc)  │
│                                                            │
│  nostrfs@<user>.service  (FUSE mount, user service)        │
└────────────────────────────────────────────────────────────┘
```

## PAM Call Graph

PAM modules run in the context of the login daemon (sshd, gdm, login),
which is a **system** process. The user's session bus does not exist
until `pam_systemd` sets up the logind session.

### pam_sm_authenticate (runs BEFORE session bus exists)

- **No D-Bus calls.** The session bus is not available.
- Performs a lightweight cache-based check: verify the username exists
  in the nostr-homed SQLite cache (`nh_cache_lookup_name`).
- Returns `PAM_SUCCESS` if the user exists, `PAM_USER_UNKNOWN` if not.
- Actual cryptographic identity verification is deferred to
  `pam_sm_open_session` where the user bus is available.

### pam_sm_open_session (runs AFTER pam_systemd)

1. Sets environment variables (HOME, SHELL, XDG_RUNTIME_DIR, etc.)
   using the looked-up UID from the cache.
2. Connects to the **session bus** (now available via pam_systemd).
3. Calls `org.nostr.Homed1.OpenSession(user)` to trigger FUSE mount
   and cache warm.
4. (Future: NIP-46 challenge/response via `org.nostr.Signer.SignEvent`
   for cryptographic identity proof — see nostrc-diz6.)

### pam_sm_close_session

- Calls `org.nostr.Homed1.CloseSession(user)` on the session bus.
- Best-effort; returns PAM_SUCCESS even on failure.

## Service Units

### nostr-homectl (user service)

- Lives in `systemd/user/nostr-homectl.service`
- `WantedBy=default.target` (starts with user session)
- Owns `org.nostr.Homed1` on the user bus

### nostrfs@ (user service template)

- Lives in `systemd/user/nostrfs@.service`
- `BindsTo=nostr-homectl.service` (lifecycle tied to homectl)
- `WantedBy=default.target`
- Requires `CAP_SYS_ADMIN` for FUSE mount

## Limitations

- **sshd without logind**: If the target system does not run
  systemd-logind (e.g., a container or minimal install), there is no
  session bus. `pam_sm_open_session` will fail gracefully and log a
  warning. The user can still log in but will not get a FUSE mount.
- **Root SSH**: Root does not typically have a user bus session.
  nostr-homed is designed for regular users backed by nostr identities.

## File Locations

| Component        | Path                                    |
|------------------|-----------------------------------------|
| homectl service  | `systemd/user/nostr-homectl.service`    |
| nostrfs template | `systemd/user/nostrfs@.service`         |
| PAM module       | `src/pam/pam_nostr.c`                   |
| D-Bus interface  | `dbus/org.nostr.Homed1.xml`             |
