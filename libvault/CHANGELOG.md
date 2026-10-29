# libvault changelog

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
