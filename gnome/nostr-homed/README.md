# nostr-homed (GNOME / Linux)

Nostr-backed roaming home directory and login stack, integrating NSS, PAM, and a FUSE3 filesystem. A per-user control service fetches manifests from Nostr relays, decrypts secrets via the existing signer D-Bus API, and mounts a POSIX home tree backed by Blossom/GRASP content-addressed storage.

Components:
- libnss_nostr — NSS module resolving users/groups from a local SQLite cache hydrated from Nostr events.
- pam_nostr — PAM module performing NIP-46-style auth via local D-Bus signer and bootstrapping the session mount.
- nostrfs — FUSE3 filesystem materializing home from homedir.manifest; write-back uploads to Blossom and republishes.
- nostr-homectl — CLI + D-Bus service managing cache, fetching events, decrypting secrets via signer, and launching mounts.
- systemd units — sandboxed services for the control daemon and per-user FUSE mount.

Defaults:
- RELAYS_DEFAULT: ["wss://nos.lol","wss://nostr.wine"]
- BLOSSOM_BASE_URL: "https://blossom.example.org"
- HOMED_NAMESPACE: "personal"
- SECRETS_NAMESPACE: "personal"
- UID_POLICY: "deterministic-hash"
- FUSE_IMPL: "fuse3"
- DBUS_SIGNER_BUS_NAME: "org.nostr.Signer"

D-Bus signer mapping
- This repository already includes D-Bus assets under `apps/grotto/` and `nips/nip55l/dbus/` using both `org.nostr.Signer` and `com.nostr.Signer` names.
- nostr-homed prefers `org.nostr.Signer` by default, but will also probe `com.nostr.Signer` for compatibility. See `src/common/nip46_client_dbus.c` for name probing.

Build
- CMake (top-level):
  ```sh
  cmake -S . -B build -DENABLE_NOSTR_HOMED=ON -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  ctest --test-dir build --output-on-failure
  ```
- Meson (subdir):
  ```sh
  meson setup build-gnome gnome/nostr-homed
  meson compile -C build-gnome
  meson test -C build-gnome
  ```

Install
- NSS and PAM modules install into distro libdirs (`libnss_nostr.so.2`, `pam_nostr.so`).
- Systemd units install into `lib/systemd/system`.

Configuration (`/etc/nss_nostr.conf`)

A sample file is shipped at `config/nss_nostr.conf.sample` and installed to `${sysconfdir}/nss_nostr.conf.sample`. Copy and edit it:

```sh
sudo cp /etc/nss_nostr.conf.sample /etc/nss_nostr.conf
sudo $EDITOR /etc/nss_nostr.conf
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `db_path` | string | `/var/lib/nostr-homed/cache.db` | Path to the SQLite cache database. Parent dir must exist and be writable. |
| `uid_base` | uint32 | `100000` | First UID in the allocation range. Must be > 1000 to avoid system accounts. |
| `uid_range` | uint32 | `100000` | Size of the UID allocation window. Must be >= 1000 for production. |

UIDs are assigned deterministically: `UID = uid_base + SHA256(npub_hex) % uid_range`.

Validate the active config:

```sh
nostr-homectl check-config              # validates /etc/nss_nostr.conf
nostr-homectl check-config /tmp/test.conf  # validates a custom path
```

Multi-user namespace selection

By default all users share the `personal` namespace. To give different users
different namespaces (e.g. alice uses `personal`, bob uses `work`):

**Option A: Per-user systemd EnvironmentFile** (recommended)

Create `/etc/nostr-homed/users/<username>.conf`:

```sh
sudo mkdir -p /etc/nostr-homed/users
echo 'HOMED_NAMESPACE=work' | sudo tee /etc/nostr-homed/users/bob.conf
```

The `nostrfs@bob.service` unit reads this file (via `EnvironmentFile=-`) and
overrides the default `HOMED_NAMESPACE=personal`.

**Option B: Database setting**

```sh
sqlite3 /var/lib/nostr-homed/cache.db \
  "INSERT OR REPLACE INTO settings(key,value) VALUES('namespace.bob','work')"
