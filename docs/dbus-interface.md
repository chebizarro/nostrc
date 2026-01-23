# GNostr Signer D-Bus Interface

This document describes the D-Bus interface exposed by the `gnostr-signer-daemon` for secure Nostr key management and cryptographic operations.

## Overview

The GNostr Signer provides a secure D-Bus service for:

- **Key Management**: Secure storage and retrieval of Nostr private keys
- **Event Signing**: Cryptographic signing of Nostr events (NIP-01)
- **Encryption/Decryption**: NIP-04 (legacy) and NIP-44 (modern) message encryption
- **Zap Processing**: Decryption of zap receipt events (NIP-57)
- **User Approval**: Interactive approval flow for sensitive operations

The daemon isolates private keys from client applications, ensuring keys never leave the secure process boundary.

## Bus Name and Object Path

| Property | Value |
|----------|-------|
| **Bus Name** | `org.nostr.Signer` |
| **Object Path** | `/org/nostr/signer` |
| **Interface** | `org.nostr.Signer` |
| **Bus Type** | Session bus (default) or System bus (`--system` flag) |

### D-Bus Service Activation

The daemon supports D-Bus activation. When a client calls a method on `org.nostr.Signer`, D-Bus will automatically start the daemon if it is not running.

Service file location: `/usr/share/dbus-1/services/org.nostr.Signer.service`

## Interface: org.nostr.Signer

### Methods

#### GetPublicKey

Returns the public key (npub) for the currently active identity.

```xml
<method name="GetPublicKey">
  <arg name="npub" type="s" direction="out"/>
</method>
```

**Parameters**: None

**Returns**:
- `npub` (string): Bech32-encoded public key (npub1...)

**Errors**:
- `org.nostr.Signer.Error.Internal`: No key configured or backend failure

---

#### SignEvent

Signs a Nostr event and returns the signature. May trigger user approval dialog.

```xml
<method name="SignEvent">
  <arg name="event_json" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="app_id" type="s" direction="in"/>
  <arg name="signature" type="s" direction="out"/>
</method>
```

**Parameters**:
- `event_json` (string): JSON-serialized Nostr event (without signature)
- `current_user` (string): Identity selector (npub, key_id, or empty for default)
- `app_id` (string): Application identifier for ACL (uses D-Bus sender if empty)

**Returns**:
- `signature` (string): Hex-encoded Schnorr signature (64 bytes / 128 hex chars)

**Errors**:
- `org.nostr.Signer.Error.InvalidInput`: Malformed event JSON
- `org.nostr.Signer.Error.ApprovalDenied`: User denied the signing request
- `org.nostr.Signer.Error.RateLimited`: Too many requests in short period
- `org.nostr.Signer.Error.Internal`: Signing operation failed

**Notes**:
- If no ACL decision exists, emits `ApprovalRequested` signal and waits for `ApproveRequest`
- The `created_at` field is auto-populated if set to 0

---

#### NIP44Encrypt

Encrypts a message using NIP-44 v2 (modern, recommended).

```xml
<method name="NIP44Encrypt">
  <arg name="plaintext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="ciphertext" type="s" direction="out"/>
</method>
```

**Parameters**:
- `plaintext` (string): UTF-8 message to encrypt
- `peer_pubkey` (string): Recipient's public key (64-char hex)
- `current_user` (string): Identity selector (npub, key_id, or empty for default)

**Returns**:
- `ciphertext` (string): Base64-encoded NIP-44 v2 ciphertext

**Errors**:
- `org.nostr.Signer.Error.InvalidInput`: Invalid public key format
- `org.nostr.Signer.Error.Internal`: Encryption failed

---

#### NIP44Decrypt

Decrypts a message using NIP-44 v2.

```xml
<method name="NIP44Decrypt">
  <arg name="ciphertext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="plaintext" type="s" direction="out"/>
</method>
```

**Parameters**:
- `ciphertext` (string): Base64-encoded NIP-44 v2 ciphertext
- `peer_pubkey` (string): Sender's public key (64-char hex)
- `current_user` (string): Identity selector

**Returns**:
- `plaintext` (string): Decrypted UTF-8 message

**Errors**:
- `org.nostr.Signer.Error.InvalidInput`: Invalid ciphertext or public key
- `org.nostr.Signer.Error.Internal`: Decryption failed (wrong key or corrupted)

---

#### NIP04Encrypt

Encrypts a message using NIP-04 (legacy, for compatibility).

