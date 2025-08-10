# ADR-0001: NIP-47 API Surface and Envelopes

Spec source: see `SPEC_SOURCE` → `../../docs/nips/47.md`.

- Info (13194): replaceable; content lists methods; tags include `encryption` and optional `notifications`.
- Request (23194): one `p` tag (wallet pubkey), optional `expiration`, encrypted content `{ method, params }` (prefer NIP-44 v2, fallback NIP-04).
- Response (23195): one `p` tag (client pubkey), one `e` tag (request id), encrypted content `{ result_type, error{code,message}|null, result|null }`.
- Notifications: 23197 (NIP-44) or 23196 (NIP-04 legacy).

Encryption negotiation: wallet advertises `nip44_v2 nip04`; client requests preferred; mismatch → `UNSUPPORTED_ENCRYPTION`.

Implementation notes:
- Reuse libnostr JSON interface (`json.h`).
- Reuse NIP-44/NIP-04 for encryption.
- No code outside `nips/nip47/` for this feature.