```

`nostr-homectl open-session bob` reads `settings.namespace.bob` and passes the
namespace to warm-cache automatically.

Both methods are optional — existing single-namespace deployments continue to
work unchanged with the unit-level default.

Quickstart
- See `docs/QUICKSTART.md` for publishing sample events, running fake relay and blob servers, and mounting a demo home.

NSS/PAM enablement (overview)

Add `nss_nostr` to `nsswitch.conf` after files (cache-only; no network on hot path):

```
passwd:         files systemd nostr
group:          files systemd nostr
```

On Debian/Ubuntu, enable PAM session for `pam_nostr` (example using common-session):

```
# /etc/pam.d/common-session (append near the end)
session optional pam_nostr.so
```

OpenSSH example (append to `/etc/pam.d/sshd` after `@include common-session`):

```
session optional pam_nostr.so
```

Notes:
- `nostr-homectl warm-cache` provisions deterministic UID/GID mapping and persists `settings.manifest.<namespace>` and `settings.relays.<namespace>`.
- `OpenSession` is called by `pam_nostr` to start `nostrfs@user` via systemd and mount `/home/$USER`.

Groups

- Primary group is created during `WarmCache` provisioning with name equal to the username and `gid == uid`.
- NSS group lookups are resolved from the local cache only. Supplemental groups are not managed in v1.

Security
- See `docs/SECURITY.md` for threat model, secret handling (tmpfs), and systemd hardening.

DBus service (Homed1)

- `nostr-homectl --daemon` owns `org.nostr.Homed1` on the session bus and exports the object `/org/nostr/Homed1` implementing the `org.nostr.Homed1` interface.
- Inspect at runtime with:

```
gdbus introspect --session \
  --dest org.nostr.Homed1 \
  --object-path /org/nostr/Homed1
```


## Portable-home operator quickstart

The portable-home stack (`nostr-home-syncd`, `nostr-home-fuse`,
`nostr-homed-provision`, `nostr-home-fetch`) is a separate deliverable from
the login stack above. It gives an existing account a Nostr-relay-backed,
Blossom-stored, resumable home tree that follows the user between machines.
Operator workflow, from a fresh account file to a first push:

```sh
# 1. Provision a new identity; publish the empty (gen 0) pointer.
nostr-homed-provision enroll \
    --relay wss://relay.example \
    --blossom https://blossom.example \
    --out-dir ~/.config/nostr-homed \
    --publish

# 2. Snapshot the home tree, upload chunks, publish gen 1.
nostr-homed-provision push \
    --account-file ~/.config/nostr-homed/<pubkey8>.account.json \
    --bump-gen