```xml
<method name="NIP04Encrypt">
  <arg name="plaintext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="ciphertext" type="s" direction="out"/>
</method>
```

**Parameters**: Same as NIP44Encrypt

**Returns**:
- `ciphertext` (string): Base64-encoded NIP-04 ciphertext with IV

**Notes**: NIP-04 is deprecated. Prefer NIP-44 for new implementations.

---

#### NIP04Decrypt

Decrypts a message using NIP-04 (legacy).

```xml
<method name="NIP04Decrypt">
  <arg name="ciphertext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="plaintext" type="s" direction="out"/>
</method>
```

**Parameters**: Same as NIP44Decrypt

---

#### DecryptZapEvent

Decrypts the content of a zap receipt event (NIP-57).

```xml
<method name="DecryptZapEvent">
  <arg name="event_json" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="decrypted_event" type="s" direction="out"/>
</method>
```

**Parameters**:
- `event_json` (string): JSON-serialized zap event
- `current_user` (string): Identity selector

**Returns**:
- `decrypted_event` (string): Event JSON with decrypted content field

**Notes**:
- Extracts peer pubkey from the first `p` tag
- Tries NIP-44 first, falls back to NIP-04

---

#### GetRelays

Returns the list of configured relay URLs.

```xml
<method name="GetRelays">
  <arg name="relays_json" type="s" direction="out"/>
</method>
```

**Returns**:
- `relays_json` (string): JSON array of relay URLs (e.g., `["wss://relay.damus.io"]`)

---

#### StoreKey

Stores a private key in the secure backend (libsecret/Keychain).

```xml
<method name="StoreKey">
  <arg name="key" type="s" direction="in"/>
  <arg name="identity" type="s" direction="in"/>
  <arg name="ok" type="b" direction="out"/>
  <arg name="npub" type="s" direction="out"/>
</method>
```

**Parameters**:
- `key` (string): Private key (64-char hex or nsec1... bech32)
- `identity` (string): Optional identity label/selector

**Returns**:
- `ok` (boolean): True if stored successfully
- `npub` (string): Derived public key (npub1...)

**Errors**:
- `org.nostr.Signer.Error.PermissionDenied`: Key mutations disabled
- `org.nostr.Signer.Error.RateLimited`: Rate limit exceeded
- `org.nostr.Signer.InvalidKey`: Invalid private key format
- `org.nostr.Signer.SecretServiceUnavailable`: Backend unavailable

**Security**: Requires `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1` environment variable.

---

#### ClearKey

Removes a private key from the secure backend.

```xml
<method name="ClearKey">
  <arg name="identity" type="s" direction="in"/>
  <arg name="ok" type="b" direction="out"/>
</method>
```

**Parameters**:
- `identity` (string): Identity selector (npub or key_id)

**Returns**:
- `ok` (boolean): True if removed successfully

**Security**: Requires `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1` environment variable.

---

#### ApproveRequest

Responds to a pending approval request (from UI).

```xml
<method name="ApproveRequest">
  <arg name="request_id" type="s" direction="in"/>
  <arg name="decision" type="b" direction="in"/>
  <arg name="remember" type="b" direction="in"/>
  <arg name="ttl_seconds" type="t" direction="in"/>
  <arg name="ok" type="b" direction="out"/>
</method>
```

**Parameters**:
- `request_id` (string): ID from `ApprovalRequested` signal
- `decision` (boolean): True to approve, false to deny
- `remember` (boolean): Save decision to ACL file
- `ttl_seconds` (uint64): How long to remember (0 = forever)

**Returns**:
- `ok` (boolean): True if request was found and processed

---

### Signals

#### ApprovalRequested

Emitted when a signing operation requires user approval.

```xml
<signal name="ApprovalRequested">
  <arg type="s" name="app_id"/>
  <arg type="s" name="identity"/>
  <arg type="s" name="kind"/>
  <arg type="s" name="preview"/>
  <arg type="s" name="request_id"/>
</signal>
```

**Arguments**:
- `app_id`: Requesting application identifier
- `identity`: Target identity (npub)
- `kind`: Request type (e.g., "event")
- `preview`: Human-readable preview of the content (truncated)
- `request_id`: Unique ID to use with `ApproveRequest`

---

#### ApprovalCompleted

Emitted when an approval request has been resolved.

```xml
<signal name="ApprovalCompleted">
  <arg type="s" name="request_id"/>
  <arg type="b" name="decision"/>
</signal>
```

