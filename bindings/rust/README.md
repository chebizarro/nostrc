# Installed C-library Rust bindings

Experimental, unsafe `porthome-sys` and `vault-sys` crates for the Layer-6
Onyx/Tauri assembly. They do not build C sources or implement cryptography in
Rust. Install libporthome >= 0.4.0 and libvault >= 0.2.0 and their dependencies
first; `vault-sys` also binds the additive `<vault/nip59.h>` API. No GLib, Git or
Marmot Rust dependency is introduced.

`build.rs` uses **bindgen** (C -> Rust), not cbindgen (Rust -> C). Generating at
build time keeps declarations and layouts tied to the actual installed headers
and target ABI. CI installs libclang explicitly rather than maintaining a
checked-in, platform-specific generated snapshot. `pkg-config` supplies include
paths and linker metadata; headers are never read from another package's `src/`.
`vault-sys` reuses `porthome-sys` types so a vault ring view can be passed directly
to porthome decryption. C ownership and error contracts still apply. Generated
bindings track included-header changes through bindgen's Cargo callbacks.

## Build against an existing prefix

Requires Rust/Cargo, pkg-config and libclang (set `LIBCLANG_PATH` if it is not in
a standard location). Install the C libraries with CMake or the platform's
package manager. For shared libraries on Linux:

```sh
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cargo build --locked --manifest-path bindings/rust/Cargo.toml --workspace
cargo test --locked --manifest-path bindings/rust/Cargo.toml --workspace
```

Use the prefix's actual lib directory (possibly `lib64` or a multiarch directory).
On macOS configure an install rpath or use `DYLD_LIBRARY_PATH`. For a static C
installation, the pkg-config crate honors `PORTHOME_STATIC=1` / `VAULT_STATIC=1`
and obtains the private link closure from the installed `.pc` files. The checked
consumer route below exercises shared libraries, not static-link qualification.

## Small seal/unseal consumer

This is an example, **not a production key-management tool**. It accepts a file
of exactly 32 raw node-key bytes, uses a single epoch (1), seals a PCS v2 chunk,
and unseals it using a libvault ring. It performs no network I/O. Protect key and
plaintext files yourself; unseal writes plaintext. It is not a grant/revocation
or full-vault UI. The C primitives remain experimental and unreviewed.

```sh
cargo run --manifest-path bindings/rust/examples/vault-cli/Cargo.toml -- \
  seal node-key.bin plaintext.bin sealed.bin
# stdout: <SHA256 of complete sealed object> <original plaintext byte count>
cargo run --manifest-path bindings/rust/examples/vault-cli/Cargo.toml -- \
  unseal node-key.bin sealed.bin opened.bin SIZE_PLAIN SHA256
```

Unseal verifies the supplied sealed-object address before decryption. In a real
vault both address and original size must come from authenticated metadata.

## Reproduce the CI consumer check locally

```sh
bash bindings/rust/test-installed.sh
```

Requires CMake, Ninja, a C compiler, Rust, libclang, and the C dependency development
packages (OpenSSL, curl >= 7.85, jansson, secp256k1, libwebsockets and nsync).
The script builds only the C closure required by the consumer, installs it to a
throwaway prefix, runs the original C `test_seal_v2.c` against that install, then
runs `cargo build` and `cargo test --workspace`. All scratch build/prefix/Cargo
outputs are removed on exit, including failures. The CLI integration test checks
all four C chunk envelopes and SHA256 addresses byte-for-byte (empty, text,
trailing-zero and binary), recovers each plaintext, refuses a wrong address, and
links the NIP-59 carrier API without contacting a relay.

The main `ci-selective.yml` dispatcher selects `rust-bindings-ci.yml` for changes
in this consumer or its C dependencies. Rust-only edits do not select unrelated
C package test suites. Hosted CI execution is separate from a local script pass.
