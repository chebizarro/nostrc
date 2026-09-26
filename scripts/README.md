# scripts/

Operator and CI utility scripts for nostrc. This file documents
`check-authd-dep-purity.sh`; the other scripts here are self-documenting via
their own header comments.

## `check-authd-dep-purity.sh` — the authd/pam_nostr dep-purity gate

**What it protects:** `nostr-authd` and `pam_nostr.so` are the pre-login
trust boundary — `nostr-authd` runs as a system daemon and `pam_nostr.so` is
`dlopen()`-ed by `login`/`gdm`/`sshd` before any user session exists. Neither
may ever gain a build-time or link-time dependency on:

- **libhanami** (`libhanami/`) — the Blossom/libgit2 backend.
- **porthome** (`gnome/nostr-homed/src/porthome*`) — the portable-home
  crypto/manifest/sync/FUSE stack (`nostr_porthome`, `nostr_syncd_core`,
  `nostr_provision_cli`, `nostr-home-fuse`).
- **the nip55l signer *client* API** (`nips/nip55l/`) — `nostr-authd` and
  `pam_nostr` must never become *consumers* of the session signer; see
  `docs/plans/gnome-integration-and-samba-server-2026-09-25.md` §1.2 D6 for
  the boundary rationale.
- **FUSE3** (`libfuse3`) — the read-only portable-home overlay is a separate
  binary (`nostr-home-fuse`) for exactly this reason.

Every one of the above is architecturally optional and gated behind its own
CMake option (`NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL`,
`NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL`,
`NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL`, `ENABLE_NIP55L`) specifically
so a base login build never drags them in. This script is the mechanical
proof that the boundary holds, so nobody has to re-audit the whole link graph
by hand after every change.

### How it works

Given the built `nostr-authd` executable and (optionally) `pam_nostr.so`:

1. **Symbol scan** (`nm -a`, not just `-D`): every symbol name — defined or
   undefined, from the dynamic table or a statically-linked archive member —
   is checked against `FORBIDDEN_SYMBOL_REGEX`. `nm -a` is used deliberately:
   every forbidden library in this tree is a `STATIC` CMake target today, so
   an `--undefined-only` scan would miss a symbol that got statically linked
   in (the actual failure mode this gate exists to catch).
2. **Forbidden shared-object scan** (`ldd`): any `DT_NEEDED` entry matching
   `libfuse3` / `libhanami` / a porthome or nip55l shared-object name fails
   immediately, regardless of version.
3. **Positive allowlist** (`ldd`, again): every *other* shared library either
   binary links against must already appear on that binary's pinned
   `*_ALLOWED_SONAMES` array inside the script. A dependency that is new but
   not one of the four forbidden namespaces above (e.g. a stray `-lz`) still
   fails — the allowlist is closed, not just a blocklist.

### Updating the allowlist

**Adding a dependency to `nostr-authd` or `pam_nostr.so` requires editing
`check-authd-dep-purity.sh` by hand.** That is intentional — it is the
tripwire. If your change legitimately needs a new shared library:

1. Build locally (or in CI) and confirm the *reason* the new `ldd` entry
   showed up — trace it to the specific `target_link_libraries()` call.
2. Add the library's basename (with the version suffix normalized away —
   `libfoo.so.1.2.3` → `libfoo.so`) to the relevant `*_ALLOWED_SONAMES` array.
3. Explain *why* in the commit message. A reviewer reading that diff should
   be able to tell at a glance whether the new edge is one of the four
   forbidden namespaces in disguise (e.g. a transitive dependency introduced
   by linking `nostr_porthome` into `nostr_auth_runtime`) or a genuinely new,
   unrelated dependency.

Routine SONAME version bumps (a distro upgrading `libssl.so.3` to
`libssl.so.3.1`, say) do **not** require an edit — the comparison strips
everything after `.so`.

### Baseline provenance

The allowlists currently pinned in the script were captured from a clean
build of this tree on Ubuntu 24.04 (aarch64) on 2026-09-25, using the same
CMake flags `scripts/vm-linux-ci.sh` uses against the lab VM:

```
-DNOSTR_HOMED_ENABLE_AUTH_CORE=ON -DNOSTR_HOMED_ENABLE_IDENTITY_CORE=ON
-DNOSTR_HOMED_ENABLE_DOMAIN_CONFIG=ON -DNOSTR_HOMED_ENABLE_NSS=ON
-DNOSTR_HOMED_ENABLE_AUTH_RUNTIME=ON -DNOSTR_HOMED_ENABLE_PAM=ON
-DNOSTR_HOMED_ENABLE_SMB=ON
```

At that baseline, `nostr-authd` links `libnostr-json`, `libjansson`,
`libwebsockets`, `libnsync`, `libssl`/`libcrypto`, `libsecp256k1`,
`libsqlite3` (plus libc/vdso/ld-linux), plus three purely transitive C
libraries pulled in by those: `libcap` and `libz` via `libwebsockets`
(TLS capability drop / compression), and `libm` via `libsqlite3` (math
functions). `pam_nostr.so` links only `libpam`, `libjansson`, `libaudit`,
`libcap-ng` (the last two arrive transitively via `libpam` itself on
Debian/Ubuntu, not via anything nostr-authd-specific). Neither carries any
hanami/porthome/nip55l-client/FUSE edge, and no forbidden symbol namespace
appears in either artifact's symbol table.

Note: `ldd`'s transitive resolution only recurses as deep as the libraries it
can actually locate on the host running the check — a minimal container
missing a `-dev`/runtime package will report an indirect dependency as `not
found` and silently fail to recurse into *its* further dependencies. The
baseline above was captured with every runtime library actually resolvable
(the full `scripts/vm-linux-ci.sh` apt package list installed), which is why
it includes the second-order `libcap`/`libz`/`libm` edges — a naive capture
on a bare container would have missed them and produced a false "clean"
allowlist that fails on the first real CI run.

### Where it runs

Wired into CTest as `homed_authd_dep_purity` in
`gnome/nostr-homed/CMakeLists.txt`, guarded on
`NOSTR_HOMED_ENABLE_AUTH_RUNTIME AND TARGET nostr-authd` (matching the
existing `homed_syncd_off_closure` guard pattern) — **not** nested inside
`NOSTR_HOMED_ENABLE_AUTH_INSTALL`, since `scripts/vm-linux-ci.sh` (the
environment that actually exercises these two artifacts today) never turns
that option on. The test is required (no `WILL_FAIL`) and finishes in low
single-digit seconds — it's two `nm`/`ldd` invocations, not a build.

On a host without both `nm` and `ldd` (e.g. macOS), the script exits 77 and
CTest reports the test as skipped (`SKIP_RETURN_CODE 77`) rather than failed.

### Scope note

This script is the **baseline scaffold** (plan Execution-Index #21a). The
**final integrated check** (#21b) — rerunning this same script after every
subsequent track lands, plus an installed-artifact check on the lab — is
separate follow-up work and does not require any change to this script or
its CMake wiring; #21b rides this test as-is.
