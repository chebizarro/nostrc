# Nostr Linux and Samba Login Acceptance Matrix

**Contract version:** 1  
**Current result:** **UNAVAILABLE / BLOCKED**  
**Release support claim:** none

This document and `gnome/nostr-homed/tests/acceptance/matrix.json` are the D0
acceptance contract. The JSON file is authoritative for executable inventory,
required pins, lab roles, and case identifiers. It deliberately records missing
infrastructure as `unavailable`; candidate operating-system names are not exact
package or image pins.

The current macOS arm64 host and Docker Linux arm64 can run the portable harness
tests and future compile/unit checks. They cannot establish GDM pre-login,
console recovery, amd64 packaging, Samba AD trust, Windows Explorer, GNOME
Files/GVfs, or real external-signer acceptance. A container result must not be
attached to any of those case IDs as passing evidence.

## Required pinned lab roles

| Role | Candidate target | Current state | Required before ready |
|---|---|---|---|
| Desktop GDM | Ubuntu 24.04 amd64, distribution GDM/GNOME/PAM/systemd | Unavailable | Snapshot image hash and exact package builds; console break-glass exercised |
| Standalone Samba | Dedicated Ubuntu 24.04 amd64, `security = user`, `tdbsam` | Unavailable | Image, Samba build, and dedicated-config hash |
| Samba AD DC | Separate laboratory DC | Unavailable | Image, Samba build, and domain fixture ID |
| Joined desktop | Ubuntu 24.04 amd64 with winbind/pam_winbind | Unavailable | Image, package builds, and reviewed idmap contract hash |
| Windows client | Windows 11 24H2 Explorer | Unavailable | Image hash and exact build |
| GNOME client | GNOME Files/GVfs | Unavailable | Image and exact Nautilus/GVfs builds |
| Controlled relay | Deterministic EVENT/OK/EOSE/CLOSED/AUTH and disconnect injection | Unavailable | Implementation revision and fixture hash |
| Controlled signer | Real-crypto NIP-46, distinct transport and user keys | Unavailable | Implementation and NIP revision pins plus capability-profile hash |
| External signer | Amber is a candidate, not supported | Unavailable | Named product version/deployment ID, pairing, and capability-profile hash |
| Package snapshot | Immutable dependency repository | Unavailable | Snapshot/manifest hashes and compiler/sanitizer versions |
| Fault injection | Network, clock, disk, process, and database faults | Unavailable | Reviewed profile hash |

Every role must be reachable, resettable, and marked `ready` in the JSON
contract. Every listed `required_pin` must have an exact non-placeholder value.
This matrix must remain blocked if the real lab is absent.

## Executable inventory and result semantics

The authoritative case list is emitted without contacting a lab:

```sh
python3 gnome/nostr-homed/tests/acceptance/run_acceptance.py --inventory
python3 gnome/nostr-homed/tests/acceptance/run_acceptance.py --check-readiness
```

Statuses are `pass`, `fail`, `skip`, and `unavailable`:

- `pass`: the exact case ran on its pinned role and supplied valid evidence.
- `fail`: the case ran and failed, the adapter/result contract was invalid, or
  the injected secret canary appeared in evidence.
- `skip`: the adapter returned exit 77 for an optional/non-applicable case.
  A required skip makes the aggregate result `unavailable` and nonzero.
- `unavailable`: prerequisites, pins, adapter, or required result are absent;
  aggregate exit status is nonzero.

Exit 0 is possible only when every selected required case is `pass`. Exit 1 is
a test or evidence failure; exit 2 is unavailable/blocked; exit 64 is a harness
contract or invocation error. Thus missing implementations or infrastructure
cannot produce a successful acceptance report.

## Safe opt-in execution

Execution is restricted to disposable lab adapters and requires all of:

1. Ready roles and exact pins in `matrix.json`.
2. An executable adapter path in the suite variable:
   `NOSTR_ACCEPTANCE_GDM_ADAPTER`, `NOSTR_ACCEPTANCE_SMB_ADAPTER`, or
   `NOSTR_ACCEPTANCE_WINBIND_ADAPTER`.
