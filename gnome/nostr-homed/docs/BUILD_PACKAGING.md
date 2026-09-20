# Nostr login build and packaging contract

**Component version:** 0.2.0  
**Release state:** unreleased; no tag or installed-system support claim

## Feature parity and defaults

All features are disabled by default. Top-level CMake first requires
`-DENABLE_NOSTR_HOMED=ON`; Meson is configured directly in this directory.

| Slice | CMake | Meson | Installs runtime auth? |
|---|---|---|---|
| B auth protocol/challenge/vault/local-provider core | `NOSTR_HOMED_ENABLE_AUTH_CORE` | `auth_core` | No; static build/test target only |
| A identity authority/reader core | `NOSTR_HOMED_ENABLE_IDENTITY_CORE` | `identity_core` | No; static build/test target only |
| D domain samples/validator | `NOSTR_HOMED_ENABLE_DOMAIN_CONFIG` | `domain_config` | No; inert files under the data directory |
| Full broker/PAM product | `NOSTR_HOMED_ENABLE_AUTH_INSTALL` | `auth_install` | Configure fails: required sources/policy are incomplete |
| Legacy roaming experiment | `NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING` | `experimental_roaming` | CMake only; Meson fails explicitly until dependency discovery is repaired |

Portable auth and identity targets require neither PAM, FUSE, GDM, curl,
Blossom, a session bus, nor a user signer. `auth_core` requires libnostr;
repository CMake must use `-DLIBNOSTR_WITH_NOSTRDB=OFF` in environments without
the vendored nostrdb headers. Standalone CMake and Meson consume installed `nostr` and `libgo` pkg-config
dependency and never searches a hard-coded CMake build directory.

The full authentication install switch deliberately fails instead of silently
omitting PAM. When the broker/PAM sources arrive, that switch must require PAM
at configure time and own the broker/module/unit/AppArmor install inventory.
Until then, no new auth header, static library, daemon, PAM module, NSS module,
socket, system unit, or AppArmor policy is installed.

## Portable test inventory

Both build systems use these exact names for enabled slices:

- `homed_auth_protocol`
- `homed_auth_transaction`
- `homed_auth_transaction_identity` (when both cores are enabled)
- `homed_auth_vault`
- `homed_auth_challenge`
- `homed_provider_local`
- `homed_identity_store`
- `homed_identity_config`
- `homed_identity_fixtures`
- `homed_domain_profile`
- `homed_acceptance_runner_contract`

None is allowed to skip. Lab acceptance remains separate and unavailable when a
required role, exact pin, adapter, or evidence artifact is missing.

## Package split and installed ownership

- `nostr-homed-auth-core-devel`: no installable payload yet.
- `nostr-homed-identity`: no installable payload yet; A owns future NSS ABI and
  authority installation.
- `nostr-homed-domain-config`: inert samples, validator, policy/rollback docs;
  does not write `/etc`, join a domain, change PAM/NSS, or restart a service.
- `nostr-homed-auth`: blocked; future B-owned broker/PAM/UI payload.
- `nostr-homed-nip46-provider`: blocked; future C-owned provider payload.
- `nostr-homed-smb`: blocked pending explicit password-contract approval and D3.
- `nostr-homed-roaming`: experimental and separate from login acceptance.

Configuration and policy files are root-owned when a future package stages them.
Runtime authorities remain the sole writers of private state. The domain sample
manifest is not activation-ready; future activation must validate exact
preexisting PAM hashes and take a durable rollback snapshot first.

## Ownership and integration

- A: `src/identity`, identity/NSS configs/docs/tests and admin identity APIs.
- B: shared `src/auth` except `provider_nip46.c`, broker/PAM/UI/system policy.
- C: `nips/nip46`, `provider_nip46.c`, focused external-provider tests.
- D: root/homed build and test registration, version/pkg-config metadata,
  acceptance matrix, domain/Samba configs, packaging, activation and rollback.

Checkout: `feat/nostr-linux-samba-login-20260920` in the bound RepoPrompt
worktree. Tracker state remains coordinator-owned in the main checkout.

## Version decisions

`nostr-homed` moves from the historical pkg-config-only 0.1.0 claim to declared
0.2.0 because feature/configuration/package contracts are public and
incompatible. CMake, Meson, pkg-config, and `VERSION_MANIFEST.md` are the
coordinated authoritative sources. Latest release and tag remain unreleased.
NIP-46 remains explicitly unversioned until its owner establishes an
authoritative source; this integration does not invent one. libgo receives a packaging PATCH bump to 0.1.1: its existing public umbrella
header and transitive includes were missing from installation. Internal fiber
hooks and version templates are not exported. libnostr and the signer have no
source or public-surface changes here, so both receive no bump.

## Portable checkpoint verification (2026-09-20)

The root CMake build registers the same homed portable cases as standalone
CMake and Meson. Both standalone paths were built against actual staged
libnostr/libgo libraries and headers, not source-tree include overrides; all 11
homed cases passed on macOS ARM64. This proves portable build wiring only, not
Linux NSS/PAM ABI, GDM, AD, SMB, or installed-system acceptance.

Example commands (supply the actual dependency prefix):

```sh
PKG_CONFIG_PATH="$deps/lib/pkgconfig" cmake -S gnome/nostr-homed -B "$build" \
  -DNOSTR_HOMED_ENABLE_AUTH_CORE=ON \
  -DNOSTR_HOMED_ENABLE_IDENTITY_CORE=ON \
  -DNOSTR_HOMED_ENABLE_DOMAIN_CONFIG=ON
cmake --build "$build"
ctest --test-dir "$build" --output-on-failure
PKG_CONFIG_PATH="$deps/lib/pkgconfig" meson setup "$meson_build" gnome/nostr-homed \
  -Dauth_core=enabled -Didentity_core=enabled -Ddomain_config=enabled
meson test -C "$meson_build" --print-errorlogs
```

CMake and Meson staged inventories match: six inert domain sample/documents,
one validator executable under libexec, and version-only pkg-config metadata.
Default-disabled CMake installs only that metadata. Both build systems refuse
an explicit full-auth installation request at configure time. No system PAM,
NSS, unit, socket, or AppArmor activation is performed.