**Arguments**:
- `request_id`: The request that was completed
- `decision`: True if approved, false if denied

---

## Error Codes

### D-Bus Error Names

| Error Name | Description |
|------------|-------------|
| `org.nostr.Signer.Error.PermissionDenied` | Operation not allowed by policy |
| `org.nostr.Signer.Error.RateLimited` | Too many requests (500ms cooldown) |
| `org.nostr.Signer.Error.ApprovalDenied` | User denied the operation |
| `org.nostr.Signer.Error.InvalidInput` | Malformed input data |
| `org.nostr.Signer.Error.Internal` | Internal error or backend failure |
| `org.nostr.Signer.InvalidKey` | Invalid private key format |
| `org.nostr.Signer.InvalidArgument` | Invalid method argument |
| `org.nostr.Signer.NotFound` | Key or identity not found |
| `org.nostr.Signer.SecretServiceUnavailable` | Secret storage backend unavailable |
| `org.nostr.Signer.Failure` | Generic operation failure |

### Internal Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0 | `NOSTR_SIGNER_OK` | Success |
| 1 | `NOSTR_SIGNER_ERROR_INVALID_JSON` | Malformed JSON input |
| 2 | `NOSTR_SIGNER_ERROR_INVALID_KEY` | Invalid key format |
| 3 | `NOSTR_SIGNER_ERROR_UNAUTHORIZED` | Unauthorized operation |
| 4 | `NOSTR_SIGNER_ERROR_PERMISSION_DENIED` | Permission denied |
| 5 | `NOSTR_SIGNER_ERROR_CRYPTO_FAILED` | Cryptographic operation failed |
| 6 | `NOSTR_SIGNER_ERROR_NOT_FOUND` | Resource not found |
| 7 | `NOSTR_SIGNER_ERROR_BACKEND` | Backend service error |
| 8 | `NOSTR_SIGNER_ERROR_RATE_LIMITED` | Rate limit exceeded |
| 9 | `NOSTR_SIGNER_ERROR_INVALID_ARG` | Invalid argument |

---

## Rate Limiting

The signer implements rate limiting to prevent abuse:

| Operation | Cooldown |
|-----------|----------|
| `SignEvent` | 100ms per D-Bus sender |
| `StoreKey` / `ClearKey` | 500ms per D-Bus sender |

Requests exceeding the rate limit receive `org.nostr.Signer.Error.RateLimited`.

---

## Security Considerations

### Verifying the Signer is Authentic

1. **Check the bus name owner**: Use `GetNameOwner` to verify `org.nostr.Signer` is owned by the expected process.

2. **Verify process credentials**: On Linux, use `org.freedesktop.DBus.GetConnectionUnixProcessID` to get the PID, then verify `/proc/<pid>/exe` points to the expected binary.

3. **Flatpak/Snap isolation**: When running in a sandbox, the signer should be on the host system with appropriate portal access.

### Permission Model

- **Key mutations disabled by default**: `StoreKey` and `ClearKey` require `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1`
- **User approval required**: Signing operations without cached ACL decisions trigger interactive approval
- **ACL persistence**: Decisions can be remembered with configurable TTL in `~/.config/gnostr/signer-acl.ini`

### Key Storage Security

| Platform | Backend | Security |
|----------|---------|----------|
| Linux | libsecret (GNOME Keyring / KDE Wallet) | Encrypted, user session locked |
| macOS | Keychain | Hardware-backed, biometric unlock |
| Fallback | Environment variables | Not recommended for production |

### Systemd Hardening

The daemon service unit includes extensive hardening:

- `NoNewPrivileges=yes`
- `ProtectSystem=strict`
- `PrivateDevices=yes`
- `MemoryDenyWriteExecute=yes`
- `SystemCallFilter` whitelist
- `LimitCORE=0` (no core dumps with secrets)

---

## Example Code

### Python (using pydbus)

