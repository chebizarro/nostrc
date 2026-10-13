# Grotto Daemon - Deployment Guide

## Overview

The grotto-daemon provides secure Nostr key management and signing services through multiple interfaces:
- **D-Bus**: System integration for desktop applications
- **Unix Domain Sockets**: High-performance local IPC
- **TCP (optional)**: Network-based access with token authentication

## Architecture

```
┌─────────────────────────────────────────┐
│         Client Applications             │
│  (Nostr clients, browser extensions)    │
└────────────┬────────────────────────────┘
             │
    ┌────────┴────────┐
    │                 │
┌───▼────┐      ┌────▼─────┐
│ D-Bus  │      │   Unix   │
│        │      │  Socket  │
└───┬────┘      └────┬─────┘
    │                │
    └────────┬───────┘
             │
    ┌────────▼────────┐
    │  grotto  │
    │     daemon      │
    │                 │
    │  • Key Storage  │
    │  • Signing      │
    │  • Encryption   │
    └─────────────────┘
```

## Installation

### From Source

```bash
cd apps/grotto
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TCP_IPC=ON
make
sudo make install
```

### Post-Installation

```bash
# Reload systemd user daemon
systemctl --user daemon-reload

# Enable and start the daemon
systemctl --user enable grotto-daemon.service
systemctl --user start grotto-daemon.service

# Check status
systemctl --user status grotto-daemon.service
```

## Configuration

### Environment Variables

The daemon can be configured using environment variables:

| Variable | Description | Default |
|----------|-------------|---------|
| `NOSTR_SIGNER_ENDPOINT` | IPC endpoint specification | `unix:$XDG_RUNTIME_DIR/gnostr/signer.sock` |
| `NOSTR_SIGNER_MAX_CONNECTIONS` | Maximum concurrent TCP connections | `100` |
| `NOSTR_DEBUG` | Enable debug logging | (unset) |
| `G_MESSAGES_DEBUG` | GLib debug domains | (unset) |

### Endpoint Configuration

#### Unix Domain Socket (Recommended)

```bash
export NOSTR_SIGNER_ENDPOINT="unix:/run/user/$(id -u)/gnostr/signer.sock"
```

#### TCP Socket (Development Only)

```bash
export NOSTR_SIGNER_ENDPOINT="tcp:127.0.0.1:5897"
```

**Security Note**: TCP mode requires authentication via a token file stored at `$XDG_RUNTIME_DIR/gnostr/token`. Only use TCP for development or when Unix sockets are not available.

### Systemd Service Configuration

Edit the user service file to customize the daemon:

```bash
systemctl --user edit grotto-daemon.service
```

Example override:

```ini
[Service]
Environment=NOSTR_SIGNER_ENDPOINT=unix:%t/gnostr/custom.sock
Environment=G_MESSAGES_DEBUG=all
```

## Remote signing and headless deployment (G05)

`grotto-daemon --bunker` uses a persistent **transport credential**, not an
account key, to receive NIP-46 envelopes. Its public key names the endpoint;
`get_public_key` and content cryptography use the authenticated client's pinned
account through the same scoped approval gate as local applications.

Configure `org.nostr.Grotto` GSettings explicitly: `bunker-enabled`,
`bunker-relays`, `bunker-allowed-methods`, and `default-identity`. There are no
fallback relays. Only `wss://` or loopback `ws://` are accepted; Tor is not
supported. `bunker-allowed-pubkeys` is legacy **admission inventory**, not a
pairing or signing grant; empty admits no new clients. Global
`bunker-auto-approve-kinds` and GUI client sessions are not signing authority.

The credential lives in Secret Service schema
`org.nostr.Grotto.BunkerTransport`, attribute `slot=default`, or Keychain service
`Grotto Bunker Transport`, account `default`. It is never an identity-list item.
A missing/unavailable credential produces `ERROR`, never automatic regeneration.
Control1 `ResetBunkerEndpoint` requires the Grotto approver check; on PID-attested
Linux an ordinary shell `gdbus` caller is not an approver. UID-only buses retain
the documented same-UID approver exception (see `docs/dbus-interface.md`); do not
claim caller isolation on those buses. Reset stops remote work,
revokes all remote clients/permissions, replaces the transport key and restarts
when enabled. Clients must re-pair. During profile reset/uninstall, the owner
should also remove this distinct credential using their key manager; do not
change the default keychain or its search list.

Runtime is reported by Control1 `GetStatus`: `DISABLED`, `STARTING`,
`LISTENING` (EOSE on every configured relay), `DEGRADED` (some usable relays),
`OFFLINE`, `STOPPING`, or `ERROR`. Opening a websocket alone is not readiness.
CLOSED, AUTH challenges and rejected response publications remove relay
readiness; AUTH is not silently signed. Journal state/error messages are
sanitised. Pairing tokens, `nostrconnect://` acknowledgement, camera scanning and
live `ConfigureBunker` are G06, not shipped setup flows in G05.

