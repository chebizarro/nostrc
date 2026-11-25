# Signet release notes

## 0.10.0 (Unreleased)

Release candidate `signet-v0.10.0-rc.1` was published on 2026-11-24 from
`3ff50b762` (release run 45505993545):
`ghcr.io/chebizarro/nostrc-signet@sha256:08200a3f5f4fd7c8f23758c3e6fbab60703ea75bb98cdb1fade191663bc8f97d`
(default) and `@sha256:9203e883865ce496858e13572de57cda01c927b9d96b61e5cae0d69fc70c8a6f`
(passkeys), both with verified SLSA provenance and SPDX SBOM attestations. It has
not been drilled on a Droplet; do not promote it before the deployment drills
are rerun.

### Incompatible pre-1.0 production changes

- When enabled, D-Bus TCP now binds to `127.0.0.1` instead of all IPv4
  interfaces. LAN deployments must set `[dbus] tcp_host` (or
  `SIGNET_DBUS_TCP_HOST`) explicitly. TCP `GetToken` and `GetSession` are now
  disabled by default; `tcp_allow_credentials = true` restores transport access
  only, retains normal authorization, and logs a plaintext warning. Unix D-Bus
  is unchanged. Restart to apply either new setting (`nostrc-y0q09`).

- D-Bus `GetSession` no longer mints a Signet token. It contacts the service
  bound to the agent's credential and returns the service's token. Provision
  `--service-url` on credential create/import; existing credentials have no
  endpoint. Calls without a unique binding or a working libcurl exchange fail
  closed. See the [session exchange contract](INTEGRATION.md#session-exchange).
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

### New functionality

- Provisioners can grant D-Bus `GetToken` and `GetSession` per agent through
  `set-policy`; wildcard policies do not grant either method.
- Provisioners can grant the five D-Bus passkey methods individually per agent
  through `set-policy`, including separate export and import grants.

### Other operator-visible changes

- Both release images now use digest-pinned Debian 13 (trixie) for the build and runtime (`nostrc-y01id`).
- `[nostr] reconnect_interval_s` is deprecated and ignored with a one-time
  warning. Reconnect timing continues to use libnostr backoff.

This release also includes replay, credential, backup, HTTP resource-bound and
secret/event ownership corrections from the quick-fix integration. No published
release or tag is created by this version declaration.

Signet now prunes credential leases that expired more than 24 hours ago, at
most once per hour, and logs the number removed when non-zero.
