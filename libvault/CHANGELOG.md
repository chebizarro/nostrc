# libvault changelog

## 0.7.0 — Unreleased

- Add `vault_file_export_nip94` and `vault_nip94_tags_free` (interop alignment
  §2 A2, nostrc-v04jd.2): project ONE vault file for a recipient who never
  holds the vault. The file is read through the authenticated chunk reader
  with exact per-chunk epochs gated before any I/O, re-sealed as one
  AES-256-GCM blob (random 96-bit nonce, 128-bit tag appended, no AAD) under a
  fresh CSPRNG key independent of every ring and derived chunk key, uploaded
  through libblossom's signed PUT with the exact ciphertext hash pinned, and
  returned as NIP-17 kind-15 / NIP-94 (NIPs PR #2437) tags: `x`, `ox`,
  `decryption-key`, `decryption-nonce` (lowercase hex, as Quartz/Amethyst
  emit and Ditto accepts), `encryption-algorithm` = `aes-gcm`, `file-type`,
  `size` and the descriptor URL. The caller wraps them in a kind-15 rumor or
  an `imeta`; nothing is published.
- Not streamed: plaintext and ciphertext are resident together, so files above
  `VAULT_NIP94_MAX_EXPORT_BYTES` (64 MiB) are refused with
  `PORTHOME_ERR_TOO_LARGE` before any I/O. Upload failures are classified
  (`PORTHOME_ERR_HASH_MISMATCH`, `VAULT_ERR_AUTH`, `PORTHOME_ERR_TOO_LARGE`,
  `VAULT_ERR_IO`). Plaintext, key and nonce are wiped; the free wipes the tags.
- No change to the vault format, grants or rings. libporthome's sealed-blob
  layout is not used and remains a future `encryption-algorithm` value.
- MINOR: backward-compatible additive public API on top of 0.6.0.

## 0.6.0 — Unreleased

- Add explicit, default-off NIP-4E resolution to the NIP-59 grant carrier via
  `vault_nip59_carrier_set_nip4e`. Follow unmerged NIPs PR #1647 revision
  `b22711d050df662bfba5f813a35d801daf067dc8`: verify kind-10044 identity author,
  id/signature and replaceable ordering, then use its unique valid `n` key for
  both existing NIP-44 envelopes. Every configured relay must reach EOSE.
- Preserve identity-key delivery when disabled or when all relays complete with
  no announcement. Propagate failed/refused queries and reject missing EOSE;
  never silently downgrade incomplete device-key delivery to identity delivery.
  Reject invalid announcements, including a malformed latest replacement.
- Record the selected encryption recipient in the wrapper's existing NIP-59
  `p` tag. No grant-body/schema change or new cryptography.
- Add `vault_nip59_carrier_set_device_key`: receive tries the configured local
  encryption key before identity, with uniform authentication failures and
  securely disposed key copies. Existing constructors/callers remain supported.
- MINOR: backward-compatible public runtime functionality. nip44/nip59 cores and
  downstream consumers are unchanged and receive no independent version bump.
