# Gnostr Signer Daemon - Deployment Guide

## Overview

The gnostr-signer-daemon provides secure Nostr key management and signing services through multiple interfaces:
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
    │  gnostr-signer  │
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
cd apps/gnostr-signer
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
systemctl --user enable gnostr-signer-daemon.service
systemctl --user start gnostr-signer-daemon.service

# Check status
systemctl --user status gnostr-signer-daemon.service
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
systemctl --user edit gnostr-signer-daemon.service
```

Example override:

```ini
[Service]
Environment=NOSTR_SIGNER_ENDPOINT=unix:%t/gnostr/custom.sock
Environment=G_MESSAGES_DEBUG=all
```

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
journalctl --user -u gnostr-signer-daemon.service -f
```

## Monitoring

### Health Checks

Check if the daemon is running:

```bash
# Via systemd
systemctl --user is-active gnostr-signer-daemon.service

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
   journalctl --user -u gnostr-signer-daemon.service -n 50
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
   systemctl --user status gnostr-signer-daemon.service
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
   journalctl --user -u gnostr-signer-daemon.service --since "1 hour ago"
   ```

3. Restart the daemon:
   ```bash
   systemctl --user restart gnostr-signer-daemon.service
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
   systemctl --user stop gnostr-signer-daemon.service
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
   systemctl --user start gnostr-signer-daemon.service
   ```

6. Verify operation:
   ```bash
   systemctl --user status gnostr-signer-daemon.service
   ```

## Development

### Running in Debug Mode

```bash
# Stop the system service
systemctl --user stop gnostr-signer-daemon.service

# Run manually with debug output
G_MESSAGES_DEBUG=all ./gnostr-signer-daemon
```

### Testing IPC Endpoints

#### Unix Socket Test

```bash
# Start daemon with custom socket
NOSTR_SIGNER_ENDPOINT=unix:/tmp/test-signer.sock ./gnostr-signer-daemon &

# Test connection
echo '{"method":"get_public_key"}' | nc -U /tmp/test-signer.sock
```

#### TCP Socket Test

```bash
# Start daemon with TCP (requires ENABLE_TCP_IPC=ON)
NOSTR_SIGNER_ENDPOINT=tcp:127.0.0.1:5897 ./gnostr-signer-daemon &

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
   journalctl --user -u gnostr-signer-daemon.service -f
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
- Documentation: https://github.com/chebizarro/nostrc/tree/main/apps/gnostr-signer
- Matrix Channel: (TBD)