```python
#!/usr/bin/env python3
"""Example: Sign a Nostr event using GNostr Signer D-Bus interface."""

from pydbus import SessionBus
import json
import time

# Connect to the session bus
bus = SessionBus()

# Get the signer proxy
signer = bus.get("org.nostr.Signer", "/org/nostr/signer")

# Get the current public key
try:
    npub = signer.GetPublicKey()
    print(f"Public key: {npub}")
except Exception as e:
    print(f"Error getting public key: {e}")
    exit(1)

# Create an unsigned event
event = {
    "kind": 1,
    "content": "Hello from GNostr Signer!",
    "tags": [],
    "created_at": int(time.time()),
    "pubkey": ""  # Will be filled by signer
}

# Sign the event
try:
    signature = signer.SignEvent(
        json.dumps(event),  # event_json
        "",                  # current_user (empty = default identity)
        "my-app"            # app_id
    )
    print(f"Signature: {signature}")
except Exception as e:
    print(f"Signing failed: {e}")

# NIP-44 encryption example
recipient_pubkey = "abc123..."  # 64-char hex pubkey
try:
    ciphertext = signer.NIP44Encrypt(
        "Secret message",
        recipient_pubkey,
        ""  # current_user
    )
    print(f"Encrypted: {ciphertext}")

    # Decrypt
    plaintext = signer.NIP44Decrypt(ciphertext, recipient_pubkey, "")
    print(f"Decrypted: {plaintext}")
except Exception as e:
    print(f"Encryption error: {e}")
```

### Python (Listening for Approval Signals)

```python
#!/usr/bin/env python3
"""Example: Listen for approval requests from GNostr Signer."""

from pydbus import SessionBus
from gi.repository import GLib

bus = SessionBus()
signer = bus.get("org.nostr.Signer", "/org/nostr/signer")

def on_approval_requested(app_id, identity, kind, preview, request_id):
    print(f"Approval requested:")
    print(f"  App: {app_id}")
    print(f"  Identity: {identity}")
    print(f"  Kind: {kind}")
    print(f"  Preview: {preview}")
    print(f"  Request ID: {request_id}")

    # Auto-approve for demonstration (in real app, show UI)
    user_approves = True
    remember = False
    ttl = 0

    ok = signer.ApproveRequest(request_id, user_approves, remember, ttl)
    print(f"Approval sent: {ok}")

def on_approval_completed(request_id, decision):
    print(f"Approval completed: {request_id} -> {'approved' if decision else 'denied'}")

# Subscribe to signals
signer.ApprovalRequested.connect(on_approval_requested)
signer.ApprovalCompleted.connect(on_approval_completed)

print("Listening for approval requests... (Ctrl+C to exit)")
loop = GLib.MainLoop()
loop.run()
```

### JavaScript (using dbus-native / Node.js)

```javascript
#!/usr/bin/env node
/**
 * Example: Sign a Nostr event using GNostr Signer D-Bus interface.
 */

const dbus = require('dbus-native');

const bus = dbus.sessionBus();

const service = bus.getService('org.nostr.Signer');

service.getInterface(
  '/org/nostr/signer',
  'org.nostr.Signer',
  (err, signer) => {
    if (err) {
      console.error('Failed to get interface:', err);
      process.exit(1);
    }

    // Get public key
    signer.GetPublicKey((err, npub) => {
      if (err) {
        console.error('GetPublicKey error:', err);
        return;
      }
      console.log('Public key:', npub);
    });

    // Sign an event
    const event = JSON.stringify({
      kind: 1,
      content: 'Hello from Node.js!',
      tags: [],
      created_at: Math.floor(Date.now() / 1000),
      pubkey: ''
    });

    signer.SignEvent(event, '', 'node-app', (err, signature) => {
      if (err) {
        console.error('SignEvent error:', err);
        return;
      }
      console.log('Signature:', signature);
    });

    // NIP-44 encryption
    const recipientPubkey = 'abc123...'; // 64-char hex
    signer.NIP44Encrypt('Secret message', recipientPubkey, '', (err, ciphertext) => {
      if (err) {
        console.error('Encrypt error:', err);
        return;
      }
      console.log('Encrypted:', ciphertext);
    });
  }
);
```

### C (using GDBus)

