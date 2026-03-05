# Signet - NIP-46 Remote Signing Bunker

Signet is a production-ready NIP-46 remote signing server (bunker) that provides secure key custody and policy-based authorization for Nostr applications.

## Features

- **NIP-46 Remote Signing**: Full implementation of the NIP-46 protocol for remote event signing
- **Secure Key Custody**: Integration with HashiCorp Vault for secure key storage
- **Policy-Based Authorization**: Fine-grained access control with wildcard matching
- **Replay Protection**: TTL-based cache prevents replay attacks
- **Comprehensive Audit Logging**: JSONL audit trail for all operations
- **Health Monitoring**: HTTP endpoint for operational monitoring
- **Management CLI**: Command-line tool for policy and key management

## Architecture

### Core Components

1. **NIP-46 Server** (`nip46_server.c`)
   - Handles NIP-46 request events (kind 24133)
   - NIP-04 encryption/decryption (ECDH + AES-256-CBC)
   - BIP340 Schnorr signature generation
   - Supported methods: `sign_event`, `get_public_key`, `nip04_encrypt`, `nip04_decrypt`, `get_relays`, `ping`

2. **Policy Engine** (`policy_engine.c`)
   - File-backed policy store with TOML configuration
   - Wildcard matching for clients, methods, and event kinds
   - SIGHUP reload support for policy updates
   - Deny-by-default security model

3. **Key Store** (`key_store.c`)
   - Vault KV v2 integration for key retrieval
   - Optional TTL-based caching
   - Secure memory handling with OPENSSL_cleanse
   - Per-identity key isolation

4. **Replay Cache** (`replay_cache.c`)
   - TTL-based event ID tracking
   - Time skew validation
   - Memory-bounded with automatic eviction

5. **Audit Logger** (`audit_logger.c`)
   - JSONL structured logging
   - SIGHUP rotation support
   - No secret material in logs
   - Atomic append operations

6. **Relay Pool** (`relay_pool.c`)
   - Multi-relay connectivity
   - Auto-reconnect with backoff
   - Event subscription management
   - Background thread for I/O

7. **Health Server** (`health_server.c`)
   - HTTP GET /health endpoint
   - Per-component health status
   - Fast (<10ms) responses
   - JSON status output

## Installation

### Prerequisites

- C compiler (GCC or Clang)
- CMake 3.15+ or Meson 0.55+
- OpenSSL 3.x
- GLib 2.70+
- json-glib 1.0+
- libcurl (for Vault integration)
- libnostr (included in monorepo)

### Build with CMake

```bash
# From the nostrc root directory
cmake -B _build -DBUILD_TESTING=ON
cmake --build _build -j8

# Binaries will be in _build/signet/
# - signetd (daemon)
# - signetctl (CLI tool)
```

### Build with Meson

```bash
cd signet
meson setup _build
meson compile -C _build

# Binaries will be in _build/
```

## Configuration

Create a configuration file at `/etc/signet/signet.toml` or specify with `-c`:

```toml
# Vault configuration for key custody
[vault]
base_url = "https://vault.example.com:8200"
token = "hvs.CAES..."  # Or use VAULT_TOKEN env var
kv_mount = "secret"
kv_prefix = "signet/keys"
secret_key_field = "nsec"
ca_bundle_path = "/etc/ssl/certs/ca-bundle.crt"
timeout_ms = 5000

# Relay configuration
[relay]
urls = [
  "wss://relay.example.com",
  "wss://relay2.example.com"
]

# Policy configuration
[policy]
file_path = "/etc/signet/policies.toml"
default_decision = "deny"

# Replay protection
[replay]
max_entries = 10000
ttl_seconds = 300
skew_seconds = 30

# Audit logging
[audit]
log_path = "/var/log/signet/audit.jsonl"
flush_each_write = false

# Health monitoring
[health]
bind_addr = "127.0.0.1:9486"

# Management
[mgmt]
admin_pubkeys = [
  "npub1...",  # Admin public keys for management commands
]
```

## Policy Configuration

Create a policy file at `/etc/signet/policies.toml`:

```toml
# Policy for identity "alice"
[alice]
allow_clients = "*"  # Allow all clients
allow_methods = "sign_event,get_public_key,nip04_encrypt,nip04_decrypt"
allow_kinds = "1,4,5,6,7"  # Short text, DM, delete, repost, reaction
deny_clients = ""
deny_methods = ""
deny_kinds = ""
default = "deny"

# Policy for identity "bob" - more restrictive
[bob]
allow_clients = "npub1client1...,npub1client2..."  # Specific clients only
allow_methods = "sign_event,get_public_key"
allow_kinds = "1"  # Only short text notes
default = "deny"

# Policy with wildcard matching
[service_account]
allow_clients = "npub1app*"  # Any client starting with npub1app
allow_methods = "*"  # All methods
allow_kinds = "*"  # All kinds
default = "allow"
```

### Policy Rules

- **Evaluation order**: deny rules checked first, then allow rules
- **Wildcard support**: Use `*` for prefix/suffix matching (e.g., `npub1app*`, `*@example.com`)
- **Comma-separated lists**: Multiple values separated by commas
- **Default decision**: Applied when no explicit rule matches
- **SIGHUP reload**: Send SIGHUP to daemon to reload policies without restart

## Running the Daemon

### Start the daemon

```bash
# With default config path (/etc/signet/signet.toml)
signetd

# With custom config
signetd -c /path/to/config.toml

# Run in foreground (for testing)
signetd -c config.toml
```

### Systemd service

Create `/etc/systemd/system/signetd.service`:

