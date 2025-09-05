# Systemd Hardening Guide for Nostr Relay

This guide provides a hardened yet practical systemd service template for running the relay as a long-lived, non-privileged background service.

## Principles
- Least privilege: no capabilities unless strictly needed.
- Strong sandboxing: isolate FS, devices, kernel tunables, namespaces.
- Predictable runtime: explicit limits for files, procs, core dumps.
- Operational sanity: rotate logs in journald or use `LogsDirectory=`.

## Recommended Unit Snippet

Place this in `/etc/systemd/system/nostr-relay.service` (adapt `ExecStart` and paths):

```ini
[Unit]
Description=Nostr Relay
After=network-online.target
Wants=network-online.target

[Service]
User=nostr
Group=nostr

# Capabilities — prefer no capabilities (bind to high ports >=1024)
CapabilityBoundingSet=
AmbientCapabilities=
# If binding to privileged ports (<1024), enable ONLY this pair:
#CapabilityBoundingSet=CAP_NET_BIND_SERVICE
#AmbientCapabilities=CAP_NET_BIND_SERVICE

# Hardening (balanced defaults)
NoNewPrivileges=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectSystem=strict
ProtectHome=yes
ProtectClock=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
LockPersonality=yes
RestrictSUIDSGID=yes
RestrictNamespaces=yes
MemoryDenyWriteExecute=yes
SystemCallArchitectures=native
# Optional (after auditing syscalls under load):
#SystemCallFilter=@system-service @ptrace-allowlist <add-precise-calls>

# Files & limits
UMask=0077
StateDirectory=nostr
CacheDirectory=nostr
LogsDirectory=nostr
ReadWritePaths=/var/lib/nostr
LimitNOFILE=65536
LimitNPROC=4096
LimitCORE=0

# Environment — security tunables (examples)
#Environment=NOSTR_WS_READ_TIMEOUT_SECONDS=60
#Environment=NOSTR_WS_PROGRESS_WINDOW_MS=5000
#Environment=NOSTR_WS_MIN_BYTES_PER_WINDOW=256
#Environment=NOSTR_MAX_FRAME_LEN_BYTES=2097152
#Environment=NOSTR_MAX_FRAMES_PER_SEC=100
#Environment=NOSTR_MAX_BYTES_PER_SEC=2097152
#Environment=NOSTR_MAX_EVENT_SIZE_BYTES=262144
#Environment=NOSTR_MAX_TAGS_PER_EVENT=100
#Environment=NOSTR_MAX_TAG_DEPTH=4
#Environment=NOSTR_MAX_FILTERS_PER_REQ=20
#Environment=NOSTR_MAX_IDS_PER_FILTER=500

# Exec
ExecStart=/usr/local/bin/nostr-relay --config /etc/nostr/relay.conf
Restart=always
RestartSec=2s

[Install]
WantedBy=multi-user.target
```

## Notes
- Run as a dedicated, non-privileged user.
- Prefer ports ≥1024 to avoid capabilities entirely.
- Add a syscall allowlist once you’ve captured the set under production load; start in log/audit mode and then enforce.
- If you need to drop privileges post-bind, it’s cleaner to let systemd handle `User=` and (optionally) CAP_NET_BIND_SERVICE rather than handling `setuid` in-process.