For unattended service use **`--bunker --no-approval-ui`**, with
`headless-bunker.conf.example` installed as a user-service drop-in (examples are
under `share/doc/grotto/examples`). The explicit `ExecStart` override matters;
relaxing the IP filter alone does not enable bunker/headless mode. Already
paired clients operate within their scopes; anything requiring review fails
promptly with `org.nostr.Signer.Error.NoApprovalAgent` (`NO_APPROVER`). This mode
also suppresses approval UI activation for local/socket calls; existing scoped
and first-party grants continue to work.

Provision the transport credential and explicit client consent before enabling
headless mode. The daemon-owned, versioned files are:

- `$XDG_CONFIG_HOME/grotto/clients.ini` v1: `[meta] version=1`, one section named
  SHA-256 of `nip46:<lowercase-client-hex>`, containing `principal`,
  `transport=NIP46`, `identity=<canonical account npub>`, `label`, `claimed-name`,
  `tier` (0 Low / 1 Medium / 2 Full), `paired=true`, `relays` (GKeyFile string
  list), `first-consented-at` and `last-used-at` (Unix seconds). A permission-only
  record without explicit first consent and paired admission cannot connect
  unattended. Offline operator provisioning is an explicit consent action, not
  a migration from an allowlist or from client-supplied permission hints.
- `$XDG_CONFIG_HOME/gnostr/signer-permissions-v2.ini`: scoped principal/account/
  operation/event-kind decisions with absolute expiry; use Control1 permission
  management (or the canonical `SignerPermissions` writer), not broad legacy
  event allows. Encryption/decryption are separate grants; pairing is not one.
- `$XDG_CONFIG_HOME/grotto/bunker-seen.ini` v1: bounded replay metadata only
  (event IDs, hashed RPC identities/parameter digests, expiry). Written durably
  **before** execution. Never delete it on an ordinary restart. A corrupt or
  unwritable guard fails closed; only authenticated endpoint rotation may clear
  it after revoking pairings. Recent response bodies stay in memory, are wiped
  on eviction, expire after ten minutes and are capped at 1,024 entries.

Keep directories private (0700) and files private (0600). Future file versions
are not silently accepted. The E2E test's `provision_headless()` demonstrates
canonical file writers with disposable keys; the test-only file credential
backend requires `NOSTRC_TEST_ISOLATION` and is not a production backend.

Pause/revoke, deadlines, explicit client cancellation, disable/reconfiguration
and shutdown cancel outstanding gate continuations. Replayed old successes
cannot bypass current pairing, pause, permission revision/expiry or account
binding. Clients should retry with fresh RPC IDs after a policy change; identical
pending arguments share the common gate's review, never create a new grant.

## Security

### Hardening Features

The daemon implements multiple security layers:

1. **Process Isolation**
   - Core dumps disabled
   - No new privileges
   - Private /tmp
   - Restricted system calls

2. **File System Protection**
   - Socket files: 0600 permissions
   - Config directory: 0700 permissions
   - Read-only system directories

3. **Network Restrictions**
   - Loopback-only binding for TCP
   - Token-based authentication
   - Connection limits

4. **Memory Protection**
   - Write-execute memory denied
   - Personality locked
   - ASLR enabled

### Key Storage

Private keys are stored securely using:
- **Linux**: libsecret (GNOME Keyring, KWallet)
- **macOS**: Keychain
- **Windows**: DPAPI

### Audit Logging

The daemon logs all security-relevant events:
- Connection attempts
- Authentication failures
- Signing requests
- Key operations

View logs:
```bash
journalctl --user -u grotto-daemon.service -f
```

## Monitoring

### Health Checks

Check if the daemon is running:

```bash
# Via systemd
systemctl --user is-active grotto-daemon.service

# Via D-Bus
dbus-send --session --print-reply \
  --dest=org.nostr.Signer \
  /org/nostr/signer \
  org.freedesktop.DBus.Introspectable.Introspect

# Via Unix socket
echo "test" | nc -U $XDG_RUNTIME_DIR/gnostr/signer.sock
```

### Statistics

The daemon tracks and logs statistics on shutdown:
- Total connections
- Active connections
- Total requests
- Error count
- Uptime

### Performance Tuning

Adjust resource limits in the systemd service:

```ini
[Service]
LimitNOFILE=8192
TasksMax=64
```

## Troubleshooting

### Daemon Won't Start

