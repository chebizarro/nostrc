# Event Schemas (nostr-homed)

This document describes the JSON payloads and tags used by nostr-homed.

Kinds:
- 30078: Profile app config (relays)
- 30079: Homedir secrets (encrypted refs)
- 30081: Homedir manifest (namespace: `personal` by default)

## Common tagging

- Namespace tagging: `{"tags":[["d","<namespace>"]]}`
- For 30078, we use `app.config:homed` as discriminator.

## 30078 (profile app config: homed)

- Purpose: Publish client-preferred relays for nostr-homed to use.
- Tagging: `tags: [["d","app.config:homed"]]`
- Content format (JSON):

```json
{
  "relays": [
    "wss://relay.damus.io",
    "wss://nostr.wine"
  ]
}
```

Notes:
- Relays discovered here override `RELAYS_DEFAULT` and are cached as `settings.relays.<namespace>`.

## 30081 (homedir.manifest)

- Purpose: List files/directories with CAS references and metadata.
- Tagging: `tags: [["d","<namespace>"]]`
- Content format (JSON, version 2):

```json
{
  "version": 2,
  "entries": [
    {
      "path": "/docs/readme.txt",
      "cid": "<sha256-hex>",
      "size": 1234,
      "mode": 420,
      "uid": 100001,
      "gid": 100001,
      "mtime": 1715800000
    }
  ],
  "links": []
}
```

Notes:
- Directories are represented with `S_IFDIR` in `mode` and may not have a `cid`.
- `mode` is stored as decimal (e.g., 420 == 0644, 384 == 0600).

## 30079 (homedir.secrets)

- Purpose: Encrypted secret references (NIP-44 envelopes) for the home namespace.
- Tagging: `tags: [["d","<namespace>"]]`
- Content format: NIP-44 envelopes serialized as JSON (provider-specific), typically a top-level JSON with encrypted refs to secrets.

Example (illustrative):

```json
{
  "version": 1,
  "entries": [
    {
      "name": "ssh/id_ed25519",
      "enc_ref": "<base64-envelope>"
    }
  ]
}
```

Notes:
- `nostr-homectl` uses the local D-Bus signer (`org.nostr.Signer`) to decrypt and writes plaintext to `/run/nostr-homed/secrets/secrets.json` (0600 in tmpfs).

## Versioning

- `manifest.version` starts at 2. Backward-incompatible changes bump the version.
- Clients should ignore unknown fields to allow forward-compatible extension.