```c
/**
 * Example: Sign a Nostr event using GNostr Signer D-Bus interface.
 * Compile: gcc -o sign_event sign_event.c $(pkg-config --cflags --libs gio-2.0)
 */

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>

#define SIGNER_BUS_NAME "org.nostr.Signer"
#define SIGNER_OBJECT_PATH "/org/nostr/signer"
#define SIGNER_INTERFACE "org.nostr.Signer"

int main(int argc, char *argv[]) {
    GError *error = NULL;
    GDBusConnection *conn;
    GVariant *result;

    // Connect to session bus
    conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error) {
        g_printerr("Failed to connect to session bus: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    // Get public key
    result = g_dbus_connection_call_sync(
        conn,
        SIGNER_BUS_NAME,
        SIGNER_OBJECT_PATH,
        SIGNER_INTERFACE,
        "GetPublicKey",
        NULL,                          // no parameters
        G_VARIANT_TYPE("(s)"),         // return type
        G_DBUS_CALL_FLAGS_NONE,
        -1,                            // default timeout
        NULL,
        &error
    );

    if (error) {
        g_printerr("GetPublicKey failed: %s\n", error->message);
        g_error_free(error);
        error = NULL;
    } else {
        const gchar *npub;
        g_variant_get(result, "(&s)", &npub);
        g_print("Public key: %s\n", npub);
        g_variant_unref(result);
    }

    // Sign an event
    const gchar *event_json =
        "{\"kind\":1,\"content\":\"Hello from C!\",\"tags\":[],\"created_at\":0,\"pubkey\":\"\"}";

    result = g_dbus_connection_call_sync(
        conn,
        SIGNER_BUS_NAME,
        SIGNER_OBJECT_PATH,
        SIGNER_INTERFACE,
        "SignEvent",
        g_variant_new("(sss)", event_json, "", "c-example"),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        30000,  // 30 second timeout for user approval
        NULL,
        &error
    );

    if (error) {
        g_printerr("SignEvent failed: %s\n", error->message);
        g_error_free(error);
    } else {
        const gchar *signature;
        g_variant_get(result, "(&s)", &signature);
        g_print("Signature: %s\n", signature);
        g_variant_unref(result);
    }

    g_object_unref(conn);
    return 0;
}
```

### C (Listening for Signals)

```c
/**
 * Example: Listen for approval signals from GNostr Signer.
 * Compile: gcc -o listen_signals listen_signals.c $(pkg-config --cflags --libs gio-2.0)
 */

#include <gio/gio.h>
#include <stdio.h>

static void on_approval_requested(GDBusConnection *conn,
                                   const gchar *sender,
                                   const gchar *object_path,
                                   const gchar *interface_name,
                                   const gchar *signal_name,
                                   GVariant *parameters,
                                   gpointer user_data) {
    const gchar *app_id, *identity, *kind, *preview, *request_id;
    g_variant_get(parameters, "(&s&s&s&s&s)",
                  &app_id, &identity, &kind, &preview, &request_id);

    g_print("Approval requested:\n");
    g_print("  App: %s\n", app_id);
    g_print("  Identity: %s\n", identity);
    g_print("  Kind: %s\n", kind);
    g_print("  Preview: %s\n", preview);
    g_print("  Request ID: %s\n", request_id);

    // In a real app, show UI and call ApproveRequest
}

static void on_approval_completed(GDBusConnection *conn,
                                   const gchar *sender,
                                   const gchar *object_path,
                                   const gchar *interface_name,
                                   const gchar *signal_name,
                                   GVariant *parameters,
                                   gpointer user_data) {
    const gchar *request_id;
    gboolean decision;
    g_variant_get(parameters, "(&sb)", &request_id, &decision);
    g_print("Approval completed: %s -> %s\n",
            request_id, decision ? "approved" : "denied");
}

int main(int argc, char *argv[]) {
    GMainLoop *loop;
    GDBusConnection *conn;
    GError *error = NULL;

    conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error) {
        g_printerr("Failed to connect: %s\n", error->message);
        return 1;
    }

    // Subscribe to ApprovalRequested
    g_dbus_connection_signal_subscribe(
        conn,
        "org.nostr.Signer",
        "org.nostr.Signer",
        "ApprovalRequested",
        "/org/nostr/signer",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_approval_requested,
        NULL,
        NULL
    );

    // Subscribe to ApprovalCompleted
    g_dbus_connection_signal_subscribe(
        conn,
        "org.nostr.Signer",
        "org.nostr.Signer",
        "ApprovalCompleted",
        "/org/nostr/signer",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_approval_completed,
        NULL,
        NULL
    );

    g_print("Listening for approval signals... (Ctrl+C to exit)\n");
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_object_unref(conn);
    return 0;
}
```

### Shell (using gdbus)

```bash
#!/bin/bash
# Example: Interact with GNostr Signer using gdbus CLI

# Get public key
gdbus call --session \
  --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.GetPublicKey

# Sign an event
EVENT='{"kind":1,"content":"Hello!","tags":[],"created_at":0,"pubkey":""}'
gdbus call --session \
  --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.SignEvent \
  "$EVENT" "" "bash-script"

# Get relays
gdbus call --session \
  --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.GetRelays

# Monitor signals
gdbus monitor --session --dest org.nostr.Signer
```