3. The literal opt-in:
   `NOSTR_ACCEPTANCE_LAB_OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB`.
4. A new, non-existing evidence directory.

Each suite also has a pinned wall-clock timeout. On expiry the runner terminates
the adapter process group, records required cases as failed, and returns nonzero.

Example after the lab exists:

```sh
NOSTR_ACCEPTANCE_LAB_OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB \
NOSTR_ACCEPTANCE_GDM_ADAPTER=/absolute/path/to/reviewed-gdm-adapter \
gnome/nostr-homed/tests/acceptance/run_gdm_vm.sh \
  --evidence-dir /absolute/new/path/gdm-run-001
```

The runner never invokes a shell for adapters, sends no stdin, suppresses
adapter stdout/stderr, and passes only a small environment allowlist. It does
not accept passwords, private keys, tokens, nsecs, or mnemonics as arguments or
configuration. Adapters must obtain real credentials interactively or through
a reviewed descriptor/agent mechanism and must never place them in evidence.

Each adapter receives `--matrix`, `--evidence-dir`, and `--results`. It must
write `--results` as:

```json
{
  "schema_version": 1,
  "suite": "gdm",
  "results": [
    {"id": "gdm.break_glass_console", "status": "pass", "evidence": ["console.json"]}
  ]
}
```

Only relative paths to regular files inside the suite evidence directory are
accepted. Every passing case requires at least one nonempty evidence file; the
result envelope itself is not evidence. Symlinks and special files fail the result. The aggregate report
records path, byte count, and SHA-256, not evidence contents. The runner injects
a per-run synthetic secret canary and fails the suite if that canary appears in
any evidence file. This is a guard, not proof that arbitrary real secrets were
handled safely; the lab adapter and journal review remain part of acceptance.

## Evidence ownership and remaining D-phase contracts

- Release/lab review owns immutable images, package snapshots, reachability,
  reset, timestamps, and aggregate reports.
- Security review owns proof admission, signer approval, cancellation, and
  secret-canary/journal inspection.
- Samba review owns dedicated-instance isolation, SID stability, credential
  rotation, client interoperability, AD trust/idmap, and session revocation.
- The next D phase must implement reviewed adapters for actual GDM workers,
  standalone Samba plus Windows/GNOME clients, and winbind/AD. It must not
  replace them with Docker-only, synthetic PAM, client-only, or mock-signer
  evidence.
- Phase 0b must pin the Samba account/SID adapter, privileged authority boundary,
  password-delivery contract, PAM service routing, Kerberos/samlogon behavior,
  and failure/restart semantics before Samba implementation claims acceptance.

D0 remains blocked until the real laboratory and exact pins exist. These
documentation- and test-only additions do not alter a shipped component, so the
component version decision is **no bump**.

## D2 build, package, ownership, and version freeze

The portable integration contract is now recorded in the JSON fields
`implementation_checkout`, `component_owners`, `build_contract`,
`portable_checks`, and `version_plan`. These fields do not weaken any D0 lab
role or case. The full auth install still fails configuration, all new features
are off by default, and the domain package installs only inert examples under
the data directory. It never changes `/etc`, PAM, NSS, Samba, GDM, or a domain
join.

The exact portable test inventory and package split are documented in
`gnome/nostr-homed/docs/BUILD_PACKAGING.md`. `nostr-homed` is declared 0.2.0 in
CMake, Meson, pkg-config, and `VERSION_MANIFEST.md`; the release/tag fields stay
unreleased. NIP-46 remains explicitly unversioned, while libnostr and signer
receive no bump from this integration-only slice.

## D8 domain configuration checkpoint

`config/domain-route.conf.sample`, `config/smb.conf.winbind.sample`, and
`packaging/domain/validate_domain_profile.py` freeze the currently testable
route contract. Portable tests reject short-name mode, overlapping ranges,
cross-route fallback, unsafe home templates, offline login, missing pins, and
changed PAM file hashes. The activation manifest remains false and no active
PAM profile is shipped because D4 has not run. Consequently
`winbind.real_gdm_pam_dispatch`, cache/TGT behavior, joined-host home creation,
and route-isolation acceptance remain **unavailable**, not passed.
