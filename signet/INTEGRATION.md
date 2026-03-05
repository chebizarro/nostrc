# Signet Integration Guide

This guide explains how to integrate Signet NIP-46 remote signing into your Nostr application.

## Table of Contents

1. [Overview](#overview)
2. [Quick Start](#quick-start)
3. [Vault Setup](#vault-setup)
4. [Client Integration](#client-integration)
5. [Production Deployment](#production-deployment)
6. [Monitoring and Operations](#monitoring-and-operations)
7. [Security Best Practices](#security-best-practices)
8. [Troubleshooting](#troubleshooting)

## Overview

Signet implements the NIP-46 protocol, which allows Nostr clients to request remote signing operations without having direct access to private keys. This architecture provides:

- **Key Custody**: Private keys stored securely in Vault, not on client devices
- **Policy Enforcement**: Fine-grained control over what operations are allowed
- **Audit Trail**: Complete logging of all signing requests
- **Multi-Device**: Same identity usable across multiple devices/applications

### Architecture

```
┌─────────────┐         ┌─────────────┐         ┌─────────────┐
│   Client    │         │   Signet    │         │    Vault    │
│ Application │◄───────►│   Bunker    │◄───────►│  (Keys)     │
└─────────────┘         └─────────────┘         └─────────────┘
      │                       │
      │                       │
      └───────────────────────┘
              Nostr Relays
```

**Flow:**
1. Client sends NIP-46 request event (kind 24133) to relays
2. Signet receives and decrypts request
3. Signet checks policy authorization
4. Signet retrieves key from Vault
5. Signet performs operation (e.g., signs event)
6. Signet encrypts and publishes response event

## Quick Start

### 1. Install Signet

```bash
# Build from source
cd nostrc
cmake -B _build
cmake --build _build
sudo cmake --install _build

# Or copy binaries
sudo cp _build/signet/signetd /usr/local/bin/
sudo cp _build/signet/signetctl /usr/local/bin/
```

### 2. Set Up Vault

```bash
# Start Vault (dev mode for testing)
vault server -dev

# Set environment
export VAULT_ADDR='http://127.0.0.1:8200'
export VAULT_TOKEN='root'

# Create a test key
vault kv put secret/signet/keys/alice nsec=nsec1...
```

### 3. Configure Signet

Create `/etc/signet/signet.toml`:

```toml
[vault]
base_url = "http://127.0.0.1:8200"
token = "root"
kv_mount = "secret"
kv_prefix = "signet/keys"
secret_key_field = "nsec"

[relay]
urls = ["wss://relay.damus.io"]

[policy]
file_path = "/etc/signet/policies.toml"
default_decision = "deny"

[audit]
log_path = "/var/log/signet/audit.jsonl"
```

Create `/etc/signet/policies.toml`:

```toml
[alice]
allow_clients = "*"
allow_methods = "*"
allow_kinds = "*"
default = "allow"
```

### 4. Start Signet

```bash
# Create log directory
sudo mkdir -p /var/log/signet
sudo chown $USER /var/log/signet

# Start daemon
signetd -c /etc/signet/signet.toml
```

### 5. Test with Client

See [Client Integration](#client-integration) section below.

## Vault Setup

### Production Vault Configuration

#### 1. Install Vault

```bash
# Using package manager
sudo apt-get install vault  # Debian/Ubuntu
sudo yum install vault      # RHEL/CentOS

# Or download binary
wget https://releases.hashicorp.com/vault/1.15.0/vault_1.15.0_linux_amd64.zip
unzip vault_1.15.0_linux_amd64.zip
sudo mv vault /usr/local/bin/
```

#### 2. Configure Vault Server

Create `/etc/vault.d/vault.hcl`:

```hcl
storage "file" {
  path = "/var/lib/vault/data"
}

listener "tcp" {
  address     = "127.0.0.1:8200"
  tls_disable = 0
  tls_cert_file = "/etc/vault.d/tls/vault.crt"
  tls_key_file  = "/etc/vault.d/tls/vault.key"
}

api_addr = "https://127.0.0.1:8200"
ui = true
```

#### 3. Initialize Vault

```bash
# Start Vault
sudo systemctl start vault

# Initialize (save unseal keys and root token securely!)
vault operator init

# Unseal (requires threshold of unseal keys)
vault operator unseal <unseal-key-1>
vault operator unseal <unseal-key-2>
vault operator unseal <unseal-key-3>

# Login
vault login <root-token>
```

#### 4. Enable KV v2 Secrets Engine

```bash
# Enable KV v2 at 'secret' path
vault secrets enable -version=2 -path=secret kv

# Create policy for Signet
vault policy write signet-policy - <<EOF
path "secret/data/signet/keys/*" {
  capabilities = ["read"]
}
EOF

# Create token for Signet
vault token create -policy=signet-policy -period=24h
```

#### 5. Store Keys in Vault

```bash
# Store a Nostr private key
vault kv put secret/signet/keys/alice nsec=nsec1...

# Verify
vault kv get secret/signet/keys/alice
```

### Vault Security Best Practices

1. **Use TLS**: Always enable TLS for production Vault
2. **Token Rotation**: Use short-lived tokens with periodic renewal
3. **Access Control**: Use Vault policies to restrict access
4. **Audit Logging**: Enable Vault audit logging
5. **Backup**: Regularly backup Vault data and unseal keys
6. **High Availability**: Use Vault HA for production

## Client Integration

### NIP-46 Protocol Overview

NIP-46 uses encrypted request/response events:

1. **Request Event** (kind 24133):
   - `content`: NIP-04 encrypted JSON request
   - `tags`: `["p", "<bunker-pubkey>"]`

2. **Response Event** (kind 24133):
   - `content`: NIP-04 encrypted JSON response
   - `tags`: `["p", "<client-pubkey>"]`, `["e", "<request-event-id>"]`

### JavaScript/TypeScript Client Example

```typescript
import { SimplePool, nip04, nip19, getPublicKey } from 'nostr-tools';

class NIP46Client {
  private pool: SimplePool;
  private clientSecretKey: string;
  private bunkerPubkey: string;
  private relays: string[];

  constructor(clientSecretKey: string, bunkerPubkey: string, relays: string[]) {
    this.pool = new SimplePool();
    this.clientSecretKey = clientSecretKey;
    this.bunkerPubkey = bunkerPubkey;
    this.relays = relays;
  }

  async signEvent(identity: string, unsignedEvent: any): Promise<any> {
    const clientPubkey = getPublicKey(this.clientSecretKey);

    // Build request
    const request = {
      id: crypto.randomUUID(),
      method: 'sign_event',
      params: [unsignedEvent]
    };

    // Encrypt request
    const encryptedContent = await nip04.encrypt(
      this.clientSecretKey,
      this.bunkerPubkey,
      JSON.stringify(request)
    );

    // Create request event
    const requestEvent = {
      kind: 24133,
      created_at: Math.floor(Date.now() / 1000),
      tags: [['p', this.bunkerPubkey]],
      content: encryptedContent,
      pubkey: clientPubkey
    };

    // Sign and publish request
    const signedRequest = await this.signLocalEvent(requestEvent);
    await this.pool.publish(this.relays, signedRequest);

    // Wait for response
    return new Promise((resolve, reject) => {
      const sub = this.pool.sub(this.relays, [{
        kinds: [24133],
        authors: [this.bunkerPubkey],
        '#p': [clientPubkey],
        '#e': [signedRequest.id],
        since: Math.floor(Date.now() / 1000)
      }]);

      const timeout = setTimeout(() => {
        sub.unsub();
        reject(new Error('Response timeout'));
      }, 30000);

      sub.on('event', async (event) => {
        clearTimeout(timeout);
        sub.unsub();

        // Decrypt response
        const decrypted = await nip04.decrypt(
          this.clientSecretKey,
          this.bunkerPubkey,
          event.content
        );

        const response = JSON.parse(decrypted);

        if (response.error) {
          reject(new Error(response.error));
        } else {
          resolve(response.result);
        }
      });
    });
  }

  async getPublicKey(identity: string): Promise<string> {
    const request = {
      id: crypto.randomUUID(),
      method: 'get_public_key',
      params: []
    };

    // Similar flow as signEvent...
    // Returns the public key for the identity
  }

  private async signLocalEvent(event: any): Promise<any> {
    // Sign event with client's key
    const { signEvent } = await import('nostr-tools');
    return signEvent(event, this.clientSecretKey);
  }
}

// Usage
const client = new NIP46Client(
  'nsec1...', // Client's secret key
  'npub1...', // Bunker's public key
  ['wss://relay.damus.io']
);

// Sign an event remotely
const unsignedEvent = {
  kind: 1,
  created_at: Math.floor(Date.now() / 1000),
  tags: [],
  content: 'Hello from NIP-46!',
  pubkey: '' // Will be filled by bunker
};

const signedEvent = await client.signEvent('alice', unsignedEvent);
console.log('Signed event:', signedEvent);
```

### Python Client Example

```python
import json
import time
import uuid
from nostr.key import PrivateKey
from nostr.event import Event, EventKind
from nostr.relay_manager import RelayManager
from nostr.message_type import ClientMessageType

class NIP46Client:
    def __init__(self, client_nsec: str, bunker_npub: str, relays: list):
        self.client_key = PrivateKey.from_nsec(client_nsec)
        self.bunker_pubkey = bunker_npub
        self.relay_manager = RelayManager()
        for relay in relays:
            self.relay_manager.add_relay(relay)
        self.relay_manager.open_connections()

    def sign_event(self, identity: str, unsigned_event: dict) -> dict:
        # Build request
        request = {
            "id": str(uuid.uuid4()),
            "method": "sign_event",
            "params": [unsigned_event]
        }

        # Encrypt request (NIP-04)
        encrypted = self.client_key.encrypt_message(
            json.dumps(request),
            self.bunker_pubkey
        )

        # Create request event
        request_event = Event(
            kind=24133,
            content=encrypted,
            tags=[["p", self.bunker_pubkey]]
        )
        self.client_key.sign_event(request_event)

        # Publish and wait for response
        self.relay_manager.publish_event(request_event)
        
        # Subscribe to response
        filters = [{
            "kinds": [24133],
            "authors": [self.bunker_pubkey],
            "#p": [self.client_key.public_key.hex()],
            "#e": [request_event.id],
            "since": int(time.time())
        }]
        
        subscription_id = str(uuid.uuid4())
        self.relay_manager.add_subscription(subscription_id, filters)
        
        # Wait for response (simplified - use proper async in production)
        timeout = time.time() + 30
        while time.time() < timeout:
            time.sleep(0.1)
            # Check for response events
            # Decrypt and return result
        
        raise TimeoutError("No response from bunker")

# Usage
client = NIP46Client(
    client_nsec="nsec1...",
    bunker_npub="npub1...",
    relays=["wss://relay.damus.io"]
)

unsigned = {
    "kind": 1,
    "created_at": int(time.time()),
    "tags": [],
    "content": "Hello from Python NIP-46!"
}

signed = client.sign_event("alice", unsigned)
print(f"Signed event: {signed}")
```

## Production Deployment

### System Requirements

- **CPU**: 2+ cores recommended
- **RAM**: 2GB minimum, 4GB recommended
- **Disk**: 10GB for logs and cache
- **Network**: Stable connection to relays and Vault

### Deployment Checklist

- [ ] Vault configured with TLS
- [ ] Vault policies created for Signet
- [ ] Keys stored in Vault with proper paths
- [ ] Signet config file created with production settings
- [ ] Policy file created with restrictive defaults
- [ ] Log directory created with proper permissions
- [ ] Systemd service configured
- [ ] Firewall rules configured (allow health port if needed)
- [ ] Monitoring configured (health endpoint, logs)
- [ ] Log rotation configured
- [ ] Backup strategy for Vault and configs
- [ ] Disaster recovery plan documented

### High Availability Setup

For production, consider running multiple Signet instances:

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  Signet 1   │     │  Signet 2   │     │  Signet 3   │
└──────┬──────┘     └──────┬──────┘     └──────┬──────┘
       │                   │                   │
       └───────────────────┴───────────────────┘
                           │
                    ┌──────┴──────┐
                    │    Vault    │
                    │   (HA Mode) │
                    └─────────────┘
```

**Benefits:**
- Load distribution across relays
- Redundancy if one instance fails
- Rolling updates without downtime

**Configuration:**
- Each instance uses same Vault
- Each instance has same policy configuration
- Different relay sets for load distribution

### Docker Deployment

Example `Dockerfile`:

```dockerfile
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    libssl3 \
    libglib2.0-0 \
    libjson-glib-1.0-0 \
    libcurl4 \
    && rm -rf /var/lib/apt/lists/*

COPY signetd /usr/local/bin/
COPY signetctl /usr/local/bin/

RUN useradd -r -s /bin/false signet && \
    mkdir -p /var/log/signet && \
    chown signet:signet /var/log/signet

USER signet
EXPOSE 9486

ENTRYPOINT ["/usr/local/bin/signetd"]
CMD ["-c", "/etc/signet/signet.toml"]
```

Example `docker-compose.yml`:

```yaml
version: '3.8'

services:
  vault:
    image: vault:1.15
    ports:
      - "8200:8200"
    environment:
      VAULT_DEV_ROOT_TOKEN_ID: "root"
    cap_add:
      - IPC_LOCK

  signet:
    build: .
    depends_on:
      - vault
    ports:
      - "9486:9486"
    volumes:
      - ./signet.toml:/etc/signet/signet.toml:ro
      - ./policies.toml:/etc/signet/policies.toml:ro
      - signet-logs:/var/log/signet
    restart: unless-stopped

volumes:
  signet-logs:
```

## Monitoring and Operations

### Health Monitoring

Set up monitoring for the health endpoint:

```bash
# Prometheus scrape config
scrape_configs:
  - job_name: 'signet'
    static_configs:
      - targets: ['localhost:9486']
    metrics_path: '/health'
```

### Alerting Rules

Example alerts:

```yaml
groups:
  - name: signet
    rules:
      - alert: SignetDown
        expr: up{job="signet"} == 0
        for: 5m
        annotations:
          summary: "Signet instance is down"

      - alert: SignetDegraded
        expr: signet_health_status != "ok"
        for: 10m
        annotations:
          summary: "Signet health is degraded"

      - alert: VaultUnreachable
        expr: signet_vault_reachable == 0
        for: 5m
        annotations:
          summary: "Signet cannot reach Vault"
```

### Log Analysis

Parse audit logs for insights:

```bash
# Count requests by identity
jq -r 'select(.event_type=="sign_request") | .details.identity' \
  /var/log/signet/audit.jsonl | sort | uniq -c

# Find denied requests
jq 'select(.event_type=="policy_decision" and .details.decision=="deny")' \
  /var/log/signet/audit.jsonl

# Track error rates
jq -r 'select(.event_type=="error") | .details.error_type' \
  /var/log/signet/audit.jsonl | sort | uniq -c
```

### Performance Tuning

1. **Replay Cache Size**: Adjust based on request volume
   ```toml
   [replay]
   max_entries = 50000  # Increase for high volume
   ```

2. **Key Store Caching**: Enable for frequently used keys
   ```toml
   [key_store]
   cache_ttl_seconds = 300  # 5 minute cache
   ```

3. **Relay Pool**: Add more relays for redundancy
   ```toml
   [relay]
   urls = ["wss://relay1.com", "wss://relay2.com", "wss://relay3.com"]
   ```

## Security Best Practices

### 1. Principle of Least Privilege

- Start with deny-all policies
- Grant minimum required permissions
- Use specific client pubkeys when possible
- Regularly audit and remove unused policies

### 2. Key Management

- Rotate keys regularly (quarterly recommended)
- Use Vault's key versioning
- Never log private keys or plaintexts
- Use secure key generation (nostr-tools, nak, etc.)

### 3. Network Security

- Use TLS for all external connections
- Bind health endpoint to localhost only
- Use firewall rules to restrict access
- Consider VPN for Vault access

### 4. Operational Security

- Run as dedicated non-root user
- Use systemd security features (NoNewPrivileges, ProtectSystem, etc.)
- Implement log retention policies
- Regular security audits of configurations

### 5. Incident Response

Have a plan for:
- Compromised keys (revocation process)
- Vault breach (key rotation)
- Policy bypass (audit log review)
- Service disruption (failover procedures)

## Troubleshooting

### Common Issues

#### 1. "Failed to connect to Vault"

**Symptoms**: Signet can't start, logs show Vault connection errors

**Solutions**:
- Verify Vault is running: `vault status`
- Check Vault URL in config
- Verify Vault token is valid: `vault token lookup`
- Check network connectivity: `curl $VAULT_ADDR/v1/sys/health`
- Verify TLS certificates if using HTTPS

#### 2. "Policy decision: deny"

**Symptoms**: Signing requests are rejected

**Solutions**:
- Check policy file syntax
- Verify identity exists in policy file
- Review audit logs for exact deny reason
- Test with permissive policy first
- Reload policies: `sudo systemctl reload signetd`

#### 3. "Replay attack detected"

**Symptoms**: Legitimate requests rejected as replays

**Solutions**:
- Check client clock synchronization
- Increase skew tolerance in config
- Verify event IDs are unique
- Check replay cache TTL settings

#### 4. "No response from bunker"

**Symptoms**: Client timeout waiting for response

**Solutions**:
- Verify Signet is running and healthy
- Check relay connectivity from both sides
- Verify bunker pubkey is correct
- Check audit logs for request processing
- Increase client timeout

#### 5. High memory usage

**Symptoms**: Signet consuming excessive memory

**Solutions**:
- Reduce replay cache size
- Disable key store caching
- Check for relay connection leaks
- Review audit log size and rotation

### Debug Mode

Enable verbose logging (if implemented):

```toml
[logging]
level = "debug"
```

Or run in foreground:

```bash
signetd -c config.toml --foreground --verbose
```

### Getting Help

1. Check audit logs: `/var/log/signet/audit.jsonl`
2. Check systemd logs: `journalctl -u signetd -f`
3. Query health endpoint: `curl http://localhost:9486/health`
4. Review configuration files
5. Test with minimal config
6. File issue with logs and config (redact secrets!)

## Additional Resources

- [NIP-46 Specification](https://github.com/nostr-protocol/nips/blob/master/46.md)
- [NIP-04 Encryption](https://github.com/nostr-protocol/nips/blob/master/04.md)
- [Vault Documentation](https://www.vaultproject.io/docs)
- [Nostr Protocol](https://github.com/nostr-protocol/nostr)

## Appendix: Event Kind Reference

Common Nostr event kinds for policy configuration:

| Kind | Description |
|------|-------------|
| 0 | User metadata |
| 1 | Short text note |
| 3 | Contact list |
| 4 | Encrypted direct message |
| 5 | Event deletion |
| 6 | Repost |
| 7 | Reaction |
| 40-49 | Channel operations |
| 1984 | Reporting |
| 9734-9735 | Zap request/receipt |
| 10000-19999 | Replaceable events |
| 20000-29999 | Ephemeral events |
| 30000-39999 | Parameterized replaceable events |

Refer to [NIP-01](https://github.com/nostr-protocol/nips/blob/master/01.md) for complete list.
