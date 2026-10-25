# libporthome (experimental)

GNOME-free PCS v1/v2 crypto, canonical CBOR, manifest v1/v2 and sealed name
codecs, wrap-seed derivation, and a storage-independent OID/sealed-object index.
The existing manifest schema v2 is distinct from the shared-vault manifest
extension (SV-2), which is not implemented here.

In-tree consumers link `nostrc::porthome` (`BUILD_LIBPORTHOME=ON`,
`ENABLE_NIP44=ON`, `BUILD_LIBBLOSSOM=ON`). Installed consumers use
`find_package(nostrc REQUIRED COMPONENTS porthome)` or `pkg-config --static
--cflags --libs porthome`. Headers live under `<porthome/*.h>`. The library
uses only libnostr, NIP-44's public KDF/padding headers, libblossom and
OpenSSL. Build libnostr with `NOSTR_WITH_GLIB=OFF` for a GLib-free closure.

After installing those dependencies, this directory can also be configured
standalone with `cmake -S libporthome -B build-porthome`, using the installed
prefix in `CMAKE_PREFIX_PATH` and `PKG_CONFIG_PATH`.

`porthome/compat-nh.h` provides one-release source aliases for the old
`nh_porthome_*`, `NH_PORTHOME_*` and private `nhpc_*` spellings. It does not
provide binary ABI aliases. Homed's old crypto/manifest/wrapkey include paths
forward to it. Filesystem materialization/rename, the signing/Blossom adapter,
provision CLI, sandbox, status/notifications, syncd and FUSE remain in homed.

PCS v2 primitives in `<porthome/crypto.h>` follow shared-vault-composition
§3.2/§4: root/child keys (optional override), per-node name/head keys, and
explicit chunk/manifest sealing. A borrowed `porthome_ring_t` selects exactly
one nonzero epoch; absent epochs return `PORTHOME_ERR_EPOCH`, with no key
retries. Head keys are valid nonzero secp256k1 scalars reduced modulo its order;
the negligible zero result is an error. This is not a grant or ring-mutation API.

Chunks zero-pad to the NIP-44 bucket schedule without a length prefix; nonce
derivation hashes the original plaintext, not padding. Decode uses `size_plain`
from the authenticated manifest, verifies AEAD and the padding tail, and returns
the true length. Manifest sealing is unpadded. Addresses hash the complete
sealed envelope. Callers must verify the expected address before decoding.
The v1 APIs and salts remain separate; neither decoder accepts the other version.

`porthome_chunk_next` is pure 64-byte-window buzhash, nominal 256 KiB target,
64 KiB minimum, 1 MiB maximum (only the final tail may be smaller). The fixed
SplitMix64 table seed, rolling function and cut mask in `src/chunker.c` define
its deterministic boundaries. Call repeatedly on the unconsumed suffix; no I/O,
mutable global state or allocation is involved.

`test_porthome_seal_v2` pins full envelopes/addresses from independent Python
RFC5869 + ChaCha20Poly1305 reference calculations, including a v1 known answer.
Ring/override, bucket-boundary and chunker tests are separate CTest targets.
All remain experimental and require cryptographic review before deployment.

`porthome_index_entry` is the plain `{git_oid, sealed_sha256, chunk_key_id}`
row shared with the future sealed ref-state codec. The callback interface
borrows backend state and propagates errors/visitor cancellation. It owns no
storage, threads or locks. Hanami implements it over its existing SQLite
store and keeps Git-specific metadata/hash helpers there; libporthome does
not link SQLite or libgit2. This interface does not encrypt objects: existing
hanami raw-mode hashes remain raw addresses with key id zero. Sealed ODB and
ref-state serialization are R7 work.
