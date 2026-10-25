# libvault-marmot

Optional Layer-5 Marmot adapter for libvault. It links libvault and libmarmot;
libvault itself has no MLS dependency. Enable `BUILD_LIBVAULT_MARMOT` (default ON
when both prerequisites are enabled). An explicitly enabled adapter with a missing
prerequisite fails configuration. Public API: `<vault-marmot/vault-marmot.h>`,
CMake target `nostrc::vault-marmot`, pkg-config `vault-marmot` and installed
`find_package(nostrc REQUIRED COMPONENTS vault-marmot)`.

`vault_marmot_seed()` calls libmarmot's public RFC 9420 §8.5 exporter over the
current canonical group epoch with exactly:

```
label   = "porthome/v2/vault"  (no NUL)
context = vault_id            (32 raw bytes, not hex)
L       = 32
```

Pass that secret seed to `porthome_root_node_key()`. Wipe the seed after use.
Add/Remove commits change the epoch and therefore its seed. No alternate label,
transport-key derivation or fallback is used. `docs/MLS_INTEGRATION.md` does not
specify a conflicting exporter label/policy; its MIP-03 encryption key is a
separate domain. This application label is not claimed to be IANA-registered.

Construct an adapter with the already-joined group, vault ID and explicit
transport callbacks. Use its `vault_grant_carrier` interface with
`vault_grant_send()` / `vault_grant_receive()`. Only signed kind-445 ciphertext
reaches transport callbacks; the unsigned kind-30804 rumor stays inside MLS.
The body is unchanged from the NIP-59 carrier. A grant is visible to **every group
member**, not just the named recipient. The application owns membership/access
alignment, commit publication/merge, Welcome delivery and secret persistence.
The adapter requires proven account leaves and checks sender/owner, recipient
membership, group routing and vault ID. Since the message API reports the account
rather than the particular sending leaf, every current leaf claiming the sender
or recipient account must be proven; mixed proven/unproven accounts are refused.
Grants from retained epochs are also refused: today's leaf proofs cannot establish
an old sender's proof. Reissue delayed grants in the current epoch after catch-up. It does not persist plaintext outgoing
grants. Calls on a Marmot instance must be serialized and its storage transaction
must be durable before publishing (see `marmot_create_message()`'s contract).

`vault_marmot_prefer()` takes the **result** of the caller's recipient-scoped
kind-443 key-package lookup on the vault's relays: PRESENT selects Marmot, ABSENT
selects NIP-59, UNKNOWN returns AGAIN. A timeout/error is not absence; use the
normal EOSE-aware subscription path. Current libmarmot produces kind-30443 key
packages; this helper implements the design's literal kind-443 preference input,
not a new key-package discovery or admission policy.

Callbacks own network transport: relay OK acceptance, OK false, AUTH, CLOSED,
EOSE, reconnect/backoff, event-id dedup and any persistent delivery state. This
adapter contacts no relays, retries no uncertain writes and performs no polling.
Commit/proposal/own-message results are handled once by libmarmot and yield AGAIN,
not a grant. Applications sharing a group with other message types must route
those explicitly; this adapter's incoming path is for vault grants and group
control messages. A rejected already-decrypted application message may consume
its MLS generation; do not offer one ciphertext to multiple vault adapters.

## Focused tests

Build `test_vault_grant_marmot` and `test_exporter`, then run:

```sh
ctest --test-dir BUILD --no-tests=error --output-on-failure \
  -R '^(test_vault_grant_marmot|marmot_test_exporter)$'
```

These use real MLS groups, Add/Remove commits and the real NIP-59 carrier, with
recording transport callbacks. They do not prove live relay ACKs/delivery.
