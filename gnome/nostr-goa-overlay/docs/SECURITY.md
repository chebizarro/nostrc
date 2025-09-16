# Security (Nostr GOA Overlay)

This overlay installs a user-scoped `goa-daemon` and the Nostr provider entirely under `~/.local`. No system files are modified.

- Sandboxing: user systemd units (`nostr-router`, `nostr-dav`, `nostrfs`, `nostr-notify`) use strict hardening:
  - NoNewPrivileges=yes, ProtectSystem=strict, ProtectHome=yes, MemoryDenyWriteExecute=yes, RestrictSUIDSGID=yes, RestrictNamespaces=yes, SystemCallArchitectures=native, SystemCallFilter=@system-service, UMask=0077.
- Signer trust: the provider communicates with the local signer `org.nostr.Signer` on the session bus. Ensure the signer service is local and trusted.
- Secrets: the provider stores only a public identifier (npub) as the account identity. Private keys should never be persisted by the provider.
- DAV endpoints: the CalDAV/CardDAV bridge listens on 127.0.0.1:7680 and is intended for local access only.
- Filesystem: `nostrfs` mounts under `~/Nostr`. See `gnome/nostr-homed/docs/SECURITY.md` for FUSE hardening details used elsewhere in the repo.