```ini
[Unit]
Description=Signet NIP-46 Remote Signing Bunker
After=network.target vault.service

[Service]
Type=simple
User=signet
Group=signet
ExecStart=/usr/local/bin/signetd -c /etc/signet/signet.toml
ExecReload=/bin/kill -HUP $MAINPID
Restart=on-failure
RestartSec=5s

# Security hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/log/signet

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl enable signetd
sudo systemctl start signetd
sudo systemctl status signetd
```

## Management CLI

Use `signetctl` to manage the daemon:

### Add or update policy

```bash
signetctl add-policy alice \
  --allow-clients='*' \
  --allow-methods='sign_event,get_public_key' \
  --allow-kinds='1,4,5,6,7' \
  --default=deny
```

### Revoke policy

```bash
signetctl revoke-policy alice
```

### List all policies

```bash
signetctl list-policies
```

### Rotate key in Vault

```bash
signetctl rotate-key alice
```

### Check daemon health

```bash
signetctl health
```

### Test connectivity

```bash
signetctl ping
```

## Health Monitoring

Query the health endpoint:

```bash
curl http://127.0.0.1:9486/health
```

Response format:

```json
{
  "status": "ok",
  "timestamp": 1234567890,
  "uptime_seconds": 3600,
  "components": {
    "relay_pool": "connected",
    "vault_client": "reachable",
    "policy_store": "loaded",
    "key_store": "available"
  }
}
```

Status values:
- `ok`: All components healthy
- `degraded`: Some components unhealthy but service operational
- `error`: Critical components failed

## Audit Logs

Audit logs are written in JSONL format to the configured path:

```json
{"timestamp":"2024-03-04T22:00:00Z","event_type":"startup","details":{"version":"0.1.0","config_path":"/etc/signet/signet.toml"}}
{"timestamp":"2024-03-04T22:01:00Z","event_type":"sign_request","details":{"identity":"alice","client":"npub1...","method":"sign_event","kind":1,"allowed":true}}
{"timestamp":"2024-03-04T22:02:00Z","event_type":"policy_decision","details":{"identity":"alice","client":"npub1...","method":"sign_event","decision":"allow","reason":"allow_methods matched"}}
```

### Log rotation

Send SIGHUP to reopen log files after rotation:

```bash
# Rotate logs
mv /var/log/signet/audit.jsonl /var/log/signet/audit.jsonl.1
gzip /var/log/signet/audit.jsonl.1

# Reopen
sudo systemctl reload signetd
# Or: kill -HUP $(pidof signetd)
```

## Security Considerations

### Key Storage

- **Never store keys in config files**: Use Vault or secure key management
- **Vault token security**: Use short-lived tokens with appropriate policies
- **Key rotation**: Regularly rotate keys using `signetctl rotate-key`

### Network Security

- **TLS for Vault**: Always use HTTPS for Vault communication
- **Relay authentication**: Consider using authenticated relays
- **Health endpoint**: Bind to localhost or use firewall rules

### Policy Best Practices

- **Deny by default**: Start with restrictive policies
- **Principle of least privilege**: Grant minimum required permissions
- **Regular audits**: Review audit logs for suspicious activity
- **Client allowlists**: Use specific client pubkeys when possible

### Operational Security

- **Run as dedicated user**: Don't run as root
- **File permissions**: Restrict config and log file access (0600)
- **Audit log retention**: Implement log rotation and retention policies
- **Monitor health endpoint**: Set up alerting for degraded status

## Troubleshooting

### Daemon won't start

1. Check config file syntax: `signetd -c config.toml --validate` (if implemented)
2. Verify Vault connectivity: `curl -H "X-Vault-Token: $VAULT_TOKEN" https://vault.example.com:8200/v1/sys/health`
3. Check audit log for errors: `tail -f /var/log/signet/audit.jsonl`

### Policy not working

1. Verify policy file syntax
2. Check policy file permissions (readable by signet user)
3. Reload policies: `sudo systemctl reload signetd`
4. Review audit logs for policy decisions

### Relay connection issues

1. Test relay connectivity: `websocat wss://relay.example.com`
2. Check relay pool status via health endpoint
3. Review relay URLs in config

### High memory usage

1. Reduce replay cache size: Lower `replay.max_entries`
2. Disable key store caching: Set `key_store.cache_ttl_seconds = 0`
3. Monitor with: `ps aux | grep signetd`

## Development

### Running tests

```bash
# Build with tests enabled
cmake -B _build -DBUILD_TESTING=ON
cmake --build _build

# Run all tests
cd _build && ctest --output-on-failure

# Run specific test
./signet/tests/test_replay_cache
```

### Code structure

```
signet/
├── include/signet/       # Public headers
│   ├── audit_logger.h
│   ├── key_store.h
│   ├── nip46_server.h
│   ├── policy_engine.h
│   └── ...
├── src/                  # Implementation
│   ├── audit_logger.c
│   ├── key_store.c
│   ├── nip46_server.c
│   ├── signetd_main.c
│   └── ...
├── tests/                # Test suite
│   ├── test_replay_cache.c
│   ├── test_policy_engine.c
│   └── ...
├── CMakeLists.txt        # CMake build
├── meson.build           # Meson build
└── signet.toml.example   # Example config
```

## License

MIT License - See LICENSE file for details

## Contributing

Contributions welcome! Please:

1. Follow existing code style
2. Add tests for new features
3. Update documentation
4. Ensure all tests pass

## Support

- Issues: https://github.com/your-org/nostrc/issues
- Documentation: https://docs.example.com/signet
- Community: nostr:npub1...

## Acknowledgments

- Built on [libnostr](../libnostr/)
- Implements [NIP-46](https://github.com/nostr-protocol/nips/blob/master/46.md)
- Uses [HashiCorp Vault](https://www.vaultproject.io/) for key custody