---

## D-Bus Introspection XML

The full interface definition is available at:
`/usr/share/dbus-1/interfaces/org.nostr.Signer.xml`

```xml
<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
  "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
  <interface name="org.nostr.Signer">
    <method name="GetPublicKey">
      <arg name="npub" type="s" direction="out"/>
    </method>

    <method name="SignEvent">
      <arg name="event_json" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="app_id" type="s" direction="in"/>
      <arg name="signature" type="s" direction="out"/>
    </method>

    <method name="NIP44Encrypt">
      <arg name="plaintext" type="s" direction="in"/>
      <arg name="peer_pubkey" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="ciphertext" type="s" direction="out"/>
    </method>

    <method name="NIP44Decrypt">
      <arg name="ciphertext" type="s" direction="in"/>
      <arg name="peer_pubkey" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="plaintext" type="s" direction="out"/>
    </method>

    <method name="NIP04Encrypt">
      <arg name="plaintext" type="s" direction="in"/>
      <arg name="peer_pubkey" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="ciphertext" type="s" direction="out"/>
    </method>

    <method name="NIP04Decrypt">
      <arg name="ciphertext" type="s" direction="in"/>
      <arg name="peer_pubkey" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="plaintext" type="s" direction="out"/>
    </method>

    <method name="DecryptZapEvent">
      <arg name="event_json" type="s" direction="in"/>
      <arg name="current_user" type="s" direction="in"/>
      <arg name="decrypted_event" type="s" direction="out"/>
    </method>

    <method name="GetRelays">
      <arg name="relays_json" type="s" direction="out"/>
    </method>

    <method name="StoreKey">
      <arg name="key" type="s" direction="in"/>
      <arg name="identity" type="s" direction="in"/>
      <arg name="ok" type="b" direction="out"/>
      <arg name="npub" type="s" direction="out"/>
    </method>

    <method name="ClearKey">
      <arg name="identity" type="s" direction="in"/>
      <arg name="ok" type="b" direction="out"/>
    </method>

    <method name="ApproveRequest">
      <arg name="request_id" type="s" direction="in"/>
      <arg name="decision" type="b" direction="in"/>
      <arg name="remember" type="b" direction="in"/>
      <arg name="ttl_seconds" type="t" direction="in"/>
      <arg name="ok" type="b" direction="out"/>
    </method>

    <signal name="ApprovalRequested">
      <arg type="s" name="app_id"/>
      <arg type="s" name="identity"/>
      <arg type="s" name="kind"/>
      <arg type="s" name="preview"/>
      <arg type="s" name="request_id"/>
    </signal>

    <signal name="ApprovalCompleted">
      <arg type="s" name="request_id"/>
      <arg type="b" name="decision"/>
    </signal>
  </interface>
</node>
```

---

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `NOSTR_SIGNER_ENDPOINT` | IPC endpoint (`unix:/path`, `tcp:host:port`) | `unix:$XDG_RUNTIME_DIR/gnostr/signer.sock` |
| `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS` | Enable `StoreKey`/`ClearKey` (`1` to enable) | Disabled |
| `NOSTR_SIGNER_MAX_CONNECTIONS` | Max concurrent TCP connections | 100 |
| `NOSTR_DEBUG` | Enable debug logging | Disabled |
| `NOSTR_SIGNER_SECKEY_HEX` | Fallback private key (64-char hex) | None |
| `NOSTR_SIGNER_NSEC` | Fallback private key (nsec1...) | None |

---

## ACL Configuration

Access control decisions are stored in `~/.config/gnostr/signer-acl.ini`:

```ini
[SignEvent]
app-id:npub1abc123=allow
other-app:npub1xyz789=deny:1706745600
```

Format: `app_id:identity=decision[:expiry_unix_timestamp]`

---

## Related Documentation

- [GNostr Signer README](/apps/gnostr-signer/README.md)
- [Architecture Overview](/apps/gnostr-signer/ARCHITECTURE.md)
- [Daemon Deployment Guide](/apps/gnostr-signer/DAEMON_DEPLOYMENT.md)
- [NIP-04 Migration Guide](/docs/NIP04_MIGRATION.md)
- [NIP-44 Specification](/docs/NIP44.md)
- [Systemd Hardening](/docs/systemd-hardening.md)