1. Check logs:
   ```bash
   journalctl --user -u grotto-daemon.service -n 50
   ```

2. Verify D-Bus availability:
   ```bash
   echo $DBUS_SESSION_BUS_ADDRESS
   ```

3. Check socket directory permissions:
   ```bash
   ls -la $XDG_RUNTIME_DIR/gnostr/
   ```

### Connection Refused

1. Verify the daemon is running:
   ```bash
   systemctl --user status grotto-daemon.service
   ```

2. Check the socket file exists:
   ```bash
   ls -l $XDG_RUNTIME_DIR/gnostr/signer.sock
   ```

3. Verify permissions:
   ```bash
   stat -c "%a %U:%G" $XDG_RUNTIME_DIR/gnostr/signer.sock
   ```

### High CPU Usage

1. Check for connection leaks:
   ```bash
   lsof -U | grep gnostr
   ```

2. Review recent logs for errors:
   ```bash
   journalctl --user -u grotto-daemon.service --since "1 hour ago"
   ```

3. Restart the daemon:
   ```bash
   systemctl --user restart grotto-daemon.service
   ```

### Permission Denied

1. Ensure you're in the correct user session:
   ```bash
   echo $XDG_RUNTIME_DIR
   ```

2. Check SELinux/AppArmor policies:
   ```bash
   # SELinux
   ausearch -m avc -ts recent | grep gnostr

   # AppArmor
   sudo aa-status | grep gnostr
   ```

## Upgrading

### Safe Upgrade Procedure

1. Stop the daemon:
   ```bash
   systemctl --user stop grotto-daemon.service
   ```

2. Backup configuration:
   ```bash
   cp -r ~/.config/gnostr ~/.config/gnostr.backup
   ```

3. Install new version:
   ```bash
   sudo make install
   ```

4. Reload systemd:
   ```bash
   systemctl --user daemon-reload
   ```

5. Start the daemon:
   ```bash
   systemctl --user start grotto-daemon.service
   ```

6. Verify operation:
   ```bash
   systemctl --user status grotto-daemon.service
   ```

## Development

### Running in Debug Mode

```bash
# Stop the system service
systemctl --user stop grotto-daemon.service

# Run manually with debug output
G_MESSAGES_DEBUG=all ./grotto-daemon
```

### Testing IPC Endpoints

#### Unix Socket Test

```bash
# Start daemon with custom socket
NOSTR_SIGNER_ENDPOINT=unix:/tmp/test-signer.sock ./grotto-daemon &

# Test connection
echo '{"method":"get_public_key"}' | nc -U /tmp/test-signer.sock
```

#### TCP Socket Test

```bash
# Start daemon with TCP (requires ENABLE_TCP_IPC=ON)
NOSTR_SIGNER_ENDPOINT=tcp:127.0.0.1:5897 ./grotto-daemon &

# Get auth token
cat $XDG_RUNTIME_DIR/gnostr/token

# Test connection
echo "AUTH <token>" | nc localhost 5897
```

## Production Deployment

### Recommended Configuration

```ini
[Service]
Type=dbus
BusName=org.nostr.Signer
Environment=NOSTR_SIGNER_ENDPOINT=unix:%t/gnostr/signer.sock
Restart=on-failure
RestartSec=5s

# Resource limits
LimitNOFILE=4096
LimitCORE=0
TasksMax=32

# Full security hardening enabled
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=read-only
```

### Monitoring Setup

1. Enable persistent logging:
   ```bash
   sudo mkdir -p /var/log/journal
   sudo systemd-tmpfiles --create --prefix /var/log/journal
   ```

2. Set up log rotation in systemd:
   ```bash
   sudo systemctl edit systemd-journald.service
   ```

   Add:
   ```ini
   [Journal]
   SystemMaxUse=500M
   SystemMaxFileSize=50M
   ```

3. Monitor with journalctl:
   ```bash
   journalctl --user -u grotto-daemon.service -f
   ```

## Best Practices

1. **Always use Unix sockets in production** - Better performance and security
2. **Enable all systemd hardening options** - Defense in depth
3. **Monitor logs regularly** - Detect issues early
4. **Keep the daemon updated** - Security patches
5. **Backup keys regularly** - Disaster recovery
6. **Test upgrades in staging** - Avoid production issues
7. **Use systemd activation** - Automatic start on demand
8. **Limit resource usage** - Prevent DoS
9. **Enable audit logging** - Compliance and forensics
10. **Document your configuration** - Team knowledge

## Support

For issues and questions:
- GitHub Issues: https://github.com/chebizarro/nostrc/issues
- Documentation: https://github.com/chebizarro/nostrc/tree/main/apps/grotto
- Matrix Channel: (TBD)
