# libvault (experimental, 0.5.0)

Layer-5 shared-vault policy over libporthome. Public API: `<vault/vault.h>`;
in-tree target: `nostrc::vault`. Installed consumers use
`find_package(nostrc REQUIRED COMPONENTS vault)` or
`pkg-config --static --cflags --libs vault`. Dependencies are libporthome,
libblossom, libnostr/NIP-44/NIP-59 and OpenSSL, with no GLib or libmarmot requirement.
For a GLib-free closure configure libnostr with `NOSTR_WITH_GLIB=OFF`.
Enable `BUILD_LIBVAULT`, `BUILD_LIBPORTHOME`, `BUILD_LIBBLOSSOM`, `ENABLE_NIP44`
and `ENABLE_NIP59`.
Standalone configuration uses the installed dependency prefix through
`CMAKE_PREFIX_PATH` and `PKG_CONFIG_PATH`.

## Rings and revocation

An owned `vault_ring` holds ordered, nonzero epochs with distinct keys. Initialize
or copy into a fresh zeroed destination; dispose before replacing it. Appends use
latest epoch + 1, either with caller-supplied exporter material or a fresh random
key. A borrowed `porthome_ring_t` retains PCS's exact-epoch, no-fallback behavior.
Dropping history is explicit, not an automatic effect of revocation.

`vault_revoke` returns an immutable proposal for §6.4 steps 1–3. It checks the
owner actor identity, the current node address and epoch, then clones the node
and ring, appends a random epoch, removes the member from both informational
readers and authorized writers, increments `grants_rev`, and seals the new node.
An unlisted reader can still be revoked: possession of keys, not readers[], is
the read capability. The owner cannot revoke itself with this operation.

For a subtree, the result contains the updated parent directory entry with the
new override, child epoch and address. The caller must seal its parent and all
changed ancestors. For a root, the input must be the owner's previously verified
head; the result advances its sequence, references it as parent, and includes a
head payload sealed **only under the new key**. The optional head `key_override`
is never sealed with an old key. A grant/exporter must deliver the new key.
An actor pubkey comparison does not establish key custody: the owner's actual
signed publication is what makes the new writer policy authoritative.
Carrier delivery, server allowlist updates and relay publication are not part of
this proposal API.

## Budgeted re-seal

A `vault_reseal_task` takes explicit per-chunk jobs over borrowed mutable nodes
and their rings. Nodes/rings must remain alive and unchanged outside the task.
True plaintext chunk lengths come from each authenticated manifest descriptor;
CDC lengths cannot be inferred from padded sizes or file totals, and trimming
trailing zeros corrupts real data. The task verifies each descriptor's length by
canonical old-key re-sealing before uploading anything.

Lazy mode executes an affordable prefix; eager mode requires the entire remaining
budget before any I/O. Costs reserve download plus upload payload bytes, not HTTP
headers or service overhead. Hashes are checked before decryption. Metadata is
updated only after `put` confirms durable, hash-verified acceptance. Failures are
terminal with no implicit retry; an uncertain upload exposes its pending address
for caller reconciliation. Callbacks own transport/authentication and return
malloc'd read buffers. The task never deletes old objects.

After successful work the caller must seal the mutated manifests and ancestors.
Only a **complete subtree inventory** proving no remaining old-epoch references
permits explicit `vault_ring_drop_before` in future grants. Completing a partial
job list is not that proof. Old blobs and removed members' cached keys remain
readable together; service allowlist removal is a separate second factor.

## Verified records and owner-sequencer fold

`vault_record_open` verifies a real signed kind-30078 head using libporthome,
checks every supplied manifest object's hash before AEAD, and closes the tree's
child-ID/epoch/address graph. The caller supplies exact per-node rings, including
independent override histories. Missing children fail with INCOMPLETE; cycles or
repeated directory objects fail with CONFLICT. File blobs need not be downloaded
for metadata folding. Records own their decoded head/root and per-path digests;
the encrypted inputs and rings may be released after opening.