# 3. Check that the daemon is happy and the mount is up.
nostr-home-status --field syncd.state
nostr-home-status --field fuse.mounted
```

The daemon (`nostr-home-syncd`) does step 2 automatically after the initial
enrolment; the CLI is for provisioning, manual publishes, and disaster
recovery. See `man nostr-homed-provision(1)` for the full operator
reference, `man nostr-home-syncd(8)` for the daemon, and
`docs/porthome-operator-guide.md` for the end-to-end story
(broker → syncd → mount → provisioner).

### Experimental shared-vault configuration

With `NOSTR_HOMED_ENABLE_VAULT_EXPERIMENTAL=ON`, set
`NOSTR_HOMED_SYNCD_CONFIG` to a JSON file containing a `vaults` array:

```json
{"vaults":[{"path":"Portable/Team","vault_id":"<64 hexadecimal characters>","is_owner":true}]}
```

Each path is a HOME-relative directory, not an absolute path. Entries must have
unique IDs and disjoint paths; unknown fields, duplicate keys and malformed
entries refuse startup before consuming the broker credential. An explicitly
configured missing file is also an error. `nostr-home-syncd --check` validates
this configuration without network access. Builds without the vault option
refuse a configured file rather than silently falling back to v1 behavior.

Configured vault subtrees are excluded from both legacy v1 capture and full
v1 manifest reconstruction. `Portable/.conflicts` remains excluded even when
`vaults` is empty, so retained shared versions cannot leak into a personal home
manifest. Each configured subtree is an independent generation source: local
capture and current-epoch object sealing precede an acknowledged writer head;
current signed heads are checkpoint-folded and the selected view is materialised
into the ordinary snapshot. A single-frontier writer advances the tree; divergent
frontiers use the owner's view. Owner checkpoints cover every incorporated head,
including inherited coverage, not merely the previous owner head. An unseen
signed owner update that cannot be opened is incomplete, never successful
catch-up of the old policy. Historical per-node rings remain available from the
encrypted retained snapshots even when that directory is no longer in the view;
conflicting keys for one node/epoch are refused.

Bootstrap is EOSE-bounded and anchors the owner in the authenticated NIP-59 grant.
Live REQs remain open; AUTH/OK/CLOSED failures are visible, not successful sends.
Publication keeps servicing AUTH while waiting for the matching OK; an explicit
auth-required rejection retries only that same signed event after authentication.
An owner with no grant first checks its existing remote slot. It never replaces
an existing vault with a randomly generated root. A new owner with a complete v1
snapshot migrates its configured subtree with root ring `[(1, home_key)]` and an
explicit home-key override at **every existing directory**. Existing v1 chunks
are neither downloaded nor uploaded by migration; only v2 manifests and the head
are published. Subsequent reads remain compatible with those v1 chunk descriptors.
A fresh home without a v1 baseline captures a new v2 subtree instead, deriving
its independent node key from the home root key and the fresh subtree node ID.
Separate new vaults never reuse the home root key as their shared capability;
the explicit home-key reuse exception is only the approved v1 migration.

`nostr-home-syncd --vault-sync` runs the same catch-up/capture/fold/commit path once.
`--vault-retention` also services the same lazy re-seal job used by the daemon's
maintenance timer. `NOSTR_HOMED_SYNCD_RETENTION_BYTES` is a strict nonnegative
integer, default 134217728 sealed payload bytes per weekly retention class. The
encrypted reservation ledger is shared by chunk prefetch and lazy re-seal;
reservations precede I/O and survive restart. Metadata/control traffic is outside
this chunk-payload accounting. Exhaustion preserves pending work and old keys;
it does not drop epochs or claim completed migration. Endpoint/quorum-bound PUT
receipts and exact signed pending heads prevent minting replacements after an
uncertain write. Unresolved PUT intents require exact-byte readback on the
configured endpoint/identity/quorum scope before another upload can be accepted;
a missing readback does not prove rejection and never authorizes a retry. Errors
retain the last committed namespace and private journal.
Dirty-HOME preflight deferrals rescan local files; they do not poll relays.

Present foreign conflicts, including complete directory subtrees, are copied to
`Portable/.conflicts/<HOME-relative-path>@<full-writer-hex>`. Overlong local names
use `Portable/.conflicts/.overflow/<sha256(UTF8(path) || raw_writer32)>` instead.
Deletion-only conflicts have no marker file. Their signed snapshots and durable
metadata are retained, and details/mappings appear in the existing
`syncd.vault_conflicts` status field and conflict notification seam. Reusing a
physical suffix marks the older copy superseded; its signed objects and
historical-generation references remain available. Reused directory copies
remove absent tracked children while preserving untracked local files. Failed
copies resume from that exact retained snapshot, even after a newer current
writer head arrives. Copy completion and namespace publication are separate:
completed copies rebuild their namespace from the authenticated retained tree
after restart, without trusting or rewriting locally edited conflict bytes.

FUSE consumes the existing folded namespace snapshot unchanged. Open file
handles bind to immutable verified trees, their exact per-node epoch rings and
CDC lengths. No home-key/tier-0 fallback is permitted for configured vault paths.
For services advertising private Blossom or auth-required NIP-11, syncd prefetches
**every** sealed object reachable from the selected view and retained conflict
snapshots into the existing blob cache before committing the namespace, subject
to its quota, retention pins and bandwidth reservation. FUSE is cache-only there:
a miss is an I/O error plus existing FUSE status, never an authenticated GET.
**Private-vault lazy loading is unsupported in v1.** No identity secret, new
socket or signing handoff is introduced in FUSE. Public services retain the
existing unsigned lazy fetch path.

Focused offline checks are `homed_syncd_vault_runtime` and
`homed_syncd_vault_fuse` (the latter invokes the real headless open/read/release
callbacks, not a kernel mount). The optional two-Linux-UID extension of the jmx0
scenario is `tests/integration/porthome_jmx0_two_npub.py --help`; it requires actual
VSP-01 relay/Blossom services and never substitutes fixture responses for them.
Services and accounts must be disposable. The lab relay currently ends REQs at
EOSE (`nostrc-vqu69`), so that scenario uses explicit owner catch-up/fold and does
not claim post-EOSE live relay acceptance. The default runner keeps LeakSanitizer
on; its explicit diagnostic switch records the pre-existing libblossom signed-GET
leak (`nostrc-k1zq6`) and is **not** leak-check acceptance.
