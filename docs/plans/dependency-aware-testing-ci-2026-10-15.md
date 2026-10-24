# Dependency-aware testing and CI/CD

## Delivery acceptance — owner reset, 2026-10-25

This delivery has **exactly three outcomes**:

1. **Correct dependency-aware affected-test selection:** changed inputs select
   their relevant checks and downstream consumers, including compatibility where
   relevant; prerequisites are built without selecting unrelated prerequisite
   tests. Explain inclusion, omission and conservative broad fallback for unknown
   paths or incomplete relevant dependency information.
2. **Operational defaults:** explicitly invoked local tests and PR execution use
   affected selection; the push hook performs cheap static checks only, never
   configure/build/test/container work, including through legacy environment flags.
   Broad master/integration, release and manual CI routes and an explicit local
   `--broad` override remain. There is no scheduled/nightly tier.
3. **Focused representative verification:** demonstrate relevant inclusion and
   unrelated omission, including relevant compatibility, and report real failures,
   cancellations, unavailable prerequisites and unexecuted work honestly.

These are the complete delivery acceptance contract. No additional acceptance
tasks, quotas, benchmarks, exhaustive baseline repair or universal custody/profile
qualification are prerequisites. Existing safety guards on invoked routes remain
in force; a refusal or incomplete receipt is not converted into a pass.
Historical results remain in Git history and separate reviews, not as current
operational instructions.

## Goal

Run checks affected by local or pre-merge changes, build their prerequisites, and
retain downstream/compatibility coverage. Run broader suites after merge to
master, on releases and manually. Unknown dependencies broaden coverage with an
explanation. AGENTS and TESTING use this same policy.

## 1. Summary

Keep one shared dependency-aware selection policy for local and CI use. Separate
test impact from build prerequisites. Relevant expensive compatibility belongs in
affected coverage; unrelated expensive work does not. Broad execution and the
explicit full override remain available. Keep selected failures visible and retain
source-specific evidence without claiming unsupported coverage.

The obsolete architecture and qualification programme is available in Git
history, not as live instructions here. Diagnostic routes retain their own safety
contracts; proving every optional profile/custody mechanism is not this delivery.

## Delivery verification and follow-up boundary

Use focused selector, local-adapter and workflow verification for the three
outcomes. Representative relevant edits must include their downstream and
compatibility checks; a genuinely unrelated edit must omit them; an unknown or
incompletely classified relevant input must broaden with an explanation. Exercise
local/PR affected routing and master/release/manual/full-override broad routing.
Use execution or adapter/workflow tests as appropriate; planning alone establishes
selection only, not successful test execution. There is no required sample quota
or performance measurement.

Retain and identify failures rather than deleting, retrying away, skipping or
relabeling them. Selected checks that cannot run must remain failed/unavailable,
not successful coverage. Track unrelated bugs and prior qualification hardening
as nonblocking follow-ups; their existence does not mandate a repository-wide
repair campaign. A defect preventing one of the three outcomes remains relevant.
Do not weaken keychain, sanitizer, approval or owned-resource cleanup safeguards.

Existing publication identity/package safeguards remain; broad release routing is
not permission to forge complete receipts or publish through failed selected
checks. No universal attestation programme or protection migration is an
additional delivery outcome.

### Representative case reference

The golden cases below help choose focused verification; they are not an
exhaustive mandatory workload or a new sample quota.

### Golden selection cases

| Case | Expected selection | Live MDK |
|---|---|---|
| Grotto GUI-only source/template | Grotto GUI/signer checks, applicable sanitizer and build contracts; build shared prerequisites | None unless an explicitly consumed shared input changed |
| Grotto daemon/bunker | Daemon/bunker, NIP55L integration, applicable fuzz/sanitizer and package contracts | None |
| Groundhog isolated About/shell UI input | Owning UI/resource tests and actual app-process consumers | None if no harness consumes it |
| `gnome/groundhog/src/mls/gh-mls-service.c` | MLS service/UI/files and consuming acceptance checks; store tests only if they compile/consume that source | MDK 0.8 and 0.11; voice where its harness consumes service |
| `gnome/groundhog/src/ui/gh-mls-copy.c` | MLS UI/model/GUI, poll and direct-compiled harness consumers | Legacy 0.8 plus adopted checks under conservative shared-executable mapping |
| Shared Groundhog store | Store consumers including MLS | Both versions and affected voice cases |
| Shared media | Ordinary DM/media plus MLS media/files consumers | Every media-consuming compatibility case, including affected MDK011 voice |
| Relay/auth/account-relay/shared conversation implementation | DM and MLS downstream consumers | Both versions when consumed by their harnesses |
| `gnome/groundhog/src/ui/gh-poll-ui.c` | Poll test/app consumers; do not run prerequisite MLS UI suites merely because the poll target links them | Only if an actual compatibility runtime/compiled input consumes it |
| `gnome/groundhog/src/ui/gh-create-poll-dialog.c` or `gh-poll-card.c` | MLS UI consumers, including the poll test, because these sources belong to MLS UI | Adopted harness checks consuming MLS UI; legacy only for an actual input edge |
| `libgo` source/header | Libgo variants and downstream core/NIP/wrapper/app checks | Both versions through core dependencies |
| NIP44 source | NIP44 consumers, Marmot/DM/signer downstream checks | Both versions |
| Local Marmot vector | Ordinary vector checks and direct vector consumers | Not automatically live MDK |
| Unknown path | All supported profiles, explicit unknown-path reason | Both versions and voice |
| Root graph/selection metadata change | Broad configuration/validation | Both versions and voice |
| Grotto-only workflow edit | Grotto profiles plus workflow/contract validation | None |
| Rename across ownership boundaries | Union of old/new ownership | As required by either side |
| Deleted runtime fixture | Base/head ownership plus missing-resource failure | Its consumers remain required |
| Submodule pointer change | Owning vendor dependency closure; unknown vendor broadens | Both if core/crypto inputs are affected |
| Unresolvable shallow range | Broad with recorded range limitation | Both versions and voice |
| Mixed Grotto + Groundhog MLS | Union, deduplicated by check/profile | MLS-required compatibility only |

### Verification commands

Use the relevant selector/adapter/workflow tests and inspect explain output:

```sh
python3 scripts/test-selection-operational.py
python3 scripts/test-plan.py validate --inventory tests/selection/inventory.json
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --explain
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA"
scripts/test-gate.sh --broad
```

The suite command is available for regression work, not a new universal acceptance
quota. `--explain` is plan-only; `--profile` narrows execution and cannot establish
whole-profile/release coverage. Retain source/command/result provenance and report
the scope actually exercised. See [TESTING](../TESTING.md#dependency-aware-delivery-policy--2026-10-25).

## Explicit diagnostics

`namespace-policy-regression.yml` is manual-only. Full experimental
`python3 -m unittest discover -s tests/selection` remains available explicitly;
namespace/AppArmor/container, custody, attestation and source-enrollment suites
are not ordinary required CI. Their tests, safety guards and failed receipts are
preserved. See [TESTING](../TESTING.md#explicit-qualification-diagnostics).
