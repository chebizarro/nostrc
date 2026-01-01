# Gnostr Signer Daemon - Quick Start Guide

## TL;DR

```bash
# Build
cd apps/gnostr-signer && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make

# Install
sudo make install

# Start
systemctl --user enable --now gnostr-signer-daemon.service

# Check status
systemctl --user status gnostr-signer-daemon.service
```

## Common Tasks

### Check if Running

```bash
systemctl --user is-active gnostr-signer-daemon.service
```

### View Logs

```bash
# Follow logs
journalctl --user -u gnostr-signer-daemon.service -f

# Last 50 lines
journalctl --user -u gnostr-signer-daemon.service -n 50

# Since boot
journalctl --user -u gnostr-signer-daemon.service -b
```

### Restart Daemon

```bash
systemctl --user restart gnostr-signer-daemon.service
```

### Stop Daemon

```bash
systemctl --user stop gnostr-signer-daemon.service
```

### Disable Auto-Start

```bash
systemctl --user disable gnostr-signer-daemon.service
```

## Configuration

### Change Socket Path

```bash
systemctl --user edit gnostr-signer-daemon.service
```

Add:
```ini
[Service]
Environment=NOSTR_SIGNER_ENDPOINT=unix:%t/gnostr/custom.sock
```

Then:
```bash
systemctl --user daemon-reload
systemctl --user restart gnostr-signer-daemon.service
```

### Enable Debug Logging

```bash
systemctl --user edit gnostr-signer-daemon.service
```

Add:
```ini
[Service]
Environment=G_MESSAGES_DEBUG=all
```

Then:
```bash
systemctl --user daemon-reload
systemctl --user restart gnostr-signer-daemon.service
```

## Testing

### Test D-Bus Interface

```bash
dbus-send --session --print-reply \
  --dest=org.nostr.Signer \
  /org/nostr/signer \
  org.nostr.Signer.GetPublicKey
```

### Test Unix Socket

```bash
# Check socket exists
ls -l $XDG_RUNTIME_DIR/gnostr/signer.sock

# Test connection (requires NIP-5F client)
# See examples in nips/nip5f/examples/
```

### Run Manually (Debug)

```bash
# Stop service
systemctl --user stop gnostr-signer-daemon.service

# Run with debug output
G_MESSAGES_DEBUG=all /usr/bin/gnostr-signer-daemon

# Press Ctrl+C to stop
```

## Troubleshooting

### Daemon Won't Start

```bash
# Check logs for errors
journalctl --user -u gnostr-signer-daemon.service -n 50

# Verify D-Bus is running
echo $DBUS_SESSION_BUS_ADDRESS

# Check permissions
ls -la $XDG_RUNTIME_DIR/gnostr/
```

### Socket Not Found

```bash
# Check daemon is running
systemctl --user status gnostr-signer-daemon.service

# Verify socket path
systemctl --user show gnostr-signer-daemon.service | grep Environment

# Check runtime directory
echo $XDG_RUNTIME_DIR
```

### Permission Denied

```bash
# Check socket permissions
stat $XDG_RUNTIME_DIR/gnostr/signer.sock

# Verify you're the owner
id -u
stat -c "%u" $XDG_RUNTIME_DIR/gnostr/signer.sock
```

### High CPU/Memory Usage

```bash
# Check resource usage
systemctl --user status gnostr-signer-daemon.service

# View detailed stats
systemd-cgtop

# Restart if needed
systemctl --user restart gnostr-signer-daemon.service
```

## Development

### Build with TCP Support

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TCP_IPC=ON
make
```

### Run Tests

```bash
cd build
ctest --output-on-failure
```

### Debug with GDB

```bash
systemctl --user stop gnostr-signer-daemon.service
gdb --args /usr/bin/gnostr-signer-daemon
(gdb) run
```

### Check for Memory Leaks

```bash
systemctl --user stop gnostr-signer-daemon.service
valgrind --leak-check=full /usr/bin/gnostr-signer-daemon
```

## Environment Variables

| Variable | Purpose | Example |
|----------|---------|---------|
| `NOSTR_SIGNER_ENDPOINT` | IPC endpoint | `unix:/tmp/signer.sock` |
| `NOSTR_SIGNER_MAX_CONNECTIONS` | TCP connection limit | `100` |
| `NOSTR_DEBUG` | Enable debug output | `1` |
| `G_MESSAGES_DEBUG` | GLib debug domains | `all` |

## File Locations

| File | Location |
|------|----------|
| Binary | `/usr/bin/gnostr-signer-daemon` |
| Systemd Service | `~/.config/systemd/user/gnostr-signer-daemon.service` |
| D-Bus Service | `/usr/share/dbus-1/services/org.nostr.Signer.service` |
| Socket | `$XDG_RUNTIME_DIR/gnostr/signer.sock` |
| Config | `~/.config/gnostr/` |
| Logs | `journalctl --user -u gnostr-signer-daemon.service` |

## Common Commands Reference

```bash
# Status and control
systemctl --user status gnostr-signer-daemon.service
systemctl --user start gnostr-signer-daemon.service
systemctl --user stop gnostr-signer-daemon.service
systemctl --user restart gnostr-signer-daemon.service
systemctl --user enable gnostr-signer-daemon.service
systemctl --user disable gnostr-signer-daemon.service

# Logs
journalctl --user -u gnostr-signer-daemon.service -f
journalctl --user -u gnostr-signer-daemon.service -n 100
journalctl --user -u gnostr-signer-daemon.service --since "1 hour ago"

# Configuration
systemctl --user edit gnostr-signer-daemon.service
systemctl --user daemon-reload
systemctl --user show gnostr-signer-daemon.service

# D-Bus
dbus-send --session --print-reply --dest=org.nostr.Signer /org/nostr/signer org.freedesktop.DBus.Introspectable.Introspect
busctl --user introspect org.nostr.Signer /org/nostr/signer

# Debugging
G_MESSAGES_DEBUG=all /usr/bin/gnostr-signer-daemon
gdb --args /usr/bin/gnostr-signer-daemon
valgrind /usr/bin/gnostr-signer-daemon
strace -f /usr/bin/gnostr-signer-daemon
```

## Getting Help

- **Documentation**: See `DAEMON_DEPLOYMENT.md` for detailed information
- **Issues**: https://github.com/chebizarro/nostrc/issues
- **Logs**: Always check logs first with `journalctl`
- **Community**: (Matrix/Discord channel TBD)

## Quick Tips

1. **Always check logs first** when troubleshooting
2. **Use systemd** for production, manual execution for debugging
3. **Unix sockets** are faster and more secure than TCP
4. **Enable debug logging** only when needed (verbose)
5. **Restart after configuration changes**
6. **Check permissions** if you get access denied errors
7. **Monitor resource usage** in production
8. **Keep the daemon updated** for security patches