`vault_fold` starts from trusted owner metadata. Only newer owner-signed records
can advance the writer policy. Readers never admit a head. Nonwriters, old
`grants_rev` heads and policy-forging heads are excluded from the materialized
view. Parent identity is the complete `(writer, seq, root_manifest)` tuple;
supply the signed history closure, including stale ancestors when referenced.
Missing closure returns INCOMPLETE rather than inventing a frontier.

One frontier head is the view. With multiple frontier heads, the latest
unambiguous admitted owner head wins, even when it is an ancestor. Missing or
forked owner state returns CONFLICT instead of a clock/LWW fallback. Conflicts
are sorted per slash-joined encrypted path and identify the other writer/head,
including deletion-vs-edit presence flags. Result indices refer to input order.
All verified input roots and parent roots are returned as a retention set,
including ignored heads. No object is deleted and no conflicting head is replaced.
On INCOMPLETE/CONFLICT the caller still owns and must free the result; view is
`SIZE_MAX`. Invalid arguments/allocation failure return no result.

Bounds: 256 records, 4096 manifest objects per record, depth 128, and PCS's path,
CBOR, entry, parent and member caps. Larger histories must be checkpointed by the
caller rather than silently truncated.

### Current-head checkpoints

`vault_fold_checkpoint(grant, records, count, owner_index, &result)` supports
fresh members querying addressable relays, which need not retain older heads.
The selected verified owner's author, vault ID and root node ID must match the
authenticated grant. Its current manifest supplies the writer policy. Each parent
reference `(W, s, root)` in an accepted current head dominates supplied heads of
W with `seq <= s`. Missing older ancestors are not an error. Ignored stale,
nonwriter or policy-forging heads cannot dominate anything. Concurrent writer
forks remain per-path conflicts against the owner view; ambiguous owner state
fails closed. Strict `vault_fold` retains its original closure requirement.

Before publishing a folded successor, call `vault_head_cover` with every accepted
record actually incorporated. It merges those records and their existing coverage
into owned parents, retaining the highest sequence per writer. Root forks at the
same sequence fail independent of input order; the parent cap fails explicitly
rather than truncating coverage. `vault_tree_head_new` also inherits the previous
owner head's coverage, so administrative grant/revoke successors preserve it.
No new event, trust source or clock rule is introduced.

## Grants, discovery and carrier seam

Grant bodies use strict named-field JSON matching §6.2, hex32 values, ordered
`ring` entries, `cap` read/write and advisory relay/Blossom hints. A write grant
alone does not admit a writer. Rumor construction creates an **unsigned** kind
30804 with a canonical event ID and empty tags. It is never signed or published
bare. Raw encoded bodies contain keys: wipe before freeing. Use
`vault_grant_rumor_free` to wipe an owned rumor before normal event disposal.

`vault_grant_carrier` is only a send/receive callback interface. Receive must
supply an authenticated carrier sender, which is checked against the grant owner;
a JSON author echo is not authentication. Callback errors propagate without
retry. The NIP-59 adapter is described below; a separate Marmot adapter supplies
MLS exporter/group delivery. Carrier preference discovery and relay I/O remain
caller responsibilities.

