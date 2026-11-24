# Signet release notes

## 0.10.0 (Unreleased)

### Incompatible pre-1.0 production changes

- Disabled-profile HTTP `/bootstrap` no longer accepts a bearer token plus public
  key equality. Clients must send a signed, token-bound server-challenge event
  object in `auth_event` (purpose `signet-bootstrap`); the response rotates the
  pending pairing secret instead of disclosing it. See the README contract.
- CTAP-HID is now default-deny: `SignetFidoCtapHidConfig` requires an explicit
  `owner_group` and per-operation authorization/consent callback. Update API
  callers; device access, channel IDs and claimed request fields are not caller
  authorization. The daemon has no such backend and refuses startup with
  `virtual_ctap=true`; keep that configuration disabled when upgrading.
- Production relay configuration requires `wss://` URLs; insecure relay URLs are rejected.
- Fleet PSK inputs and audit logs must be protected regular files. Unsafe ownership,
  permissions and symlink destinations are rejected; correct these before upgrading.
- A configured passkey service that is unavailable now prevents startup instead of
  silently continuing without that service.

### Other operator-visible changes

- `[nostr] reconnect_interval_s` is deprecated and ignored with a one-time
  warning. Reconnect timing continues to use libnostr backoff.

This release also includes replay, credential, backup, HTTP resource-bound and
secret/event ownership corrections from the quick-fix integration. No published
release or tag is created by this version declaration.
