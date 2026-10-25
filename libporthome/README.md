# libporthome (experimental)

GNOME-free PCS v1 crypto, canonical CBOR, manifest v1/v2 and sealed name
codecs, wrap-seed derivation, and a storage-independent OID/sealed-object index.
PCS v2 sealing, key rings and chunking are later SV work, not implemented here.

In-tree consumers link `nostrc::porthome` (`BUILD_LIBPORTHOME=ON`,
`ENABLE_NIP44=ON`, `BUILD_LIBBLOSSOM=ON`). Installed consumers use
`find_package(nostrc REQUIRED COMPONENTS porthome)` or `pkg-config --static
--cflags --libs porthome`. Headers live under `<porthome/*.h>`. The library
uses only libnostr, NIP-44's public `<nostr/nip44/kdf.h>`, libblossom and
OpenSSL. Build libnostr with `NOSTR_WITH_GLIB=OFF` for a GLib-free closure.

After installing those dependencies, this directory can also be configured
standalone with `cmake -S libporthome -B build-porthome`, using the installed
prefix in `CMAKE_PREFIX_PATH` and `PKG_CONFIG_PATH`.

`porthome/compat-nh.h` provides one-release source aliases for the old
`nh_porthome_*`, `NH_PORTHOME_*` and private `nhpc_*` spellings. It does not
provide binary ABI aliases. Homed's old crypto/manifest/wrapkey include paths
forward to it. Filesystem materialization/rename, the signing/Blossom adapter,
provision CLI, sandbox, status/notifications, syncd and FUSE remain in homed.

`porthome_index_entry` is the plain `{git_oid, sealed_sha256, chunk_key_id}`
row shared with the future sealed ref-state codec. The callback interface
borrows backend state and propagates errors/visitor cancellation. It owns no
storage, threads or locks. Hanami implements it over its existing SQLite
store and keeps Git-specific metadata/hash helpers there; libporthome does
not link SQLite or libgit2. This interface does not encrypt objects: existing
hanami raw-mode hashes remain raw addresses with key id zero. Sealed ODB and
ref-state serialization are R7 work.