The kind-10319 service-list plaintext is
`{"vault-relay":[...],"vault-blossom":[...]}`. It is NIP-44 self-encrypted under
the user's identity; public tags are empty. Builders return unsigned events for
the ordinary identity signer. Open verifies the signature and self author before
decrypting. Use the caller's ACK-aware relay path to publish it. Both codecs
reject unknown/duplicate fields, malformed numbers/hex and trailing input;
URLs are unescaped ASCII ws(s)/http(s), at most 2048 bytes, with 32 hints per set.
JSON bodies are bounded at 65535 bytes (NIP-44's plaintext limit).

VSP classification requires NIP-11 `limitation.auth_required: true` **and**
`vault: true`. Blossom classification consumes libblossom's `private_server`
capability; the probe wrapper invokes the actual libblossom probe/cache.
Advertisements classify a service; they do not prove enforcement, existence
hiding or allowlist behavior. Missing/ambiguous data never promotes to private.

## NIP-59 fallback carrier

`<vault/nip59.h>` adds an opaque adapter without changing `vault.h`. Explicitly
construct it with an identity secret key and borrowed transport callbacks; the
adapter copies and wipes its own key. Access its `vault_grant_carrier` through
`vault_nip59_carrier_interface`, then call the existing grant send/receive APIs.
Only configured directions can be used. No background I/O, carrier selection or
implicit enablement occurs during setup. Per D11 the caller must prefer Marmot
when the recipient has a kind-443 key package on the vault relays; discovery and
that choice are not performed by this explicitly selected fallback.

Send requires that identity to be the grant owner, NIP-44-encrypts the unsigned
rumor to the recipient in a signed kind-13 seal with empty tags, then uses the
actual nips/nip59 implementation to create a one-time-key signed kind-1059 wrap.
Both outer timestamps are independently randomized into the past. Only that
outer wrap reaches the transport. Receive checks the unique recipient p tag,
outer signature/id, signed kind-13 seal and empty tags, then decrypts and strictly
parses an unsigned rumor. The authenticated sender is the **seal author**, never
the ephemeral outer author or an echoed JSON owner. Owner/rumor author/seal author
must agree. Bare wrapped rumors, signed rumors, invalid signatures, mismatched
recipients, duplicate p tags and malformed plaintext are refused with no output.

Transport `publish` success must mean relay OK acceptance, not just a websocket
send. `receive` returns an owned event, including when returning an error; the
adapter disposes it. AGAIN denotes no available event. Transport errors propagate
without automatic retries, including uncertain publishes. The caller owns normal
AUTH/OK/CLOSED/EOSE handling, subscriptions, persistence and event-id deduplication.
The adapter does not publish anything except through that explicit callback.
Each nested NIP-44 plaintext must fit 65535 bytes; the outer seal serialization
makes the fallback's maximum grant smaller than the raw grant codec limit.
This fallback has no forward secrecy against compromise of its delivery key.

The installed nip59 static archive/pc reuses its existing OBJECT implementation,
which remains available to existing in-tree consumers. This provides an installed
private dependency for libvault without source/build-tree path references.
Protocol: [NIP-59](https://github.com/nostr-protocol/nips/blob/master/59.md).

### Experimental NIP-4E recipient resolution

Resolution is **off by default**. Set `vault_nip59_carrier_set_nip4e(carrier,
&resolver)` before `vault_grant_send(vault_nip59_carrier_interface(carrier),
recipient_identity, grant)` to enable it; pass NULL to disable it. There is no
separate `vault_grant_send_nip59` entrypoint. The resolver's borrowed relay list
and query callback integrate with the caller's event-driven relay transport.
Libvault supplies an author-scoped kind-10044, limit-1 filter for each relay.
Query must deliver borrowed events via `emit` and explicitly report subscription
EOSE. It owns NIP-11 probing, AUTH/re-REQ, CLOSED/error handling and CLOSE cleanup;
it must not poll, sleep or treat elapsed time as completed history.

Libvault verifies the identity author and event id/signature, selects the latest
replaceable event across all queried relays (lowest id on timestamp ties), then
requires exactly one valid x-only encryption pubkey in its `n` tag. A malformed
latest event or forged author/signature is an authentication error, not permission
to resurrect an older key. An EOSE-complete empty result on **every** configured
relay uses the identity key, exactly as when the option is disabled. Any unreachable,
refusing, interrupted or otherwise failed lookup propagates its error; a callback
returning without EOSE gives `VAULT_ERR_INCOMPLETE`. **No grant is published and
there is no silent identity-key fallback on incomplete lookup.**

Both the seal and wrap encrypt to the resolved key using the existing NIP-44 and
NIP-59 implementations. The signed wrapper's standard `p` tag contains the actual
encryption recipient (`["p", "<selected pubkey>"]`), so no custom tag is needed.
[NIP-59](https://github.com/nostr-protocol/nips/blob/master/59.md#3-gift-wrap-event-kind)
allows routing metadata on the wrap; the seal's tags remain empty and the grant
rumor (kind 30804) is unchanged. No public identity-to-encryption-key mapping is
added to the wrapper. This follows **unmerged** [NIPs PR #1647, revision
`b22711d050df662bfba5f813a35d801daf067dc8`](https://github.com/nostr-protocol/nips/blob/b22711d050df662bfba5f813a35d801daf067dc8/4e.md).
That draft calls this the user-level encryption key, distinct from its per-client
4454/4455 key-distribution flow; libvault does not implement that flow.

`vault_nip59_carrier_set_device_key(carrier, device_sk)` copies a local encryption
key (NULL clears it). Receive tries that key first, then the identity key. The
receive transport must supply wraps addressed to either inbox p key; configuring
a key does not create subscriptions. Wrong-key/envelope failures return the same
`VAULT_ERR_AUTH` with no matched-key diagnostic; the authenticated sender remains
the seal's identity. Configured secret copies are wiped on replacement/free.

## Single-file NIP-94 / kind-15 export

`vault_file_export_nip94` sends ONE file to someone who will never hold the
vault (alignment note §2 A2). It reads the file through `vault_chunk_read`
(exact per-chunk epochs, hash before AEAD, no key trials), re-seals the whole
file as one AES-256-GCM blob — random 96-bit nonce, 128-bit tag appended, no
AAD, the construction NIP-17 kind-15 clients implement — under a fresh CSPRNG
key, uploads it through `blossom_client_put` with the ciphertext hash pinned,
and returns `vault_nip94_tags`: `x` (ciphertext SHA-256), `ox` (plaintext
SHA-256), `decryption-key`, `decryption-nonce` (lowercase hex, as Quartz emits
and Ditto accepts), `encryption-algorithm` = `aes-gcm`, `file-type`, `size`
and the descriptor URL. The caller wraps them in a kind-15 rumor or an `imeta`.
No ring key or derived chunk key is ever exported; the export key is
independent of every ring. Nothing about the vault format, grants or rings
changes, and libporthome's sealed-blob layout stays a future algorithm value.
The export is not streamed: plaintext and ciphertext are resident together, so
files above `VAULT_NIP94_MAX_EXPORT_BYTES` (64 MiB) are refused before any I/O
with `PORTHOME_ERR_TOO_LARGE`; libblossom's `max_blob_bytes` also applies.
Upload failures are classified (`PORTHOME_ERR_HASH_MISMATCH`, `VAULT_ERR_AUTH`,
`PORTHOME_ERR_TOO_LARGE`, `VAULT_ERR_IO`). Specs followed: NIP-17 kind 15 and
NIPs PR #2437 (NIP-94 file encryption).

## Focused tests

`test_vault_ring_derivation`, `test_vault_revoke_lazy`, and
`test_vault_fold_frontier` cover §13 b/c/d with real PCS objects and signed heads.
`test_vault_codecs` covers strict codecs, self-encrypted 10319 events, the carrier
contract, NIP-11 classification and a bounded local HTTP server exercising the
actual libblossom private-capability probe. That fixture is not live VSP-01 or
Marmot acceptance. `test_vault_grant_nip59` uses the actual NIP-59/NIP-44 and
signing/parsing APIs through recording transport callbacks: both read/write
multi-epoch bodies round-trip exactly, hostile envelopes fail closed, and callback
errors are not retried. It is not live relay or delivery acceptance.
`test_vault_grant_nip4e` covers default-off
and opt-in delivery, actual device-key opening and identity receive fallback,
all-relay EOSE requirements, query failures, forged announcements, replaceable
ordering/ties/duplicates, malformed latest keys, and unchanged grant bodies using
local recording callbacks only (no relay connections).
`test_vault_nip94_export` runs the export against a bounded local Blossom stub
(BUD-02 PUT, BUD-01 GET) through the real libblossom client and signer: the
blob is fetched by `x` and decrypted with only the returned tags; the key
differs from every ring key and every HKDF-derived chunk key; empty and
multi-chunk/multi-epoch files round-trip; missing epochs, oversize files,
descriptor hash mismatches, 500/401 uploads and signer-less clients fail
closed with classified errors. It is not live Blossom acceptance. The
implementation remains experimental.
