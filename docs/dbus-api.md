# GNostr Signer D-Bus API Reference

This document provides comprehensive documentation for the `org.nostr.Signer` D-Bus interface, which enables secure Nostr key management and cryptographic operations for desktop applications.

## Overview

**Bus Name:** `org.nostr.Signer`
**Object Path:** `/org/nostr/signer`
**Interface:** `org.nostr.Signer`

The GNostr Signer D-Bus interface provides:

- Secure key storage using system keyrings (libsecret on Linux, Keychain on macOS)
- Event signing with user approval workflow
- NIP-04 and NIP-44 encryption/decryption
- NIP-46 bunker protocol integration
- Rate limiting to prevent abuse
- Session management for client applications

Private keys never leave the daemon process boundary. All sensitive operations are protected by user approval dialogs.

---

## Table of Contents

1. [Methods](#methods)
   - [Identity Methods](#identity-methods)
   - [Signing Methods](#signing-methods)
   - [Encryption Methods](#encryption-methods)
   - [Key Management Methods](#key-management-methods)
   - [User Approval Methods](#user-approval-methods)
2. [Signals](#signals)
3. [Properties](#properties)
4. [Error Codes](#error-codes)
5. [Usage Examples](#usage-examples)
   - [gdbus Command Line](#gdbus-command-line)
   - [Python (PyGObject)](#python-pygobject)
   - [Python (dbus-python)](#python-dbus-python)
   - [C (GDBus)](#c-gdbus)
   - [Rust (zbus)](#rust-zbus)
6. [NIP-46 Bunker Protocol Mapping](#nip-46-bunker-protocol-mapping)
7. [Security Considerations](#security-considerations)
8. [Integration Guide](#integration-guide)

---

## Methods

### Identity Methods

#### GetPublicKey

Returns the current user's public key (npub) according to identity selector rules.

**Signature:**
```xml
<method name="GetPublicKey">
  <arg name="npub" type="s" direction="out"/>
</method>
```

**Parameters:** None

**Returns:**
- `npub` (string): Bech32-encoded public key (`npub1...`)

**Errors:**
- `org.nostr.Signer.Error.Internal`: No key configured or key retrieval failed

**Example:**
```bash
gdbus call --session --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.GetPublicKey
```

---

### Signing Methods

#### SignEvent

Signs a Nostr event JSON and returns the Schnorr signature. May trigger a user approval dialog if no cached ACL decision exists.

**Signature:**
```xml
<method name="SignEvent">
  <arg name="event_json" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="app_id" type="s" direction="in"/>
  <arg name="signature" type="s" direction="out"/>
</method>
```

**Parameters:**
- `event_json` (string): JSON-serialized Nostr event (without signature field)
- `current_user` (string): Identity selector - npub, key_id, or empty string for default identity
- `app_id` (string): Application identifier for ACL. Uses D-Bus sender if empty

**Returns:**
- `signature` (string): Hex-encoded 64-byte Schnorr signature

**Errors:**
- `org.nostr.Signer.Error.InvalidInput`: Malformed event JSON
- `org.nostr.Signer.Error.ApprovalDenied`: User denied the signing request
- `org.nostr.Signer.Error.RateLimited`: Rate limit exceeded (100ms cooldown between requests)
- `org.nostr.Signer.Error.Internal`: Signing operation failed

**Example Event JSON:**
```json
{
  "kind": 1,
  "created_at": 1706054400,
  "tags": [],
  "content": "Hello, Nostr!",
  "pubkey": "abc123..."
}
```

---

### Encryption Methods

#### NIP44Encrypt

Encrypts a message using NIP-44 v2 (XChaCha20-Poly1305). This is the recommended encryption method for new implementations.

**Signature:**
```xml
<method name="NIP44Encrypt">
  <arg name="plaintext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="ciphertext" type="s" direction="out"/>
</method>
```

**Parameters:**
- `plaintext` (string): UTF-8 message to encrypt
- `peer_pubkey` (string): Recipient's public key (64-character hex)
- `current_user` (string): Identity selector

**Returns:**
- `ciphertext` (string): Base64-encoded NIP-44 v2 ciphertext

---

#### NIP44Decrypt

Decrypts a message using NIP-44 v2.

**Signature:**
```xml
<method name="NIP44Decrypt">
  <arg name="ciphertext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="plaintext" type="s" direction="out"/>
</method>
```

**Parameters:**
- `ciphertext` (string): Base64-encoded NIP-44 v2 ciphertext
- `peer_pubkey` (string): Sender's public key (64-character hex)
- `current_user` (string): Identity selector

**Returns:**
- `plaintext` (string): Decrypted UTF-8 message

---

#### NIP04Encrypt

Encrypts a message using NIP-04 (AES-256-CBC). **Deprecated:** Prefer NIP-44 for new implementations.

**Signature:**
```xml
<method name="NIP04Encrypt">
  <arg name="plaintext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="ciphertext" type="s" direction="out"/>
</method>
```

**Parameters:** Same as NIP44Encrypt

**Returns:**
- `ciphertext` (string): Ciphertext with IV appended

---

#### NIP04Decrypt

Decrypts a message using NIP-04. **Deprecated:** Prefer NIP-44 for new implementations.

**Signature:**
```xml
<method name="NIP04Decrypt">
  <arg name="ciphertext" type="s" direction="in"/>
  <arg name="peer_pubkey" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="plaintext" type="s" direction="out"/>
</method>
```

---

#### DecryptZapEvent

Decrypts the content of a zap receipt event (NIP-57). Tries NIP-44 first, then falls back to NIP-04.

**Signature:**
```xml
<method name="DecryptZapEvent">
  <arg name="event_json" type="s" direction="in"/>
  <arg name="current_user" type="s" direction="in"/>
  <arg name="decrypted_event" type="s" direction="out"/>
</method>
```

**Parameters:**
- `event_json` (string): JSON-serialized zap event
- `current_user` (string): Identity selector

**Returns:**
- `decrypted_event` (string): Event JSON with decrypted content

---

#### GetRelays

Returns the list of configured relay URLs.

**Signature:**
```xml
<method name="GetRelays">
  <arg name="relays_json" type="s" direction="out"/>
</method>
```

**Returns:**
- `relays_json` (string): JSON array of relay URLs

---

### Key Management Methods

#### StoreKey

Stores a private key in the secure backend (libsecret/Keychain).

**Security:** Requires `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1` environment variable to be set.

**Signature:**
```xml
<method name="StoreKey">
  <arg name="key" type="s" direction="in"/>
  <arg name="identity" type="s" direction="in"/>
  <arg name="ok" type="b" direction="out"/>
  <arg name="npub" type="s" direction="out"/>
</method>
```

**Parameters:**
- `key` (string): Private key (64-character hex or `nsec1...` bech32)
- `identity` (string): Optional identity label/selector

**Returns:**
- `ok` (boolean): True if stored successfully
- `npub` (string): Derived public key (`npub1...`)

**Errors:**
- `org.nostr.Signer.Error.PermissionDenied`: Key mutations disabled
- `org.nostr.Signer.Error.RateLimited`: Rate limit exceeded (500ms cooldown)
- `org.nostr.Signer.InvalidKey`: Invalid private key format
- `org.nostr.Signer.SecretServiceUnavailable`: Keyring backend unavailable

---

#### ClearKey

Removes a private key from the secure backend.

**Security:** Requires `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1` environment variable.

**Signature:**
```xml
<method name="ClearKey">
  <arg name="identity" type="s" direction="in"/>
  <arg name="ok" type="b" direction="out"/>
</method>
```

**Parameters:**
- `identity` (string): Identity selector (npub or key_id)

**Returns:**
- `ok` (boolean): True if removed successfully

---

### User Approval Methods

#### ApproveRequest

Responds to a pending approval request. Called by the UI to approve or deny a signing operation.

**Signature:**
```xml
<method name="ApproveRequest">
  <arg name="request_id" type="s" direction="in"/>
  <arg name="decision" type="b" direction="in"/>
  <arg name="remember" type="b" direction="in"/>
  <arg name="ttl_seconds" type="t" direction="in"/>
  <arg name="ok" type="b" direction="out"/>
</method>
```

**Parameters:**
- `request_id` (string): ID from `ApprovalRequested` signal
- `decision` (boolean): True to approve, false to deny
- `remember` (boolean): Save decision to ACL file
- `ttl_seconds` (uint64): How long to remember the decision (0 = forever)

**Returns:**
- `ok` (boolean): True if request was found and processed

---

## Signals

### ApprovalRequested

Emitted when a signing operation requires user approval.

**Signature:**
```xml
<signal name="ApprovalRequested">
  <arg type="s" name="app_id"/>
  <arg type="s" name="identity"/>
  <arg type="s" name="kind"/>
  <arg type="s" name="preview"/>
  <arg type="s" name="request_id"/>
</signal>
```

**Arguments:**
- `app_id` (string): Requesting application identifier
- `identity` (string): Target identity (npub)
- `kind` (string): Request type (e.g., "event", "encrypt", "decrypt")
- `preview` (string): Human-readable preview of the content (truncated to ~96 characters)
- `request_id` (string): Unique ID to use with `ApproveRequest`

---

### ApprovalCompleted

Emitted when an approval request has been resolved.

**Signature:**
```xml
<signal name="ApprovalCompleted">
  <arg type="s" name="request_id"/>
  <arg type="b" name="decision"/>
</signal>
```

**Arguments:**
- `request_id` (string): The request that was completed
- `decision` (boolean): True if approved, false if denied

---

## Properties

The GNostr Signer exposes configuration through GSettings (`org.gnostr.Signer`). Key settings include:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `default-identity` | string | `''` | Default identity npub |
| `lock-timeout-sec` | int | `300` | Auto-lock timeout in seconds |
| `remember-approvals` | bool | `false` | Remember app approvals |
| `approval-ttl-hours` | int | `24` | Hours to remember approvals |
| `bunker-enabled` | bool | `false` | Enable NIP-46 bunker |
| `bunker-relays` | string array | `[]` | Relays for bunker requests |
| `bunker-allowed-pubkeys` | string array | `[]` | Allowed client pubkeys |
| `bunker-allowed-methods` | string array | `[...]` | Allowed NIP-46 methods |
| `bunker-auto-approve-kinds` | string array | `['1','6','7']` | Auto-approve event kinds |
| `client-session-timeout-sec` | int | `900` | Client session timeout (15 min) |

---

## Error Codes

| Error Code | Description |
|------------|-------------|
| `org.nostr.Signer.Error.Internal` | Internal error (key retrieval, signing failure) |
| `org.nostr.Signer.Error.InvalidInput` | Malformed input (bad JSON, invalid parameters) |
| `org.nostr.Signer.Error.ApprovalDenied` | User denied the operation |
| `org.nostr.Signer.Error.RateLimited` | Too many requests, try again later |
| `org.nostr.Signer.Error.PermissionDenied` | Operation not permitted (e.g., key mutations disabled) |
| `org.nostr.Signer.Error.SessionExpired` | Client session has expired |
| `org.nostr.Signer.Error.CryptoFailed` | Cryptographic operation failed |
| `org.nostr.Signer.InvalidKey` | Invalid key format |
| `org.nostr.Signer.SecretServiceUnavailable` | Keyring backend unavailable |

---

## Usage Examples

### gdbus Command Line

**Get public key:**
```bash
gdbus call --session --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.GetPublicKey
```

**Sign an event:**
```bash
EVENT_JSON='{"kind":1,"created_at":1706054400,"tags":[],"content":"Hello!","pubkey":"..."}'

gdbus call --session --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.SignEvent \
  "$EVENT_JSON" "" ""
```

**Encrypt a message (NIP-44):**
```bash
gdbus call --session --dest org.nostr.Signer \
  --object-path /org/nostr/signer \
  --method org.nostr.Signer.NIP44Encrypt \
  "Secret message" \
  "abc123def456..." \
  ""
```

**Monitor approval signals:**
```bash
gdbus monitor --session --dest org.nostr.Signer \
  --object-path /org/nostr/signer
```

---

### Python (PyGObject)

```python
#!/usr/bin/env python3
"""
Example: Using GNostr Signer D-Bus API with PyGObject
"""
import json
import time
from gi.repository import Gio, GLib

# Connect to the session bus
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

# Create a proxy for the signer interface
proxy = Gio.DBusProxy.new_sync(
    bus,
    Gio.DBusProxyFlags.NONE,
    None,  # GDBusInterfaceInfo
    "org.nostr.Signer",
    "/org/nostr/signer",
    "org.nostr.Signer",
    None  # GCancellable
)


def get_public_key():
    """Get the current user's public key."""
    result = proxy.call_sync(
        "GetPublicKey",
        None,  # No parameters
        Gio.DBusCallFlags.NONE,
        -1,  # Default timeout
        None
    )
    npub = result.unpack()[0]
    print(f"Public key: {npub}")
    return npub


def sign_event(event_dict, identity="", app_id=""):
    """Sign a Nostr event."""
    event_json = json.dumps(event_dict)

    try:
        result = proxy.call_sync(
            "SignEvent",
            GLib.Variant("(sss)", (event_json, identity, app_id)),
            Gio.DBusCallFlags.NONE,
            30000,  # 30 second timeout for user approval
            None
        )
        signature = result.unpack()[0]
        print(f"Signature: {signature}")
        return signature
    except GLib.Error as e:
        print(f"Signing failed: {e.message}")
        return None


def encrypt_nip44(plaintext, peer_pubkey, identity=""):
    """Encrypt a message using NIP-44."""
    result = proxy.call_sync(
        "NIP44Encrypt",
        GLib.Variant("(sss)", (plaintext, peer_pubkey, identity)),
        Gio.DBusCallFlags.NONE,
        -1,
        None
    )
    ciphertext = result.unpack()[0]
    return ciphertext


def decrypt_nip44(ciphertext, peer_pubkey, identity=""):
    """Decrypt a message using NIP-44."""
    result = proxy.call_sync(
        "NIP44Decrypt",
        GLib.Variant("(sss)", (ciphertext, peer_pubkey, identity)),
        Gio.DBusCallFlags.NONE,
        -1,
        None
    )
    plaintext = result.unpack()[0]
    return plaintext


# Example usage
if __name__ == "__main__":
    # Get public key
    npub = get_public_key()

    # Create and sign an event
    event = {
        "kind": 1,
        "created_at": int(time.time()),
        "tags": [],
        "content": "Hello from Python!",
        "pubkey": npub.replace("npub1", "")  # Convert to hex if needed
    }

    signature = sign_event(event)
    if signature:
        event["sig"] = signature
        print(f"Signed event: {json.dumps(event, indent=2)}")
```

---

### Python (dbus-python)

```python
#!/usr/bin/env python3
"""
Example: Using GNostr Signer D-Bus API with dbus-python
"""
import dbus
import json
import time

# Connect to the session bus
bus = dbus.SessionBus()

# Get the signer object
signer = bus.get_object("org.nostr.Signer", "/org/nostr/signer")
interface = dbus.Interface(signer, "org.nostr.Signer")


def get_public_key():
    """Get the current user's public key."""
    npub = interface.GetPublicKey()
    return str(npub)


def sign_event(event_dict, identity="", app_id=""):
    """Sign a Nostr event."""
    event_json = json.dumps(event_dict)
    try:
        signature = interface.SignEvent(event_json, identity, app_id)
        return str(signature)
    except dbus.exceptions.DBusException as e:
        print(f"Error: {e.get_dbus_name()}: {e.get_dbus_message()}")
        return None


def encrypt_nip44(plaintext, peer_pubkey, identity=""):
    """Encrypt using NIP-44."""
    return str(interface.NIP44Encrypt(plaintext, peer_pubkey, identity))


def decrypt_nip44(ciphertext, peer_pubkey, identity=""):
    """Decrypt using NIP-44."""
    return str(interface.NIP44Decrypt(ciphertext, peer_pubkey, identity))


# Signal handler for approval requests
def on_approval_requested(app_id, identity, kind, preview, request_id):
    print(f"Approval requested:")
    print(f"  App: {app_id}")
    print(f"  Identity: {identity}")
    print(f"  Kind: {kind}")
    print(f"  Preview: {preview}")
    print(f"  Request ID: {request_id}")


# Subscribe to signals
signer.connect_to_signal(
    "ApprovalRequested",
    on_approval_requested,
    dbus_interface="org.nostr.Signer"
)


if __name__ == "__main__":
    npub = get_public_key()
    print(f"Public key: {npub}")

    # Sign an event
    event = {
        "kind": 1,
        "created_at": int(time.time()),
        "tags": [],
        "content": "Hello via dbus-python!",
    }

    sig = sign_event(event)
    if sig:
        print(f"Signature: {sig}")
```

---

### C (GDBus)

```c
/* Example: Using GNostr Signer D-Bus API from C */
#include <gio/gio.h>
#include <stdio.h>

#define SIGNER_NAME "org.nostr.Signer"
#define SIGNER_PATH "/org/nostr/signer"
#define SIGNER_IFACE "org.nostr.Signer"

static GDBusProxy *proxy = NULL;

gboolean
signer_init(GError **error)
{
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
    if (!conn)
        return FALSE;

    proxy = g_dbus_proxy_new_sync(conn,
                                   G_DBUS_PROXY_FLAGS_NONE,
                                   NULL,
                                   SIGNER_NAME,
                                   SIGNER_PATH,
                                   SIGNER_IFACE,
                                   NULL,
                                   error);
    g_object_unref(conn);
    return proxy != NULL;
}

gchar *
signer_get_public_key(GError **error)
{
    GVariant *result = g_dbus_proxy_call_sync(proxy,
                                               "GetPublicKey",
                                               NULL,
                                               G_DBUS_CALL_FLAGS_NONE,
                                               -1,
                                               NULL,
                                               error);
    if (!result)
        return NULL;

    gchar *npub = NULL;
    g_variant_get(result, "(s)", &npub);
    g_variant_unref(result);

    return npub;
}

gchar *
signer_sign_event(const gchar *event_json,
                  const gchar *identity,
                  const gchar *app_id,
                  GError **error)
{
    GVariant *params = g_variant_new("(sss)",
                                      event_json,
                                      identity ? identity : "",
                                      app_id ? app_id : "");

    GVariant *result = g_dbus_proxy_call_sync(proxy,
                                               "SignEvent",
                                               params,
                                               G_DBUS_CALL_FLAGS_NONE,
                                               30000,  /* 30s timeout */
                                               NULL,
                                               error);
    if (!result)
        return NULL;

    gchar *signature = NULL;
    g_variant_get(result, "(s)", &signature);
    g_variant_unref(result);

    return signature;
}

/* Signal handler for approval requests */
static void
on_signal(GDBusProxy *proxy,
          const gchar *sender_name,
          const gchar *signal_name,
          GVariant *parameters,
          gpointer user_data)
{
    if (g_strcmp0(signal_name, "ApprovalRequested") == 0) {
        const gchar *app_id, *identity, *kind, *preview, *request_id;
        g_variant_get(parameters, "(&s&s&s&s&s)",
                      &app_id, &identity, &kind, &preview, &request_id);

        g_print("Approval requested:\n");
        g_print("  App: %s\n", app_id);
        g_print("  Identity: %s\n", identity);
        g_print("  Kind: %s\n", kind);
        g_print("  Preview: %s\n", preview);
        g_print("  Request ID: %s\n", request_id);
    }
}

int
main(int argc, char *argv[])
{
    GError *error = NULL;

    if (!signer_init(&error)) {
        g_printerr("Failed to connect: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    /* Connect to signals */
    g_signal_connect(proxy, "g-signal", G_CALLBACK(on_signal), NULL);

    /* Get public key */
    gchar *npub = signer_get_public_key(&error);
    if (npub) {
        g_print("Public key: %s\n", npub);
        g_free(npub);
    } else {
        g_printerr("GetPublicKey failed: %s\n", error->message);
        g_clear_error(&error);
    }

    /* Sign an event */
    const gchar *event = "{\"kind\":1,\"created_at\":1706054400,"
                         "\"tags\":[],\"content\":\"Hello from C!\"}";

    gchar *sig = signer_sign_event(event, NULL, NULL, &error);
    if (sig) {
        g_print("Signature: %s\n", sig);
        g_free(sig);
    } else {
        g_printerr("SignEvent failed: %s\n", error->message);
        g_clear_error(&error);
    }

    g_object_unref(proxy);
    return 0;
}
```

**Compile with:**
```bash
gcc -o signer-example signer-example.c $(pkg-config --cflags --libs gio-2.0)
```

---

### Rust (zbus)

```rust
//! Example: Using GNostr Signer D-Bus API from Rust with zbus

use zbus::{Connection, Result, proxy};

#[proxy(
    interface = "org.nostr.Signer",
    default_service = "org.nostr.Signer",
    default_path = "/org/nostr/signer"
)]
trait Signer {
    /// Get the current user's public key
    fn get_public_key(&self) -> Result<String>;

    /// Sign a Nostr event
    fn sign_event(
        &self,
        event_json: &str,
        current_user: &str,
        app_id: &str,
    ) -> Result<String>;

    /// Encrypt using NIP-44
    fn nip44_encrypt(
        &self,
        plaintext: &str,
        peer_pubkey: &str,
        current_user: &str,
    ) -> Result<String>;

    /// Decrypt using NIP-44
    fn nip44_decrypt(
        &self,
        ciphertext: &str,
        peer_pubkey: &str,
        current_user: &str,
    ) -> Result<String>;

    /// Signal: Approval requested
    #[zbus(signal)]
    fn approval_requested(
        &self,
        app_id: &str,
        identity: &str,
        kind: &str,
        preview: &str,
        request_id: &str,
    ) -> Result<()>;
}

#[tokio::main]
async fn main() -> Result<()> {
    // Connect to the session bus
    let connection = Connection::session().await?;

    // Create the proxy
    let proxy = SignerProxy::new(&connection).await?;

    // Get public key
    let npub = proxy.get_public_key().await?;
    println!("Public key: {}", npub);

    // Sign an event
    let event_json = r#"{
        "kind": 1,
        "created_at": 1706054400,
        "tags": [],
        "content": "Hello from Rust!"
    }"#;

    match proxy.sign_event(event_json, "", "").await {
        Ok(signature) => println!("Signature: {}", signature),
        Err(e) => eprintln!("Signing failed: {}", e),
    }

    Ok(())
}
```

**Cargo.toml:**
```toml
[dependencies]
zbus = "4"
tokio = { version = "1", features = ["macros", "rt-multi-thread"] }
```

---

## NIP-46 Bunker Protocol Mapping

The GNostr Signer implements NIP-46 (Nostr Connect) for remote signing. The D-Bus interface maps to NIP-46 methods as follows:

| NIP-46 Method | D-Bus Method | Description |
|---------------|--------------|-------------|
| `connect` | (handled internally) | Client connection via `nostrconnect://` URI |
| `get_public_key` | `GetPublicKey` | Retrieve signer's public key |
| `sign_event` | `SignEvent` | Sign a Nostr event |
| `nip04_encrypt` | `NIP04Encrypt` | Encrypt (legacy NIP-04) |
| `nip04_decrypt` | `NIP04Decrypt` | Decrypt (legacy NIP-04) |
| `nip44_encrypt` | `NIP44Encrypt` | Encrypt (modern NIP-44) |
| `nip44_decrypt` | `NIP44Decrypt` | Decrypt (modern NIP-44) |

### Bunker URI Formats

**Bunker URI (signer shares with client):**
```
bunker://<signer-pubkey-hex>?relay=wss://relay1.example&relay=wss://relay2.example&secret=<optional-secret>
```

**Nostr Connect URI (client shares with signer):**
```
nostrconnect://<client-pubkey-hex>?relay=wss://relay.example&metadata=<app-metadata>
```

### Session Management

NIP-46 client sessions are managed via `GnClientSessionManager`:

- Sessions track authenticated remote applications
- Default timeout: 15 minutes of inactivity
- Sessions can be persistent (remembered across restarts)
- Users can revoke individual sessions or all sessions

**Session States:**
- `PENDING`: Awaiting user approval
- `ACTIVE`: Session is valid and usable
- `EXPIRED`: Session timed out due to inactivity
- `REVOKED`: Session manually revoked by user

**Permissions:**
- `GN_PERM_CONNECT` (0x01): Basic connection
- `GN_PERM_GET_PUBLIC_KEY` (0x02): Retrieve public key
- `GN_PERM_SIGN_EVENT` (0x04): Sign events
- `GN_PERM_ENCRYPT` (0x08): Encrypt messages
- `GN_PERM_DECRYPT` (0x10): Decrypt messages

### Auto-Approval

Certain event kinds can be auto-approved without user interaction:

| Kind | Description | Default Auto-Approve |
|------|-------------|---------------------|
| 1 | Short text note | Yes |
| 6 | Repost | Yes |
| 7 | Reaction | Yes |
| 4 | Encrypted DM | No (requires confirmation) |
| 30023 | Long-form content | No (requires confirmation) |

Configure via GSettings `bunker-auto-approve-kinds`.

---

## Security Considerations

### Private Key Protection

1. **Process Isolation**: Private keys never leave the daemon process boundary
2. **Secure Storage**: Keys stored in system keyring (libsecret/Keychain)
3. **Memory Protection**: Sensitive data cleared from memory after use
4. **No Key Export**: D-Bus interface does not expose private keys

### Rate Limiting

The signer implements rate limiting to prevent abuse:

| Operation | Cooldown | Max Attempts | Lockout |
|-----------|----------|--------------|---------|
| SignEvent | 100ms | 5 per minute | Exponential backoff |
| StoreKey | 500ms | 3 per minute | 5 minute lockout |
| Client Auth | Varies | 5 attempts | Exponential (1s, 2s, 4s... up to 5 min) |

### ACL and Approval Flow

1. **First Request**: Triggers user approval dialog
2. **Remembered Decisions**: Stored in ACL with configurable TTL
3. **Always Confirm**: Some event kinds (DMs, long-form) always require confirmation
4. **Revocation**: Users can revoke approvals at any time

### Environment Variable Security

| Variable | Purpose | Security Note |
|----------|---------|---------------|
| `NOSTR_SIGNER_ALLOW_KEY_MUTATIONS` | Enable key storage/deletion | Required for StoreKey/ClearKey |
| `NOSTR_SIGNER_ENDPOINT` | Custom IPC endpoint | Use for sandboxed deployments |

### D-Bus Security

- Uses D-Bus authentication (credentials passing)
- Each caller identified by unique bus name
- App-specific ACLs based on D-Bus sender
- Session bus isolation (per-user)

---

## Integration Guide

### For Desktop Application Developers

**Step 1: Check if signer is available**
```bash
gdbus introspect --session --dest org.nostr.Signer \
  --object-path /org/nostr/signer
```

**Step 2: Handle the approval flow**
```python
# Option 1: Synchronous (blocks until user responds)
try:
    signature = interface.SignEvent(event_json, "", "")
except dbus.exceptions.DBusException as e:
    if "ApprovalDenied" in e.get_dbus_name():
        print("User denied the request")
    elif "RateLimited" in e.get_dbus_name():
        print("Too many requests, slow down")

# Option 2: Subscribe to signals for async workflow
bus.add_signal_receiver(
    on_approval_completed,
    signal_name="ApprovalCompleted",
    dbus_interface="org.nostr.Signer"
)
```

**Step 3: Implement graceful degradation**
```python
def sign_with_fallback(event):
    try:
        # Try D-Bus signer first
        return dbus_sign(event)
    except dbus.exceptions.DBusException:
        # Fall back to local signing (if user has local key)
        return local_sign(event)
```

### For NIP-46 Client Developers

**Step 1: Generate nostrconnect:// URI**
```python
import secrets

client_privkey = generate_keypair()
client_pubkey = derive_pubkey(client_privkey)
relay = "wss://relay.example.com"

uri = f"nostrconnect://{client_pubkey}?relay={relay}&metadata={{\"name\":\"MyApp\"}}"
# Display as QR code or copy-paste for user
```

**Step 2: Listen for responses on relay**
```python
# Subscribe to events addressed to your client pubkey
# Parse NIP-46 responses and handle accordingly
```

### For Extension/Plugin Developers

When building browser extensions or plugins:

1. Use native messaging to communicate with a local bridge
2. The bridge connects to D-Bus and forwards requests
3. Handle approval dialogs gracefully (may need focus)

Example native messaging host:
```python
#!/usr/bin/env python3
import json
import struct
import sys
import dbus

def send_message(message):
    encoded = json.dumps(message).encode('utf-8')
    sys.stdout.buffer.write(struct.pack('@I', len(encoded)))
    sys.stdout.buffer.write(encoded)
    sys.stdout.buffer.flush()

def read_message():
    raw_length = sys.stdin.buffer.read(4)
    length = struct.unpack('@I', raw_length)[0]
    return json.loads(sys.stdin.buffer.read(length))

# Connect to signer
bus = dbus.SessionBus()
signer = dbus.Interface(
    bus.get_object("org.nostr.Signer", "/org/nostr/signer"),
    "org.nostr.Signer"
)

while True:
    msg = read_message()
    if msg['method'] == 'getPublicKey':
        npub = signer.GetPublicKey()
        send_message({'npub': str(npub)})
    elif msg['method'] == 'signEvent':
        sig = signer.SignEvent(json.dumps(msg['event']), "", "")
        send_message({'signature': str(sig)})
```

---

## Appendix: D-Bus Interface XML

The complete interface definition is available at:
`data/dbus/org.nostr.Signer.xml`

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
    <!-- ... see full XML for complete interface -->
  </interface>
</node>
```

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2024-01 | Initial D-Bus interface |
| 1.1 | 2024-01 | Added NIP-44 support, rate limiting |
| 1.2 | 2024-01 | Added client session management (nostrc-09n) |

---

## See Also

- [NIP-46: Nostr Connect](https://github.com/nostr-protocol/nips/blob/master/46.md)
- [NIP-04: Encrypted Direct Message](https://github.com/nostr-protocol/nips/blob/master/04.md)
- [NIP-44: Versioned Encryption](https://github.com/nostr-protocol/nips/blob/master/44.md)
- [NIP-57: Lightning Zaps](https://github.com/nostr-protocol/nips/blob/master/57.md)
- [D-Bus Specification](https://dbus.freedesktop.org/doc/dbus-specification.html)
