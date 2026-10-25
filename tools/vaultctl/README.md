# vaultctl (experimental 0.1.0)

Layer-7 assembly over libvault, libporthome, libblossom and libnostr's pool.
Build with `BUILD_LIBVAULT=ON`, `BUILD_VAULTCTL=ON`, `ENABLE_NIP42=ON` and
`ENABLE_NIP11=ON` (plus libvault's NIP-44/59 prerequisites). Installs `vaultctl`
to the normal binary directory. No GLib, Marmot, daemon or keychain backend.

## Commands

Use an explicitly selected, owner-only identity key file: raw 32 bytes or 64 hex
characters with an optional newline. Do not put a secret key on the command line.
The state directory must be owned by the current user, mode 0700, and outside the
imported subtree. State files are mode 0600 and contain **keys and local basenames**;
back them up as secrets. One state directory holds one vault and is exclusively
locked while a command runs. There is no implicit home-directory discovery.

```sh
vaultctl share --state /private/vault --key-file /private/identity \
  --relay wss://relay.example --blossom https://blobs.example /path/to/subtree
vaultctl grant --state /private/vault --key-file /private/identity --recipient HEX
vaultctl grant --state /private/vault --key-file /private/identity --recipient HEX --write
vaultctl revoke --state /private/vault --key-file /private/identity --member HEX --budget 16777216
vaultctl revoke --state /private/vault --key-file /private/identity --resume --budget 16777216
vaultctl fold --state /private/vault --key-file /private/identity
vaultctl fsck --state /private/vault --key-file /private/identity
```

- **share** imports a standalone vault rooted at a POSIX directory. Files use the
  PCS content-defined chunker; directory keys follow the cryptree. Safe relative
  symlinks are preserved; devices, sockets, FIFOs, unsafe symlinks and changing
  input files are refused. Hard links are independent files. Nothing is uploaded
  before the complete import has been sealed locally. The owner is an explicit
  initial writer. This does not splice a vault into an existing homed namespace.
- **grant** updates owner-signed membership before delivering an actual NIP-59
  kind-1059 wrap. The unsigned kind-30804 rumor is never published. Read membership
  is informational and never removes existing writer authority. Default grants
  contain only the latest root epoch; `--history` explicitly includes held root
  history. Historical independently overridden descendant keys are not a new
  wire extension and are **not** exported by that option. Latest-only recipients
  may not read old chunks pending migration.
- **revoke** rotates the root and every descendant directory, rehashes locally
  known names, removes membership, increments grants_rev, migrates chunks within
  the byte budget, seals bottom-up, publishes the new-key-only owner head, and
  sends latest-only continuation grants to remaining members. Lazy is default;
  exit 4 means old chunks remain. `--resume` continues without rotating again.
  `--eager` reserves the full remaining chunk budget before chunk I/O; insufficient
  budget leaves the proposal pending without publishing a partial eager result.
  Budgets cover chunk download+upload payloads, not metadata, grants or capability
  traffic. Old keys, signed history and addressed objects are retained, not
  deleted. Per-node held rings are archived independently of current reachability,
  so removing a directory does not discard the keys for retained snapshots. Service allowlist removal is reported as an **operator action**: the
  spec supplies no generic service administration API.
- **fold** admits signed, hash-bound manifest closures and prints the SDK's
  authoritative view, frontier, per-path conflicts and retained-root count as
  JSON. Paths are encrypted name tags, not guessed plaintext names. Relay queries
  request this vault's kind/d and owner/writer author set. Signed parent history
  is retained locally because [NIP-01 addressable-event storage](https://github.com/nostr-protocol/nips/blob/master/01.md)
  is not a durable DAG archive. Missing ancestry fails incomplete rather than inventing a frontier. Administrative
  grant/revoke predecessors must also have a complete, unambiguous signed ancestor
  closure; unrelated writer forks cannot veto an owner revoke.
- **fsck** fetches and verifies all supported reachable objects of every retained
  signed snapshot, including conflict/history roots, using exact held epochs.
  A command's local cache does not hide missing/corrupt remote objects. Chunk
  lengths are persisted explicitly during import/migration, validated by canonical
  re-sealing, and never inferred from padding or a file total. Missing lengths or
  keys are incomplete. This is a check, not a repair or remote materializer.

Recursive mutation across nested share boundaries is refused. Reachable
`ref_state` is incomplete until that codec is available. Limits are inherited
from the SDK (256 held heads, 4096 manifests, bounded CBOR and path sizes);
exceeding them is visible, never silent truncation. Endpoint configuration is
retained; supplied overrides must match exactly so acknowledgement bits cannot
be rebound to other services. Service privacy flags are advertisements, not proof
of an allowlist or privacy behavior. Untrusted grant hints are not enabled here.

## Completion and recovery

Each configured Blossom upload must return a hash/size-verified descriptor, and
all configured relays must return matching `OK true`. The pool's EOSE boundary
and drain are required for reads; `CLOSED`, rejected AUTH/OK and missing EOSE are
failures. NIP-42 responses use the public NIP-42 frame builder. Deadlines
(`--deadline-ms`, default 30000) fail operations; they never establish success.

Private atomic state records exact outgoing signed events and per-endpoint
intent/ACKs before external writes. A persisted publication-dirty flag survives
migration checkpoints and failed manifest uploads until the new head is acknowledged. On explicit rejection, `--resume` may retry
that **same** signed event after the operator fixes the service. An uncertain
write does not retry automatically: use `--resume --reconcile` to observe the
exact event by ID through EVENT/EOSE, or exact addressed blob bytes through GET.
Not observing an uncertain write is not proof it failed. Recipient-only relays
may make a gift wrap unobservable to its sender; that remains uncertain. Do not
delete the journal or mint another wrap to disguise an unresolved send.

Exit codes: 0 completed; 1 invalid/authentication/corrupt/I/O failure;
2 incomplete inventory/keys/history; 3 ambiguous view/conflict;
4 pending migration/budget; 5 pending/uncertain operation requiring explicit
resume or reconciliation. Read commands refuse an unpublished proposal.
Successful fold output may contain conflicts, with the owner fallback selected
by the SDK. Published policy is not rolled back if later grant delivery fails.

## Verification

`test_vaultctl_fixtures` starts temporary loopback HTTP/WebSocket fixtures and
executes the installed-shape CLI with the real pool and Blossom client. Fixture
services verify BIP340 signatures and AUTH tags, serve bounded REQ/EOSE results,
and test ACK rejection, missing EOSE/CLOSED, corruption, missing inventory,
concurrent signed heads, lazy/eager revocation and explicit reconciliation.
Checks also cover interrupted post-migration manifest publication, missing owner
ancestry, removed-directory key history and unexpected AUTH/CLOSED recovery.
`--allow-local` only permits explicitly chosen loopback HTTP/WS endpoints.
Temporary services/state are torn down even on failure. This is local fixture
execution, not acceptance against a deployed relay, lab service or release.
`test_vault_tree_ops` covers the reusable recursive/epoch/checking API with
ASan/UBSan in sanitizer-enabled builds.
