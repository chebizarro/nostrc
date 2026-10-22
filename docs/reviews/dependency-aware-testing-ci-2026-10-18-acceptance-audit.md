# Dependency-aware testing: acceptance reconciliation

## Ordinary media ownership disagreement — 2026-10-22

**Base:** `93970ed48fb5711193bc73bd0b304b9e81e6f25f`.
**Implementation/tested commit:** `ccdbd77a2bf6210bc99c8f57f7077801b6c00f0c`.
Only the ordinary `groundhog.media` check gains the existing
`groundhog.unclassified` owner. No graph edges, source reclassification,
descriptor, membership, profile, authority/source-review renewal, workflow,
default or release-mode changes. **No component version bump:** internal
metadata/tests/docs only.

The unchanged [fallback descriptor](../../gnome/groundhog/TestInventory.cmake#L191)
assigns `groundhog-media` to `groundhog.unclassified`, with empty explicit targets;
CTest command/artifact mapping resolves `test-groundhog-media`. Its actual
[media/conversation links](../../gnome/groundhog/CMakeLists.txt#L3142) and
[net → recipient link](../../gnome/groundhog/CMakeLists.txt#L1986) compile twelve
inputs with that existing owner: ten conversation sources (including later
`gh-nip17-file.c` and `gh-attachment-transfer.c` additions), `gh-recipient.c`
and `tests/media/test_media.c`. The check omitted this owner; neither a blanket
graph edge nor a descriptor/source-authority rewrite is needed.

Fresh independent nonroot Linux/amd64 Rosetta configurations/preaudits ran at
base and implementation commits respectively, in the unchanged reviewed image
`sha256:0c8050c100ee91acab20ea1c47bdb47968ac8c5fc41669451a6fc41254a44724`,
with networking disabled and pinned Blueprint 0.22.2 available. Only independent
build-root prefixes are normalized; source roots/node labels are identical.
Full target, test, descriptor, runtime and input-hash sets agree per pair.

| Profile | Registrations / targets | Before → after, both FAIL | Exact removed / added | Identical input hashes |
|---|---:|---:|---:|---:|
| `groundhog.linux` | 422 / 552 | **8,918 → 8,905** | **13 / 0** | 1,316 |
| `grotto.linux` | 461 / 603 | **9,146 → 9,146** | **0 / 0** | 1,700 |

Removed: exactly one `check_descriptor_owner_disagreement` and twelve
`missing_impact_edge` source findings, all for `groundhog-media`, missing only
`groundhog.unclassified`. Remaining Groundhog: **365 unclaimed + 8,540
missing-impact**. Remaining Grotto: **369 unclaimed + 8,777 missing-impact**.
Both full audits still block execution. Exact33 omission records, selected/fixture
arrays and all 193 reviewed source hashes per profile are unchanged. All 29
omitted-target closures remain required but **NOT freshly built**; omissions
remain `not_executed`, `complete=false`, `complete_gate_eligible=false`.

Evidence: `/tmp/nostrc-owner-disagreement-20261022/comparison-summary.json`, SHA-256
**`6ea373e019973a62cd804275c4a78024b26a1deeb0b475281c2b58d163bcb35f`**,
binds raw full failed preaudits/exports and exact removed/added finding sets.
`media-source-witness.json`, SHA-256
**`e2ed80d5ea828a19c06137cc8e177be510a485c545e7205e9c72c9bc8ae54ec4`**,
retains actual compiled-target/source ownership and source hashes. The earlier
headless-identity receipt was inspected/hash-checked, not relabeled as fresh proof.

At `ccdbd77a2`: **513/513 selector PASS, zero skips, 240.064s**; focused shared-edge
suite **4/4 PASS**. New miniature replays authored CMake statements and unchanged
descriptor with stub bodies; remove-owner negatives restore descriptor/source and
postbuild-header gaps while preserving unrelated unclaimed/runtime failures and
full input hashes. It is metadata regression proof, **not product execution**.
Clean safe native default-all configure/build, read-only keychain guards, inventory
validation, four statics, actionlint and whitespace **PASS**. Native Groundhog stays
OFF; libsecret/secret-service safety flags remain set. **Full native CTest, actual
selected-profile execution, all-target Linux/omitted builds and production postbuild
header audits NOT RUN.** One bounded actual inline review found no blockers after
rejecting stale packaged context. Development test/harness failures remain retained.
No full-profile, hosted/W11, release, cutover or acceptance completion is claimed;
`.2` remains open and `.9` in progress. Historical receipts/ledger remain unchanged.

## Pure remote identity/account configuration family — 2026-10-22

**Base:** master `3c7c42c08cf1fade80da1355e4392847060abdb2`.
Enrollment/selected-closure renewal **`5d616c6543d6bfc8f97ab85d2b9395c56bcd29a6`**.
Internal inventory/tests only: **no shipped component version bump**. No product,
CMake, profile, capability, engine, images, gitlinks, workflow or default changes.

The next coherent pure family is the remote-identity/account configuration stack:

| Exact registration | Exact build target | Ordinary check (`groundhog.` prefix) | File API target closure |
|---|---|---|---:|
| `groundhog-account-backends` | `test-groundhog-account-backends` | `identity_account_backends` | 24 |
| `groundhog-account-remote-switch` | `test-groundhog-account-remote-switch` | `identity_account_remote_switch` | 24 |
| `groundhog-nip46-credentials-list` | `test-groundhog-nip46-credentials-list` | `identity_credentials_list` | 22 |
| `groundhog-nip46-session` | `test-groundhog-nip46-session` | `identity_nip46_session` | 19 |
| `groundhog-signer-nip46` | `test-groundhog-signer-nip46` | `identity_signer_nip46` | 21 |

Actual [session/signer targets](../../gnome/groundhog/CMakeLists.txt#L373),
[account targets](../../gnome/groundhog/CMakeLists.txt#L678) and
[credential-list target](../../gnome/groundhog/CMakeLists.txt#L1098), plus the
[recording transport](../../gnome/groundhog/tests/app/gh-test-bunker.h#L4),
[injected account discovery](../../gnome/groundhog/tests/app/test_account_backends.c#L56),
[injected session factory](../../gnome/groundhog/tests/app/test_account_remote_switch.c#L185)
and [credential backend](../../gnome/groundhog/tests/store/test_nip46_credentials_list.c#L109)
establish the scope—not name-based enrollment. Private-bus identity/account/auth,
wire/live-signer, account-relay/store, keyring/keychain and display/UI registrations
remain excluded. No fake capabilities or new nonmember omissions are admitted.

The [new export witness](../../tests/selection/fixtures/headless_identity_exports.json)
retains actual command/artifact, source/transitive-target, descriptor, runtime and
fixture relationships. None of these closures contains the application, legacy
GTK resource archive, Groundhog UI or Marmot. Both existing setup registrations
remain exactly `groundhog-test-homes` and `groundhog-test-gst-registry`. Account
checks retain `groundhog-test-schemas` and declared schema inputs; isolation,
timeouts, memory settings and original fatal-guard presence/absence are unchanged.
Existing conservative descriptor owners remain explicit, including
`groundhog.unclassified`; three command/artifact-resolved fallback descriptors
retain empty explicit target lists. Check nodes cover actual source/descriptor
owners, without adding dependencies to unclassified/execution catchalls. All four
new test/witness/helper files immediately enter `selection.contract` inputs.
UI/resource-only no-MDK behavior remains covered.

### Exact selected-coverage renewal; immutable older receipts

Only Groundhog's `primary_registrations` **16 → 21** and `required_closure`
**18 → 23** add the exact five names. The same **33 omitted libgo registrations**,
candidate hash, full universe, all **193 source-review inputs**, profile/toolchain
and workflow fields, Grotto authority and sanitizer authority are unchanged.
The [new renewal witness](../../tests/selection/fixtures/headless_identity_subset_renewal_20261022.json)
owns only this five-name delta. All eleven earlier export/renewal fixtures are
byte-identical to master, including the prior headless-model **16/18** receipt.
Historical tests first project out only this exact later delta and check the full
prior-record digest, then retain their original three-name/source-review guards.
This does not freeze old historical arrays as current authority or loosen them to
an arbitrary subset. The actual current all-ordinary resolution has exactly
**21 primaries / 23 expected executions**, including the old three-test family
and both setups.

On fresh current authored x86 exports, production `admit`, actual CLI `resolve`
and `validate_resolved` **PASS** for the five-check plan with exactly **seven
expected executions**, no required capabilities, the same 33 omissions and 29
mandatory omitted-build targets. An additional independently configured inventory
with the new checks but old selected arrays makes actual CLI resolution and
`admit` refuse **`authored primary/fixture closure changed`**. No hash/context
rebinding substitutes for that negative proof.

### Fresh full audits and physical diagnostics

Master/current inventories are independently configured for each authored profile
on committed `5d616c654`. Only independent build-root prefixes are normalized;
source roots and node labels remain identical. Full target, registration,
descriptor, runtime and input-hash sets agree within each pair.

| Profile | Registrations / targets | Before → after, both FAIL | Removed / added | Identical full input hashes |
|---|---:|---:|---:|---:|
| `groundhog.linux` | 422 / 552 | **8,923 → 8,918** | **5 / 0** | 1,316 |
| `grotto.linux` | 461 / 603 | **9,146 → 9,146** | **0 / 0** | 1,700 |

Exactly five unclaimed registrations disappear; no source/target audit failure is
removed or reclassified. Groundhog still has **365 unclaimed + 8,552 missing-impact
+ one owner disagreement**. Grotto retains **369 unclaimed + 8,777 missing-impact**.
Omitted-target audits remain full, not selected-only. Omissions stay
`not_executed`, `complete=false`, `complete_gate_eligible=false`.

Separately, a fresh authored x86 configure at base `3c7c42c08` and physical build
of all five targets **PASS**; all five binaries are ELF x86-64. Direct CTest passes
**7/7, zero skips, 7.90s**, with networking disabled and no display, Wayland or
session-bus variables, under nonroot Linux/amd64 Rosetta using the original image
`sha256:0c8050c100ee91acab20ea1c47bdb47968ac8c5fc41669451a6fc41254a44724`.
Read-only source/Git; no image build or hosted run. This is explicitly **raw
diagnostic execution**, not profile/backend execution: build parallelism is 2,
but CTest parallelism 1 differs from authored 2. Later metadata changes are not
relabeled as a new physical family build/test.

Evidence `/tmp/nostrc-headless-identity-20261022/comparison-summary.json`, SHA-256
**`0699253e7ca576aec87ab873f24587631a9e9dc4423aedaa78cf14dda1e34e9e`**,
binds full failed audits, exact deltas, exports, current/old CLI guard results and
authority proof. `focused-proof.json`, SHA-256
**`922b6f5d8faacd7ba3817493862a9dbe0dcbfa777952b732227539ccde0be4ad`**,
binds physical build/CTest logs and ELF evidence. Initial missing/in-progress
submodule setup refusals, development negative-test errors and the first negative
admission hitting inventory identity before the closure guard are retained.
The first Grotto configure correctly refused PATH-selected Blueprint 0.12.0;
separate fresh Grotto contexts select the existing pinned 0.22.2 in
`/usr/local/bin`. Groundhog contexts retain their original `/usr/bin:/bin` phase
environment and strict reload checks. No stale export overwrite, blind rehash,
profile/toolchain relaxation or failed-attempt erasure.

### Gates and remaining acceptance

- At `5d616c654`: **511/511 selector PASS, zero skips, 276.574s**, including
  **15/15** combined headless-family and **14/14** ordinary-subset regressions.
- Fresh clean safe native macOS/arm64 default-all configure/build **PASS at
  `5d616c654`**, using libsecret/secret-service safety flags, parallelism 4 and
  default-off Groundhog. **Full native CTest NOT RUN** for inventory-only work;
  no previous native test pass is claimed current.
- Four statics, actionlint and whitespace **PASS**. One bounded actual inline
  metadata/projection review reports no concrete blockers after discarding an
  app-supplied stale review package; it does not attest full-profile acceptance.
- Full selected-profile backend execution, all-target Linux build, physical
  completion of all 29 omitted-target closures and production postbuild header
  audits remain **NOT RUN**. Full preaudits still block gated execution. No
  profile-union, hosted/W11, complete-gate, release or migration-completion credit.
- `.2` remains open; `.9` remains in progress. Official `bd export --all` preserves
  all issue/memory rows. `bd dolt pull` refuses `no remote`; no sync success.
  No `.5`/ledger/counter refresh, default/release/protection cutover, push or merge.
  Parent owns verification, landing and cleanup.

## Exact ordinary headless store/conversation family — 2026-10-22

**Base:** master `d953c37fb`. Enrollment `c70951f30a39386dd967ebe60c8a445d8b3eb25d`;
parent-authorized selected-closure renewal
**`51fcfc5ffdf6ff1a272703b59f12bd3330893e6a`**. Internal inventory/tests only:
**no shipped component version bump**. No product, CMake, images, gitlinks,
profile, workflow, capability, engine-guard or default changes.

Three separate ordinary checks enroll exactly `groundhog-store`,
`groundhog-conversations` and `groundhog-store-conversations`: the G02 encrypted
store/testhooks, pure conversation model and G05 persistence delegate. Actual
[CMake definitions](../../gnome/groundhog/CMakeLists.txt#L441),
[store target](../../gnome/groundhog/CMakeLists.txt#L1166) and
[delegate target](../../gnome/groundhog/CMakeLists.txt#L1297) establish this family.
`groundhog-store-sqlite` is deliberately excluded: its application loader/link-set
contract is not a pure-model test. Identity, UI, MLS and other registrations are
not enrolled. Existing sanitizer checks remain byte-identical. Existing
conservative unclassified descriptors/path ownership remain explicit; no
`depends_on` edges are attached to unclassified or execution catchalls.

The [tracked export witness](../../tests/selection/fixtures/headless_model_exports.json)
retains real command-artifact mappings, File API sources/transitive targets,
properties and descriptors from the earlier authored x86 export, not a new
execution claim. Fresh exports independently reproduce its three family closures.
Their target closures contain 2/22/23 targets, respectively for store/model/delegate;
none includes the application, identity, legacy GTK resource archive or Marmot.
Portable GTK remains a real linked prerequisite, not a display requirement.
Both required setup registrations remain `groundhog-test-homes` and
`groundhog-test-gst-registry`; original environments, timeouts and fatal guards
are unchanged. All new regression/witness files are explicit `selection.contract`
inputs. UI/resource-only no-MDK selection remains covered; genuine shared-store
consumers and existing unknown failures are not hidden.

### Admission correction: more selected coverage, not more omissions

The first fresh current-profile preaudit at `c70951f30` correctly **refuses**:
`ordinary nonmember subset: authored primary/fixture closure changed`.
The initial 503-case selector pass and raw CTest alone were insufficient.
Following explicit parent authorization, only the Groundhog record's
`primary_registrations` (**13 → 16**) and `required_closure` (**15 → 18**) add
these exact three names. The approved **33 omitted libgo names**, 422-name
universe, candidate/toolchain/profile/workflow hashes, all 193 reviewed source
inputs and their history remain unchanged. The candidate hash covers the omitted
libgo family, not these selected tests; fresh actual x86 recomputation confirms
it without a hash refresh. Grotto and sanitizer authorities are unchanged.

The [bounded renewal witness](../../tests/selection/fixtures/headless_model_subset_renewal_20261022.json)
preserves the prior arrays/record digest and physical proof. Original historical
witnesses are unmodified. Their regression projects only the two exact added-name
arrays before checking every earlier source-review hash; unknown changes still
refuse. On fresh `51fcfc5ff` exports, production `resolve_checks`, ordinary-subset
`admit` and `validate_resolved` **PASS** with exactly three primaries plus two
fixtures, no required capabilities, the same 33 omissions and 29 mandatory
omitted-build targets. All five `expected_executions` fit the renewed closure;
substituting the old contract still refuses. This is actual admission/resolution,
**not** a passing full input audit, backend execution receipt or release eligibility.

### Fresh full audits and focused physical execution

Before/master and after/renewed inventories each have independently configured,
source/inventory/phase-bound contexts on committed `51fcfc5ff`, for both authored
Linux/x86_64 profiles. Only independent build-root prefixes are normalized;
source roots and node labels are unchanged. Full target, registration, descriptor,
runtime and input-hash sets agree within each pair.

| Profile | Registrations / targets | Before → after, both FAIL | Removed / added | Identical full input hashes |
|---|---:|---:|---:|---:|
| `groundhog.linux` | 422 / 552 | **9,280 → 8,923** | **357 / 0** | 1,316 |
| `grotto.linux` | 461 / 603 | **9,146 → 9,146** | **0 / 0** | 1,700 |

Groundhog removes three unclaimed registrations and 354 emitted missing-impact
findings for those same three tests—not 357 distinct root-cause repairs. It still
has **370 unclaimed + 8,552 missing-impact + one owner disagreement**; Grotto retains
**369 unclaimed + 8,777 missing-impact**. All omitted-target source/runtime audits
remain full, not selected-only. Omissions remain `not_executed`, `complete=false`,
`complete_gate_eligible=false`.

Separately, at **`c70951f30`**, a fresh authored x86 configure and physical target
build pass; all three binaries are ELF x86-64. Direct CTest passes **5/5, zero
skips, 23.10s** (three primaries and both setups), with no display, Wayland or
session-bus variables. This is nonroot Linux/amd64 under Rosetta, original reviewed
image `sha256:0c8050c100ee91acab20ea1c47bdb47968ac8c5fc41669451a6fc41254a44724`,
read-only source/Git and network disabled, not a new image build or hosted run.
No product bytes changed in the later metadata renewal; that execution is not
relabeled as a new `51fcfc5ff` build/test.

Local proof `/tmp/nostrc-headless-family-20261022/renewed-authored-pairs/summary.json`,
SHA-256 **`8cca9d82e933d3073df9058407ede5bd2eeac261d8e358e6f4e747148438b6d7`**,
binds raw failed full audits, exports, exact removed sets, admission/resolution
and old-contract refusal. `focused-proof.json` and raw build/CTest logs retain the
physical execution. The initial stale-contract refusal, development negative-test
mistakes, bind/import and setup failures, and a comparison-driver assertion that
incorrectly expected the intentionally enlarged arrays to be equal are retained.
The independently configured Groundhog contexts were strictly reloaded under their
original phase environment (Docker hostname restored from retained events), not
rebound by changing hashes; Grotto contexts were separately configured afterward.
No failed attempt is counted as coverage or silently erased.

### Gates and limits

- Final code `51fcfc5ff`: **504/504 selector PASS, zero skips, 236.227s**;
  focused family **8/8** and ordinary-subset **14/14 PASS**. Earlier `c70951f30`
  **503/503 PASS, 241.150s** remains a separate pre-renewal result.
- Fresh clean native macOS/arm64 default-all configure/build **PASS at c70951f30**,
  with libsecret/secret-service safety flags and parallelism 4; Groundhog stays
  default-off. This is not the Linux family execution. **Full native CTest NOT RUN**
  for metadata-only work; no earlier native test pass is borrowed.
- Four final statics, actionlint and whitespace PASS. The bounded inline diff
  review and parent-requested admission-delta review find no concrete blockers;
  stale packaged review context was rejected in favor of actual current inline code.
- Production backend selected-profile execution, full all-target Linux build,
  physical completion of all 29 omitted-target closures and production postbuild
  header audits remain **NOT RUN**. The still-failing full preaudits block gated
  execution. No hosted/W11, complete-gate, release or migration-completion credit.
- `.9` remains in progress; `.2` remains open. Official `bd export --all` preserves
  all issue/memory rows. `bd dolt pull` refused `no remote`; no sync success.
  No `.5`/ledger refresh, defaults, release/protection cutover, push or merge.
  Parent owns independent verification, landing and cleanup.

## Bounded shared-target edges and resource-history projection — 2026-10-22

**Base:** master `37e144cab898e942d2875d903670ccaf4335158b`.
Graph/header repair `693d76795260aa148edf47937be2459701e4f0d5`;
resource-history projection/indentation `784abcc291583fb2ca721d34439a48abe50a2ce0`;
final live-authority decoupling **`dba601f225fa6ebce0fb81f586105df46d541c9c`**.
Internal inventory/tests only: **no shipped component version bump**. No product,
CMake, image, gitlink, profile, exclusion, membership or admission-authority edits.

### Source causes, not emitted-count enrollment

The remaining bulk is rooted in explicitly unclassified Groundhog registrations.
It is not evidence for attaching broad prerequisites to `groundhog.unclassified`
or execution nodes. Those nodes retain empty `depends_on`; unclaimed tests remain
unclaimed. This bounded group repairs three actual shared-target chains:

- **Portable issue-model archive:** `nostr.gtk.portable → nip34` follows both the
  embedded `gn_issue_model` object and the direct
  [`nostrc_link_archives(nostr_gtk_portable PRIVATE nip19 nip34)`](../../nostr-gtk/CMakeLists.txt#L257).
- **Identity shared dependencies:** `groundhog.identity_auth → gnome.seahorse`
  follows [`groundhog-identity → gnostr-secret`](../../gnome/groundhog/CMakeLists.txt#L266);
  `groundhog.identity_auth → groundhog.relay` follows its
  [`groundhog-nip46-session`](../../gnome/groundhog/CMakeLists.txt#L362) dependency.
- **Group UI consumers:** `groundhog.group_ui → groundhog.account_relays` and
  `→ groundhog.net` follow the real group-UI → conversation-UI → DM-inbox/account-relay
  and [conversation-UI web-content/network](../../gnome/groundhog/CMakeLists.txt#L3520)
  chains. Exact `gh-account-relays.h` ownership now matches its implementation;
  no directory-wide ownership expansion substitutes for the missing header edge.

[`SharedDependencyEdges`](../../tests/selection/test_shared_dependency_edges.py)
replays the actual literal link statements/root archive wrapper in an isolated
CMake project with explicitly fixture-only bodies. File API closure, independent
five-edge removals, source/runtime/omitted inputs and discovered-header audits are
exercised. Existing secret-store broad recovery remains broad, not waived.
PR #6 legacy UI/resource-only changes still avoid MDK; true Portable inputs retain
real consumers. New regression/support/JSON sources are explicitly declared in
`selection.contract` before the full suite.

### Fresh independently bound full preaudits

At committed **`693d76795`**, each baseline/repaired inventory was independently
configured with its own binding on the same source. All four exports reloaded
under the original phase environment. Only independent **build-root prefixes**
are normalized; source roots and node labels are identical. Targets, registrations,
descriptors and full input hashes agree within each pair.

| Authored Linux/x86_64 profile | Registrations / targets | Before → after, both FAIL | Removed / added | Identical full input hashes |
|---|---:|---:|---:|---:|
| `groundhog.linux` | 422 / 552 | **9,325 → 9,280** | **45 / 0** | 1,316 |
| `grotto.linux` | 461 / 603 | **9,179 → 9,146** | **33 / 0** | 1,700 |

All removed findings are `missing_impact_edge`; the distinct dependency causes
are the five edges above, not 78 independent ownership repairs. Groundhog retains
373 unclaimed registrations and one owner disagreement; Grotto retains 369
unclaimed registrations. Exact33, 29 required omitted-target closures, 193 reviewed
source inputs and every authority/profile record are unchanged. Full source,
runtime and later required Ninja/header audits are not filtered or replaced.

Window `2026-10-21T23:22:48Z`–`23:28:46Z`; existing reviewed amd64 image
`sha256:0c8050c100ee91acab20ea1c47bdb47968ac8c5fc41669451a6fc41254a44724`,
nonroot, read-only source/Git mount and network disabled; no image build.
Compact comparison `/tmp/nostrc-shared-audit-20261022/comparison-summary.json`,
SHA-256 **`99bb37b12403df027c852aab570e97c8f5d444ce60a27f9d84288538d261b9ca`**,
binds raw before/after audits, exports, File API witnesses and exact removed sets.
These are **prebuild** results on `693d76795`, not relabeled final-tree exports:
later changes add only test-history projection/source closure and formatting.
Production selected-profile build/test, fresh physical omitted-target builds and
production postbuild header audits remain **NOT RUN**. Omissions stay
`not_executed`, `complete=false`, `complete_gate_eligible=false`.

### Preserve historical resource guards across PR #9

The first full selector run at `693d76795` **FAILS: 496 tests, two failures,
372.935s**. Both guards correctly reject historical helper hash `ae7c2628…`
versus current `44f7e7a4…`; the current helper is byte-identical to master37e144cab.
No original fixture/hash was replaced. The separate
[`resource_diagnostics_projection_20261022.json`](../../tests/selection/fixtures/resource_diagnostics_projection_20261022.json)
witness retains both original fixture byte hashes, exact four reverse hunks,
fresh authored registration/descriptor evidence and PR #9 native resource
nine-case receipts at `a3aa27ee7`. Current native Grotto's 461 names and affected
resource descriptor contracts agree with their historical witnesses.

[`project_resource_helper`](../../tests/selection/resource_export_projection.py)
requires the exact current helper hash, reconstructs the exact historical hash,
and checks unchanged six resource-case bodies, existing setup and child-call/
failure/return behavior. Only the three known diagnostic regressions are added;
unknown byte changes still fail. The historical PR #9 nine-case results are
**not re-execution here**, and the original timeout cause remains unresolved.
The witness's authority hashes are dated evidence only: this projection does
**not** compare against or freeze live authority records. Task-scoped unchanged
records/profiles are independently verified against master37e144cab in
`/tmp/nostrc-shared-audit-20261022/task-scoped-authority-proof.json`; actual admission
validators retain their existing live-source authority.

### Final gates and unchanged acceptance boundaries

- Final **`dba601f22`: 496/496 selector PASS, zero skips, 211.531s**;
  `env XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -q`.
  Focused graph **2**, resource contracts **4**, Grotto source guard **1** PASS.
  Intermediate `784abcc29` **496/496 PASS, 216.318s** remains separately retained;
  it is not substituted for the final decoupled-tree result.
- Fresh safe-backend native normal configure/all-default build PASS at
  `693d76795`; compiled product and resource-helper bytes are unchanged through
  `dba601f22`. **Full native CTest NOT RUN**, intentionally for this inventory/test-only
  work, not borrowed from PR #9 or earlier native passes.
- Four final statics, actionlint and whitespace PASS at `dba601f22`; all six
  shell suites PASS at `693d76795` with their runtime scripts unchanged afterward.
  One bounded actual graph-diff review found no blockers after stale packaged
  input was replaced with current inline code. Parent inspected the narrow
  history projection and requested the applied authority decoupling; no repeat Oracle.
- Retain the first selector failure, initial miniature fixture failures, the
  Docker single-directory bind/import failure (before configure), all raw failed
  full audits, and all earlier historical failures. No guard relaxation, timeout
  change, hidden retry, source renewal or acceptance promotion.
- `.2` remains OPEN; `.9` remains in progress. Official `bd export --all` preserves
  existing issue/memory rows. `bd dolt pull` refused `no remote`; no sync success.
  No `.5`/W11 ledger/counter refresh, defaults, release/protection changes or hosted
  credit. Local commits only; parent owns verification, push, merge and cleanup.

## Bounded service producer ownership — 2026-10-22

**Implementation:** `71d57a83c`; explicit regression-source closure correction
`1af1772d128849dd77a25c94d6165941f224388b`, from master `705d7b051` (PR #7).
This is a metadata-only correction under `.2`/`.9`, **not a W11 comparison,
ledger refresh or acceptance promotion**. No shipped component version bump.

The source-linked PR #7 final-authored proof and its producer/template/helper
hashes were verified unchanged. Its 9,181-finding Grotto preaudit contains two
real impact gaps for `grotto_dbus_service_configured`: the newly proven
[`apps/grotto/CMakeLists.txt`](../../apps/grotto/CMakeLists.txt#L496-L520)
producer was classified only as GUI/version input, and its
[`org.nostr.Signer.service.in`](../../apps/grotto/data/org.nostr.Signer.service.in)
template as GUI/selection-contract input. The exact producer path now also owns
`grotto.daemon`; the Signer template replaces GUI ownership with daemon ownership
while retaining the selection-contract fixture consumer. GUI service/template
ownership stays separate. No daemon→GUI or catchall prerequisite is introduced.

[`test_service_impact.py`](../../tests/selection/test_service_impact.py) executes
the actual configure/helper/test statements and real descriptors/checker/templates
in a miniature CMake project. Counterfactual old ownership reproduces both gaps;
independent negatives, selected/omitted source/runtime and postbuild header audits,
no invented target, real configured-service CTests and bound reloads are retained.
The new test is explicitly declared in `selection.contract`'s source closure.

Groundhog inspection confirms the real
[configure→READ→configured header→background target](../../gnome/groundhog/CMakeLists.txt#L1504-L1520)
chain and [compiled template use](../../gnome/groundhog/src/app/gh-background.c).
The existing `groundhog.resources` path rule already selects the declared
`sanitizer.groundhog_background` check. A regression pins that route without
adding dependencies to `groundhog.unclassified`/execution nodes or pulling MDK
for the template. Ordinary background registrations remain unclaimed; this is
not new configured-header provenance, ordinary membership or postbuild acceptance.
No Groundhog graph/product change was justified for that already-covered check.

### Final source-bound full preaudit comparison

Fresh authored Linux/x86_64 configurations independently bind master and repaired
inventories on committed `1af1772d1`; no inventory-identity guard is bypassed.
All four exports reload with their original effective environment and bindings.

| Profile | Registrations / targets | Before → after, both FAIL | Removed / added | Identical full input hashes¹ |
|---|---:|---:|---:|---:|
| `groundhog.linux` | 422 / 552 | **9,325 → 9,325** | **0 / 0** | 1,316 |
| `grotto.linux` | 461 / 603 | **9,181 → 9,179** | **2 / 0** | 1,700 |

¹ Only independent build-directory prefixes are normalized; every hash is equal.
Both removed findings are exactly the daemon service's producer/template
`missing_impact_edge`s. Grotto retains 8,810 missing-impact and 369 unclaimed
findings; Groundhog retains 8,951 missing-impact, 373 unclaimed and one owner
disagreement. The configured service registrations remain unclaimed. Exact33,
29 required omitted-target closures, 193 reviewed source inputs, all authority
records/profiles and PR #6 portable/UI/resource optimization remain unchanged.
No source-renewal churn, filters, exclusions or new nonmember policy.

Raw configure/preaudit window `2026-10-21T22:03:54Z`–`22:12:25Z`; retained-export
verification finished `22:14:27Z`. Existing reviewed amd64 image
`sha256:0c8050c100ee91acab20ea1c47bdb47968ac8c5fc41669451a6fc41254a44724`,
nonroot, read-only source/Git mounts, network disabled; no image build.
Evidence `/tmp/nostrc-service-impact-20261022/final-comparison-summary.json`,
SHA-256 `7b04e3bd6bab9458b6b0f7f96c03481d87da91ea6bdae4bb296472f3ee994e4d`,
binds each raw audit/export and exact removed set. These remain prebuild audits:
no production selected-profile build/test, refreshed omitted-target physical
build or postbuild header proof. Omissions stay `not_executed`, `complete=false`
and `complete_gate_eligible=false`; all other failures remain blocking.

### Gates and retained failures

- Final `1af1772d1` selector suite **494/494 PASS, zero skips, 673.947 seconds**;
  `env XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -q`.
  Log SHA-256 `e174ae797327c11ab97abd7c3fece54ea911c3968dd7a627555e9ff3fc52d70e`.
  First 494-test run failed the explicit source-closure guard (243.730 seconds);
  declaring the new regression fixed it, not weakening the guard.
- Safe native normal-path configure/all-default build **PASS** at `71d57a83c`,
  libsecret/secret-service overrides. Product/test runtime bytes are unchanged
  from master through `1af1772d1`; default Groundhog is OFF.
- Native full CTest **FAIL**: **433 passed, 5 skipped, 1 failed**, 520.18 seconds,
  exit 8. `nip55l_approval_service` aborts `/approval/expiry-queued-revision` at
  [`test_signer_approval_service.c:404`](../../nips/nip55l/tests/test_signer_approval_service.c#L404):
  pending length **2**, expected **1**. Cause/inherited/flaky classification is
  unproven; separate bug **`nostrc-2omeg`** retains the seed and exact evidence.
  No assertion/product edit, waiver, baseline execution or whole-native retry.
  Private home/bus/throwaway Secret Service, loopback/Unix-only sandbox; processes
  reaped, socket absent, read-only keychain guards PASS. The five existing skips
  remain visible in the retained log; this run is not a passing mandatory gate.
- Six shell suites, four static checks, actionlint and whitespace checks PASS.
  One bounded code review found no blockers after stale packaged review input
  was replaced with the actual inline diff and complete regression code.
- All failed attempts remain under `/tmp/nostrc-service-impact-20261022`: initial
  baseline/current inventory-binding refusal; final wrapper's incorrect extra
  command-input expectation (both raw preaudits had completed with identical
  CTest input-hash sets); and retained-export reload refusal in a new container's
  different hostname environment. Docker lifecycle evidence recovered the exact
  original hostnames; unchanged environment guards then admitted all four reloads.
  No audit failure was suppressed or failed wrapper relabeled successful.
- `.2` stays open, `.9` in progress; the separate native bug stays open. Official
  all-record `bd export --all` preserves prior rows. `bd dolt pull` refused
  `no remote`; no synchronization success is claimed. Local commits only;
  parent assesses mandatory gates and owns review/push/merge/cleanup. No `.5`,
  W11 ledger/default/release/protection changes or complete/release acceptance.

## Bounded configured-service byte provenance — 2026-10-22

**Local implementation:** `dc2700cd5ac44579fc51092689bd3c5cb5758d1a`; source-only
renewal and standalone-test isolation `606215453`; historical-export projection
`973c09ee6`, from master `a14b8b38cbd1b350324c2fbfed227592ede3ab2b`.
This repairs the two configured Grotto service runtime-provenance gaps, not the
remaining ordinary full-audit failures. It is **not a W11 comparison, ledger
refresh or acceptance promotion**.

### Actual producer and supported byte provenance

The builtin `configure_file(... @ONLY)` calls in
[`apps/grotto/CMakeLists.txt`](../../apps/grotto/CMakeLists.txt) produce
`org.nostr.Signer.service` and `org.nostr.Grotto.service` during configure, with no
build target. The service-contract tests consume those files through the real
`CheckDbusServiceFile.cmake` checker. File API `cmakeFiles-v1` corroborates the
directory producers/templates, but neither it nor the final file hash alone
proves that a later command did not overwrite the configured result.

Both builtin calls remain unwrapped and unchanged. Only selection-token
configurations call the explicit
[`NostrcConfigureProvenance.cmake`](../../cmake/NostrcConfigureProvenance.cmake)
helper immediately afterward. It records the actual producer/template/output
hashes at that point. Realization cross-checks the complete expanded trace,
direct directory caller, exact helper append and token manifest, File API inputs
and directory contexts. Producer/template/helper/output must still have those
hashes at audit/load time; trace, manifest, File API reply and output bytes are
generation-bound. No synthetic target, command allowlist, interception, replay or
generic CMake interpreter is introduced. Unsupported/unrecorded/pre-project,
included/ambiguous and custom-overridden cases remain refused. Unchanged later
bytes retain their exact known result; changed bytes do not inherit the template's
provenance.

Real CMake regressions cover later `file(WRITE/APPEND/GENERATE/COPY/COPY_FILE/CONFIGURE)`
and `execute_process` overwrites, missing/changed producer/template/helper/output,
trace/manifest/File API tampering and custom definitions. Full selected **and
omitted** source/runtime audits and postbuild discovered-header audits remain in
the miniature tests. No product runtime, target, image or gitlink changes.
Normal configure artifacts are unchanged; **no shipped component version bump**.

### Deliberate source-only admission renewal

Parent explicitly authorized review of the necessary source changes, not a new
policy authority. Fresh actual Linux/x86_64 authored configurations first prove
the old source guard refuses `apps/grotto/CMakeLists.txt`. Renewal changes only
that reviewed hash and adds the helper (192 → 193 source inputs). Existing
profiles, exact33 lists, universes, candidate/toolchain contracts, primary/fixture
closures, 29 required audit targets, workflow bindings and sanitizer authority
are unchanged. The original witness fixture is unmodified; the
[new source-renewal witness](../../tests/selection/fixtures/ordinary_subset_source_renewal_20261022.json)
retains the full previous review and reconstructs its original record hashes.

| Authored profile | Registrations / targets | Raw → source-renewed full preaudit | Full input hashes |
|---|---:|---:|---:|
| `groundhog.linux` | 422 / 552 | **9,358 → 9,325, both FAIL** | 1,316 identical |
| `grotto.linux` | 461 / 603 | **9,214 → 9,181, both FAIL** | 1,700 identical |

Only the exact33 unclaimed-registration findings per profile are reclassified;
no other finding is removed and no finding is added by renewal. Grotto's two
generated-runtime gaps are absent, but auditing the newly proven producer exposes
one additional real producer-impact gap for `grotto_dbus_service_configured`.
Thus the prior accepted-source Grotto baseline is **9,182 → 9,181**, not a green
audit. Groundhog's owner disagreement, all remaining missing-impact and unclaimed
findings stay blocking. Omitted registrations remain `not_executed`, with
`complete=false` and `complete_gate_eligible=false`; no selected-only fallback.

These are **prebuild audits** (`after_build=false`), including the candidate audit
after source renewal. The existing review-schema `post_audit_sha256` now references
that post-renewal **preaudit**, not a postbuild header audit. Fresh renewal did
**not** physically rebuild the 29 omitted-target closures or execute a selected
production profile. The older 29-target physical-build/postbuild witness remains
historical, not refreshed. The two real configured-service CTests pass as direct
configure-only diagnostics; that does not execute omitted registrations or
establish profile acceptance.

Renewal proof window `2026-10-21T20:51:49Z`–`20:55:50Z`, dirty source
`dc2700cd5`, snapshot
`1ebc2af6d9e49cba54865dffbf90e427b69fb91144468d90ac366a465dff03ef`.
Evidence: `/tmp/nostrc-configured-runtime-20261022/explicit/renewal-proof/summary.json`,
SHA-256 `45dde8abf3ed4539beb8f2d9ae21fa277feab827c43e1b78ab1abbac4ab0c112`.
Existing reviewed amd64 image
`sha256:0c8050c100ee91acab20ea1c47bdb47968ac8c5fc41669451a6fc41254a44724`,
GCC 13.3.0, CMake/CTest 3.28.3, Ninja 1.11.1; read-only source/Git mounts,
network disabled, no image build.

**Final actual on-disk inventory proof:** committed source
`973c09ee655da76f6be173fe0b9983a911b2d6cd`,
`2026-10-21T21:14:33Z`–`21:16:59Z`. Both fresh authored full-registration
preaudits retain the same failing 9,325/9,181 findings and full 1,316/1,700
input-hash sets; only the two Grotto configure outputs are mapped.
Missing/changed reviewed producer/helper negative controls refuse, and bound
realizations reload successfully. Under the same evidence root,
`explicit/final-authored-973c09ee6/summary.json`, SHA-256
`3c4f872bd776ea6e8a2a52671b622567a40e6bd7269b7ec06cb14a9abfc0179f`,
contains the exact inventory, generation, source/profile identities and physical
execution limits. The earlier clean `606215453` proof is retained separately.

### Verification, preserved failures and handoff

- Exact standalone command (no `PYTHONPATH`, no prior cached test modules):
  `env XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -p test_configure_outputs.py -q`:
  **16/16 PASS**, 16.097 seconds on clean `606215453`; first isolated pass also
  16/16, 35.882 seconds. The parent-reproduced missing import was real; importing
  `ROOT` from established `selector_support` fixes isolation. Ordinary-subset
  **14/14**, source-contract **11/11** and historical Grotto **7/7** pass independently.
- Clean `973c09ee6` full selector suite: **491/491 PASS, zero skips, 801.591 seconds**,
  no blanket `PYTHONPATH` or preceding-suite setup. Log SHA-256
  `dbb9909eb93ff3530efe6fa6753616f3eb6d80497db9c92c03edd19d1c87f9df`.
  The first full isolated run retained one historical-export hash failure (491
  tests, 400.178 seconds). Its fix preserves the archived native/ASan export
  unchanged: removing only the exact two reviewed metadata blocks reproduces the
  old source hash, while current bytes must match the fresh x86 renewal. No
  sanitizer/architecture acceptance is inferred from that historical projection.
- The unusually slow final run was investigated live rather than restarted. The
  native thread sample shows `os__fcopyfile_impl`/file I/O and open source/destination
  fixture paths, not a readiness/cancellation wait. The existing command-input
  fixture copy loop is unchanged from master. Python-frame attachment was
  permission-denied; exact Python test attribution and the runtime inflation
  remain unproven. It completed without cancellation/retry. Observation, native
  stack and process/file snapshots remain in `explicit/hang-973c09ee6/`.
- Safe native normal-path configure/all-default build PASS at helper implementation
  `dc2700cd5`; native CTest **434 passed, 5 skipped, zero failed**, 444.06 seconds.
  Private bus, throwaway Secret Service/home, loopback/Unix-only networking;
  processes reaped, socket absent and read-only keychain guards PASS. Groundhog is
  OFF in the default build; this is not authored Linux build/omitted-target proof.
  Visible skips: `test_nip5f_tcp`, `signer/qr-scanner`, `signer/onboarding`,
  `signer/ui-layout`, `signer/ui-accessibility`.
- Six shell regression suites, four normal static checks, actionlint and whitespace
  checks PASS. Parent's bounded actual diff/helper review accepted the explicit
  route; its standalone-import finding is fixed. No further Oracle requested or
  claimed. Earlier Oracle input mismatch/relative-output finding remain retained;
  they are not approval of the final helper.
- Unsound trace-only `5f32b2077` is withdrawn by `301e7abce`. Its real later-WRITE
  false admission, discarded wrapper's observable configure-scope/line drift,
  failed development/COPY fixture attempts and interrupted configurations remain
  under `/tmp/nostrc-configured-runtime-20261022`; none is relabeled a pass.
- `.2` stays open and `.9` in progress for wider blocking audits/execution scope.
  `.5`, dated W11 ledger, defaults, release mode and protection are unchanged.
  Official complete `bd export --all` preserves existing records. No push/merge;
  parent owns landing and cleanup. `bd dolt pull` refused `no remote`; no successful
  tracker synchronization is claimed.

## Bounded GTK resource/portable ownership split — 2026-10-22

**Local implementation:** `e770cbea740d60e6eee620cdc77420e1030d4b2b`, from
master `7219eb4b5dd7e3bd046dd96f846baa26ed08ddbf` (PR #5). This implements the
resource-cost optimization left open by the historical PR #4 repair below;
it is **not a W11 comparison, ledger refresh or acceptance promotion**.

### Actual target boundary, not an MDK filter

[`nostr-gtk/CMakeLists.txt`](../../nostr-gtk/CMakeLists.txt#L199-L276) already
separates `nostr_gtk_portable` (portable sources plus `gn_content_parser` and
`gn_issue_model` objects) from legacy `nostr_gtk` (core/coupled UI sources plus
`GRESOURCE_C`, linking portable PUBLIC). The real ordinary x86 CMake File API
closures confirm conversations includes portable **without** legacy GTK;
the generated `nostr-gtk-resources.c` belongs to legacy GTK, not portable.

The inventory now expresses `nostr.gtk.portable`, `nostr.gtk.resources`, and the
legacy `nostr.gtk` umbrella depending on both. Conversations' prerequisite is
**portable**, retaining PR #5's real transitive dependency. Exact legacy C/header
ownership and the `nostr-gtk/data` prefix follow those target inputs. Shared
Blueprint/resource helpers select both real resource-build checks. Portable
catalogs and unknown new component inputs retain conservative portable impact.
Existing direct check/descriptors' coarse GTK ownership is retained; it is not
permission to weaken source-review bindings or claim minimal ordinary selection.

No CMake, runtime, image, gitlink, membership, profile, exclusion or authority
record changes. Every ordinary reviewed source-input hash remains equal; both
exact-33 ordinary and sanitizer authorities are unchanged. Raw historical witness
fixtures remain untouched: only the two exact parser C/header witness paths map
the former coarse GTK node label to portable in the regression's interpretation.
No witness, transitive source/header audit, fixture or omitted execution is dropped.

### Selection and full-audit comparisons

For shared GTK Blueprint/GResource and representative legacy C/header changes,
CI-context selection changes **76 → 52**, with **all 17 expensive MDK checks
removed** and no new check added. Shared resource-helper inputs change **77 → 53**,
also with zero MDK checks. Portable source/public-header samples retain the exact
same **76-check set**, including all 17 MDK checks. Both resource-build checks and
existing affected UI consumers remain selected. These are logical selection
comparisons, **not execution receipts or production acceptance**.

Fresh independent before/after configurations use the unchanged authored
Linux/x86_64 ordinary profiles on the same current source snapshot: GCC 13.3.0,
Debug, CMake/CTest 3.28.3, Ninja 1.11.1, existing reviewed image
`sha256:0c8050c100ee91acab20ea1c47bdb47968ac8c5fc41669451a6fc41254a44724`.
Source/Git mounts are read-only and networking disabled; no image was built.

| Profile | Registrations / targets | Full preaudit before → after | Identical input hashes | Semantic additions / removals |
|---|---:|---:|---:|---:|
| `groundhog.linux` | 422 / 552 | **9,325 → 9,325, both FAIL** | 1,316 | **0 / 0** |
| `grotto.linux` | 461 / 603 | **9,182 → 9,182, both FAIL** | 1,696 | **0 / 0** |

Raw audits retain the node-identity migration: Groundhog 540 removed/540 added
and Grotto 560 removed/560 added findings. Only portable-input missing-node labels
are canonicalized for comparison; raw audits are unmodified. The remaining
Groundhog owner disagreement, both Grotto generated-runtime gaps and all unclaimed
registrations remain blocking. Exact-33 omission authority/fixture/build-target
accounting agrees before/after; omitted registrations remain `not_executed`,
`complete=false` and `complete_gate_eligible=false`. **No production-profile build,
postbuild header audit or CTest execution is claimed.**

Audit window `2026-10-21T16:44:13Z`–`16:48:15Z`; dirty base `7219eb4b5`, snapshot
`fbbe19c869ec5b2e623bc839778aae527b1507b84587ea0fafa79c5550239799`, inventory
`a1ce3b4556e281279e40788ad47bc92e3c2e2cbcf8a5c7318d93e5e7db3bf29b`.
Local evidence: `/tmp/nostrc-gtk-resource-split-20261022/summary.json`, SHA-256
`e74ed34d1bd112d3551008870c269bbed26fc4220212aaa8e3ff3f2af6153ddf`;
raw independent configurations/audits, full input hashes, selection comparisons,
logs and review exports are retained alongside it.

### Gates and local handoff

- Clean implementation `e770cbea7`: **475/475 selector tests PASS, zero skips,
  209.531 seconds**; the preceding development pass was 475/475 in 222.183 seconds.
- Actual CMake source-list/header ownership and every tracked resource are checked.
  Real native miniature pre/postbuild audits retain selected **and omitted**
  source/discovered-header/runtime inputs; removing either portable or resource
  dependency exposes the expected gaps with identical input hashes. PR #5's
  conversations edge-removal negative audits remain.
- Safe native default root configure/all-default build **PASS** with
  `NIP55L_SECRET_BACKEND=libsecret` and
  `GROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`. Groundhog is OFF in this
  default build; this is not authored Linux/Groundhog build acceptance.
- Native CTest: **434 passed, 5 skipped, zero failed**, 456.41 seconds. Private bus,
  throwaway Secret Service/home and loopback-only networking; both private
  processes reaped and bus socket absent. Read-only macOS keychain checks PASS.
  Visible skips: `test_nip5f_tcp`, `signer/qr-scanner`, `signer/onboarding`,
  `signer/ui-layout`, `signer/ui-accessibility`; no live/native UI acceptance inferred.
- Six shell regression suites, four normal static checks, actionlint and whitespace
  checks **PASS**. One bounded supplied-diff review: **no blockers**. The first
  review call refused stale tool-packaged input; the same chat received the actual
  five-file material before reviewing. Execution results were not independently
  verified by that reviewer.
- Development full-suite failure and initial missing-submodule configure logs are
  retained separately; exact pinned submodules were initialized without gitlink
  changes before the successful native build. No failed attempt is promoted.
- **No shipped component bump:** internal graph/test/documentation changes only.
  `.9` and `.2` retain wider unresolved acceptance scope. `.5`, historical ledger,
  defaults, release mode and protection remain unchanged. Local commits only;
  parent owns push, merge and branch/worktree cleanup.

## Bounded PR #4 full-audit root-cause repair — 2026-10-22

**Local implementation:** `dd2ff1d72460c806775cfafe833d63a3a1efdf13`, from master
`f9d1e853794d81037e8dcf177e40b9bc83178b9c`. This is a bounded worker/audit
repair, **not a W11 comparison, root-ledger refresh or acceptance promotion**.
The existing dated ledger and `.5` are unchanged; historical failures remain.

### Exact hosted source and failure

API capture `2026-10-21T15:55:31Z` records
[PR #4 shadow run 39522018295](https://github.com/chebizarro/nostrc/actions/runs/39522018295)
completed **failure**. API head is `172139a92e617d19d8d1aa3e0b9bd3fe5937e52d`;
the plan and both receipts bind actual tested synthetic merge
`033cc630b1b7f3b728166fe96026c796a70bd301`. The commit API confirms parents
`f39401e12eaac24cbf86e903cdc37198fb332479` and `172139a92`.
Plan ID `7a9949362daff9643309ac973078558d304421cecb25feedac9d3f0f1daf2d93`,
inventory hash `79ad8d8a7884b6afe5df4f6309377ce65691e3ffc78dbc609445631aa1ae680c`.

| Worker | Actual full-registration preaudit | Execution |
|---|---|---|
| [Grotto 118971755374](https://github.com/chebizarro/nostrc/actions/runs/39522018295/job/118971755374) | **9,342** = 8,971 missing-impact + 369 unclaimed + 2 generated-runtime-without-target | Configure passed; build/test `not_executed` |
| [Groundhog 118971755424](https://github.com/chebizarro/nostrc/actions/runs/39522018295/job/118971755424) | **9,605** = 9,231 missing-impact + 373 unclaimed + 1 descriptor-owner disagreement | Configure passed; build/test `not_executed` |

Both receipts are `lane_failure_v1`, `complete=false`,
`complete_gate_eligible=false`; neither establishes successful hosted coverage.
All four ZIP sizes/SHA-256 match API metadata and all member CRCs pass:

| Artifact | SHA-256 |
|---|---|
| [Grotto receipt 12313524094](https://github.com/chebizarro/nostrc/actions/runs/39522018295/artifacts/12313524094) | `d5c9e2890f203d3e6099a69f26f8756a674299daa98baa11e17f49877643dda7` |
| [Grotto evidence 12313429141](https://github.com/chebizarro/nostrc/actions/runs/39522018295/artifacts/12313429141) | `78c8e0d1724f0a9830b9e3505ad9e4af0576b19c09c67269f9d9fc552925ffa1` |
| [Groundhog receipt 12312758821](https://github.com/chebizarro/nostrc/actions/runs/39522018295/artifacts/12312758821) | `b1d3248ce77fc54ceff030da0a9ed895903bc3ece552b73486dfcf6858b3338f` |
| [Groundhog evidence 12312808738](https://github.com/chebizarro/nostrc/actions/runs/39522018295/artifacts/12312808738) | `72c07fb18ba7db369df4960449996ac7b10a153d99c92ac0e62c7f6b1f594016` |

### One shared missing prerequisite

[Groundhog CMake 449–450](../../gnome/groundhog/CMakeLists.txt#L449-L450)
already links `groundhog-conversations` **PUBLIC `NostrGtk::Portable`**;
[`gh-message.c` 364](../../gnome/groundhog/src/app/gh-message.c#L364) calls
`gn_nostr_shared_note_parse()` through its installed portable header. The semantic
`groundhog.conversations` node lacked the existing `nostr.gtk` prerequisite.
The repair adds **that one edge only**, covering transitive media/MLS/group/voice
consumers. Structural comparison against `f9d1e8537` verifies every other inventory
field is unchanged: profiles, ownership, membership, exclusions and authorities.
`groundhog.unclassified` and `execution.groundhog` retain empty dependencies.

New regressions prove real source/header changes select five ordinary transitive
consumers, not unrelated `libgo.channel`, and expose the missing selection on
edge removal. A real CMake miniature audits linked sources/discovered headers
for selected and omitted executions, retaining identical input hashes under
negative edge removal. It is **not** a production-profile pass.

**Existing granularity limitation, not a policy solution:** `nostr.gtk` owns both
portable code and GTK resources. The corrected dependency therefore conservatively
**over-selects MDK consumers for GTK resource changes**; it does not claim those
resources are portable link inputs. The amended resource regression asserts the
exact current MDK consumer set and no broadening, while preserving **no MDK for
app-only Gnostr inputs**. No resource exclusion or finer graph is invented.
Existing MDK link evidence is at CMake 2526–2528 and 2599–2601.

### Fresh authored x86 audits — both still fail

Local Docker Linux/x86_64, emulated on macOS/arm64; **no authored profile override**.
Debug, GCC **13.3.0**, CMake/CTest **3.28.3**, Ninja **1.11.1**. Existing image
`sha256:0c8050c100ee91acab20ea1c47bdb47968ac8c5fc41669451a6fc41254a44724`;
source/Git mounts read-only. Each inventory gets its own normal configure and
hash-bound realization. Baseline policy is the pre-repair inventory on the same
source snapshot, **not historical-source execution**. Only comparisons normalize
build-directory spellings; raw records and bindings are unmodified.

| Profile | Audited registrations / targets | Full preaudit before → after | Removed / added | Identical input hashes |
|---|---:|---:|---:|---:|
| `groundhog.linux` | 422 / 552 | **9,605 → 9,325**, both FAIL | **280 / 0** | 1,316 |
| `grotto.linux` | 461 / 603 | **9,342 → 9,182**, both FAIL | **160 / 0** | 1,696 |

Every removed finding is `missing_impact_edge` for `nostr.gtk`. Unclaimed counts,
Groundhog media-owner disagreement and both Grotto configured-service runtime
gaps remain. Both exact-33 authorities are unchanged, freshly admitted and still
`not_executed`; `complete=false` and `complete_gate_eligible=false` remain.
All registrations, including omitted executions, remain audited. **No production-profile build, postbuild/header
audit or CTest execution is claimed.** Thousands of findings still block it.

Audit window `2026-10-21T16:13:15Z`–`16:17:00Z`; dirty base `f9d1e8537`, snapshot
`a0b3222cb46ed401878eeb9aeb0ed78f082581be1a9884f328e8d4a6c0a67bb9`.
All seven captured source hashes match committed implementation `dd2ff1d72`.
Temporary exact set differences, raw audits, File API and binding/hash manifest:
`/tmp/nostrc-conversations-edge-20261022/attempt2/summary.json`, SHA-256
`96100fb4914fdda7394da957e4e000d89ccb028fa2b89a46f44d04c528a80faa`.
Each profile's `removed-findings.json` sits beside its unmodified before/after audits.

Earlier failed attempts remain: mismatched inventory hash refusal; comparison
helper's unnormalized-path assertion; effective-environment refusal when trying
a different container. Final execution uses separately hash-bound policies,
comparison-only normalization and the original container environment. No guard
was weakened or receipt rewritten. Only the three named exited probe containers
were removed; evidence remains. This is not arbitrary-tree/lifecycle acceptance.

### Gates and local handoff

- Clean implementation `dd2ff1d72`: **471/471 selection tests, zero skips,
  264.246s, exit 0**. Exact log SHA-256
  `3409f5aec340e2d5cb9051f252d3096e167de13628c6b4ed81c8188c4a644ad1`.
  Earlier final-content run passed 471/471 in 223.785s. Original full-run resource
  failures and pre-fix source/header red run remain, not relabeled as success.
- Fresh safe native default configure/all-default build **PASS**, using the
  documented toolchain environment and `libsecret` / `secret-service` backend
  flags. Unix Makefiles, empty build type, Groundhog OFF; warnings retained.
  Full serial native CTest: **434 pass / 5 existing skips / 0 failures**, 439 total,
  exit 0, 470.12s. Loopback/Unix-socket sandbox, private HOME/XDG/bus and throwaway
  Secret Service; both children reaped, socket absent; original-HOME read-only
  keychain guards pass. Compiled product sources are unchanged from `f9d1e8537`.
  Logs: `/tmp/nostrc-conversations-edge-20261022/native-configure.log`,
  `native-build.log`, `native-ctest/ctest.log`, `native-ctest/result.json`.
- Six normal shell suites, four mandatory statics, full actionlint and whitespace
  **PASS**. One bounded core-patch review: **no blockers**, after correcting the
  tool's stale attachment with the actual inline delta in the same chat. The
  subsequent resource-expectation amendment is full-suite tested, not rereviewed.
- `.2` stays open and `.9` stays in progress for their wider existing scope.
  Official `bd export --all`
  preserves **1,512 issue + 37 memory rows**; only those two issues change
  semantically. `bd dolt pull` refuses `no remote`. Per-invocation commit-hook
  override avoids the Beads hook's shared-main export; official owned-worktree
  export and static gates are performed explicitly, without persistent hook changes.

Gate/source hash checkpoint: `/tmp/nostrc-conversations-edge-20261022/checkpoint.json`,
SHA-256 `61e2f2b606e442bcd957a69d62e5c7c05bb28898c0179313a522e50e5cf38741`.
Jev triage/usage notes are in `jev-usage.json` there (109 requests, 883,109 input
tokens, USD 0.037090); the low-precision inventory scan was replaced by actual
node/source inspection. **No component bump:** internal CI metadata/tests/docs.
No push/merge, W11 credit, successful hosted coverage, production-profile/sanitizer/
live-peer acceptance or release/protection/default/enforcement/legacy cutover is
claimed. Parent owns independent focused tests, review and landing.


## Bounded PR #2 ordinary-worker diagnosis — 2026-10-21

**Partial worker inspection only, not a root/counter refresh.** API metadata
captured `2026-10-21T00:20:20Z`–`00:20:26Z` shows
[shadow run 39519271161/1](https://github.com/chebizarro/nostrc/actions/runs/39519271161)
**queued**, conclusion null, while the two ordinary jobs below are completed
failures. The existing dated root ledger and `.5` counters are **not refreshed**;
no successful root, qualifying PR comparison or W11 credit is claimed.

API/job head is `9271b0667d141f979f196e59fd5d617117bc81d8`; both logs and
receipts bind the actual tested checkout to PR synthetic merge
`bc2c9014847207d0f90782e6120c39f025ab6e30`. The commit API confirms parents
`1fad10a239f9cef72b7b056b8ab99679337d53c4` and `9271b0667`.
Plan ID `afbd3888481ea7c935008b6a79b5a73fd4234e844d2bc09ec692650856ad272b`,
inventory hash `79ad8d8a7884b6afe5df4f6309377ce65691e3ffc78dbc609445631aa1ae680c`.
Both logs prove the prior tool-selection fix actually invoked CMake/CTest/CPack
**3.28.3**, Ninja **1.11.1**; neither fails the previous toolchain guard.

| Completed worker | Actual next failure | Retained scope |
|---|---|---|
| [Groundhog 118958930421](https://github.com/chebizarro/nostrc/actions/runs/39519271161/job/118958930421) | After configure/resolve, `ordinary subset phase parallel/timeout override differs from authored profile` | Authored/worker shared parallelism is 2; ordinary guard incorrectly derives expected 1 from the legacy API's implicit execution default. Build/test `not_executed`. |
| [Grotto 118958930323](https://github.com/chebizarro/nostrc/actions/runs/39519271161/job/118958930323) | Full preaudit FAILED: **9,342** findings = **8,971** missing-impact edges + **369** unclaimed registrations + **2** generated-runtime-without-target | Exactly 33 libgo omissions admitted, still `not_executed`. Generated runtime findings name `grotto_app_dbus_service_configured` / `grotto_dbus_service_configured`. Build/test `not_executed`; not repaired by a parallelism fix. |

All four inspected ZIPs match API sizes/SHA-256 and pass member CRC checks:

| Artifact | SHA-256 |
|---|---|
| [Grotto receipt 12310032325](https://github.com/chebizarro/nostrc/actions/runs/39519271161/artifacts/12310032325) | `3277703c4afad171b5bda86b20224cd99d7c7e625ed604c25a5780208819ff9a` |
| [Grotto evidence 12310037498](https://github.com/chebizarro/nostrc/actions/runs/39519271161/artifacts/12310037498) | `d0463af2c28274a7f98559dabf75086c5b501e466c3eca1d422ba3f50cfe1cb0` |
| [Groundhog receipt 12310816278](https://github.com/chebizarro/nostrc/actions/runs/39519271161/artifacts/12310816278) | `eaae72da3bd95e44e25a3867427a4c19a771df348fccde01aeb6542d8bf5f3a4` |
| [Groundhog evidence 12311081134](https://github.com/chebizarro/nostrc/actions/runs/39519271161/artifacts/12311081134) | `cd4b299277e161e2c20ab8fbd71a6599b543f789a172012faa8952e8aedc1f26` |

Both normalized receipts are `lane_failure_v1`, `complete=false`,
`complete_gate_eligible=false`. Grotto retained `preaudit.json` SHA-256
`d5427a8699d0a9269d0e33c0f14594e463b82d1ddafc6b4709ed2f09bf31e31b`.
No downloaded binary/installer was executed. Raw capture and local gate logs:
`/tmp/nostrc-worker-next-evidence/` (temporary).

The bounded implementation from master `f39401e12` corrects the ordinary
execution guard **and receipt validator** to compare authored split phase values,
falling back to authored `parallelism`, not implicit legacy API defaults.
Legacy API/CLI defaults, worker argv, every profile, exact-33 authority/review
record, source/toolchain/fixture bindings and full audits remain unchanged.
The new miniature regression reproduces the original failure, then proves real
build/CTest argv and normalized receipts at shared 2; mismatched/default 1,
other counts, booleans, timeout overrides and forged legacy-1 evidence refuse.
Existing split build2/test1 and omission/header/runtime/source negatives remain.
This is internal testing only: **no component version bump**.

### Local verification at implementation `bbfa17ce2`

- Exact-commit full selection suite: **469/469 PASS, zero skips, 195.775s,
  exit 0**, `caffeinate -i /usr/bin/env XPC_SERVICE_NAME=0
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v`.
  Final log SHA-256 `579ac2cf89adc0649829e53dfc68bf893db8faaecd7cfcdf17bab45d0c78ac4c`.
- Focused ordinary subset: **14/14 PASS**. Before the correction, the new test
  fails with the actual parallelism-override error (**14 cases, 1 error**);
  that log remains. The first full run also printed **469/469 OK, 210.225s**,
  but its outer zsh wrapper failed assigning read-only `status`; the final
  clean bash rerun above has independently recorded exit 0. The native-build
  wrapper had the same post-command error; configure/build were rerun cleanly.
- Mandatory safe native default configure/default-all build: **PASS, exit 0**,
  with `~/groundhog-smoke/toolchain/env.sh` and
  `-DNIP55L_SECRET_BACKEND=libsecret
  -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`. Default Unix Makefiles,
  empty build type, `BUILD_GROUNDHOG=OFF`; the unused Groundhog-variable warning
  remains. No native root CTest or GUI/keychain application execution is claimed.
  Read-only post-build keychain guard passed; no keychain configuration changed.
- Six shell suites (`test-pre-push`, `test-full-gate`,
  `test-linux-gate-lifecycle`, `test-linux-gate-smoke`, `test-sanitizer-gate`,
  `test-test-gate`), all four mandatory static checkers, actionlint and whitespace:
  **PASS**. Shell-log concatenation (listed order) SHA-256
  `f28f6313c7b0cbd34ade4b73be1a91b23e1e35e0d3e5e2231803b614a22c93bb`.
- One bounded static review: **no blockers**, after the tool's stale October 19
  attachment was corrected with the actual four-file inline delta in the same
  chat. No broader rereview. Official `bd export --all` preserves all **1,512
  issue + 37 memory rows**; only `.31`, `.9`, `.2` have changed semantic content
  (unchanged memory rows may reorder). `bd dolt pull` still refuses `no remote`.
  The repository commit hook also exports to the shared main checkout's
  `.beads/issues.jsonl`; its staged/unstaged state is left for the parent, not
  reset or committed from this worktree.

`.31`, `.9`, `.2` remain open for the unrepaired full-audit/profile scopes.
Groundhog reaching/passing its own full audit and both hosted selected builds/
tests at this fix: **NOT RUN**. W11 **NOT MET**, W12 **NOT RUN**; no authority
expansion, policy/architecture approval, default/release/protection cutover or
acceptance closure. Parent reviews/lands the local commit; no push/merge here.

## Follow-up refresh — 2026-10-21 local / 2026-10-20 UTC

**Frozen API snapshot: `2026-10-20T23:44:45.035117Z`**, sequential capture
`23:41:39.393572Z`–`23:44:45.035117Z`, from implementation base
`1fad10a239f9cef72b7b056b8ab99679337d53c4` (merged PR #1).
The [new ledger section](dependency-aware-testing-ci-2026-10-18-run-ledger.md#follow-up-snapshot--2026-10-20t234445035117z)
records all 14 reviewed root dispositions, source/attempt identities and artifact
integrity. Older snapshots below remain historical, not live counters.

- **14 shadow roots / 14 attempts**, all attempt 1: 12 failed pushes, 1 failed
  PR run, 1 cancelled manual run. All are terminal; the previously pending
  `39196963896` is now a retained failure, not a successful integration.
- **13 failed actual broad family accounting jobs**, 1 selection failure before
  launch. **0/2 successful complete broad integrations**. The 611 nested jobs
  (541 success / 52 failure / 14 skipped / 4 cancelled) are not integrations.
- Repository context since October 15: **190 roots** (167 push / 13 schedule /
  6 PR / 4 manual). All six PR workflow events share head `d120bc972`; they
  are not six comparisons. PR #1 is rejected for qualifying credit: selected
  workers and actual broad family accounting fail; no reviewed successful
  legacy/selective comparison or required-workload union is established.
  **0/20 qualifying reviewed distinct PR comparisons; W11 NOT MET / W12 NOT RUN.**
- 534 API-visible artifacts; 171 small plan/receipt/aggregate ZIPs verified
  (100 cached historical bytes unchanged + 71 newly reviewed archives).
  All sizes/API digests/CRCs and available payload identities pass. Separately,
  12 PR evidence ZIPs were inspected for failure-stage details. No downloaded
  binary or installer was executed; these are inspections, not hosted reruns.

### PR #1 worker findings and bounded next fix

[Shadow run 39280037488/1](https://github.com/chebizarro/nostrc/actions/runs/39280037488)
has API/job head **`d120bc972bbb0f4c09951e376bcc229b41931e0e`**, but its
plan and checkout correctly test synthetic merge
**`39dcccd70e629adb3efb0d904700afe1e45793f9`**. The Git commit API proves
parents `b72b9ec70` and `d120bc972`; the retained job log explicitly records
`refs/pull/1/merge` and that checkout. This distinction is checked, not replaced
with a fabricated head-equals-tested-SHA assertion. Plan ID
`a6ebb802ba08794d50d49d9ec1938fdfa29a4e1907199200ea3a583d3239553a`;
all 12 normalized receipts match its tested source/plan/inventory/lane identity.

| Profile | Retained failure / bounded result | Existing scope |
|---|---|---|
| `groundhog.linux`, `grotto.linux` | Resolve refuses `ordinary nonmember subset: reviewed compiler/toolchain/generator changed`; configure passed, build/test not executed | `.9` / `.31` |
| `core.linux` | Preaudit: 429 unclaimed, 581 missing-impact, 2 generated-runtime-without-target findings | `.9` / `.31` |
| `core.linux.asan`, `core.macos.asan` | Each preaudit retains 3 unclaimed registrations (`MockRelayUnit`, `concurrency_subscription_shutdown_asan`, `test_json_envelope`) | `.9`; no additional nonmember authority |
| `grotto.linux.asan_ubsan` | Preaudit: 321 unclaimed, 277 missing-impact, 2 generated-runtime-without-target findings | `.9` / `.31` |
| `groundhog.linux.sanitizers` | Resolve refuses fresh registration partition mismatch | `.8` / `.9`; original exact policy unchanged |
| `groundhog.mdk08`, `groundhog.mdk011` | Capability refusal: guarded live peer/image lifecycle required | `.8` / `.23` |
| `libgo.linux.asan`, `libgo.macos.asan`, `static.policy` | Success diagnostics (33 / 32 / 1 observed executions), not complete eligible roots | No acceptance credit |

The [Groundhog worker log](https://github.com/chebizarro/nostrc/actions/runs/39280037488/job/118152909114)
shows apt installing CMake `3.28.3-1build7` and Ninja `1.11.1-2`, but the worker
never selected those tools over the hosted PATH. Its recorded
[runner image manifest](https://github.com/actions/runner-images/blob/ubuntu24/20261012.334/images/ubuntu/Ubuntu2404-Readme.md)
advertises CMake **3.31.6** and Ninja **1.13.2**. The exact ordinary contract was
reviewed with CMake/CTest **3.28.3**, Ninja **1.11.1**, `/usr/bin` CMake paths.
Thus installing dependencies without selecting them is a concrete worker setup
bug. **The exact invoked hosted tool versions are not retained in these resolve
failures**; PATH shadowing explains the refusal, but a fresh hosted rerun must
establish whether any additional compiler/graph drift remains. Master push
[39280051357](https://github.com/chebizarro/nostrc/actions/runs/39280051357)
also retains the same ordinary refusal (artifacts `12201691329`, `12200151668`).

The bounded fix selects only distro `cmake`, `ctest`, `cpack`, `ninja` through a
private worker PATH directory, **only for `groundhog.linux` / `grotto.linux`**.
It prints their versions and refuses missing tools before publishing PATH.
Pinned Blueprint and other tools keep their existing lookup. No profile or
review hash is rewritten; the full exact toolchain, graph, source, fixture,
input and header audits still decide admission. Exactly 33 ordinary omissions
remain `not_executed`, with `complete=false` and `complete_gate_eligible=false`; no sanitizer/core
policy extension, selected-only audit fallback, default/enforcement/release,
legacy trigger/schedule/protection change or unmet-criterion closure.

**Final local gates:** `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover
-s tests/selection -v` under `caffeinate -i`: **468/468 PASS, zero skips,
201.382s, exit 0**. Focused workflow contracts **38/38 PASS**; all six existing
shell regression suites (`test-pre-push`, `test-full-gate`,
`test-linux-gate-lifecycle`, `test-linux-gate-smoke`, `test-sanitizer-gate`,
`test-test-gate`), four mandatory static checkers, actionlint and whitespace PASS.
All 89 job-accounting records validate; only the worker job's two source hashes
changed in inventory. Every profile, exact-33 review record and other inventory
field equals the base. The first full run retained **468 cases, 1 failure +
2 errors**: a portable `/var` versus `/private/var` test assertion (already
corrected but loaded before the edit) and stale worker source-accounting hashes.
No test was skipped/weakened; both earlier focused/full failure logs remain.

One bounded static review completed with **no blockers** on the authoritative
five-file delta. The review tool initially supplied an unrelated older policy
diff; that blocked input was corrected in the same chat with the actual inline
delta, not treated as a review of this change. No additional broad rereview.
**Hosted rerun, full production-profile/root configure/build/CTest and release
validation at the changed worker source: NOT RUN.** Shell regressions and the
miniature below do not substitute for those gates.

Final selection log SHA-256
`2e9d87c872915cebecbc25e564144a41d7f511a877390076b09ff2858271d20d`;
first failed full log `32568f810a994b2b91fa96f8a1089d68e635f8384a395b582128f0ceb52d3748`;
six-shell log `396387fe02ddddcb4458c895c0794291086ca964e22e930e36e2df5106fe9c9f`.
No policy relaxation or hosted success inferred from their exits.
A Linux/amd64 miniature (existing pinned local image, not a GitHub runner)
executes the actual worker setup shell with poisoned higher-PATH tools, then
configures/builds/tests one C target. Its CMake File API identity and CTest/Ninja
versions match the reviewed tool fields, and 1/1 CTest passes. This does **not**
prove the full profile toolchain/candidate contract or production acceptance.
Probe result SHA-256 `3236f82594052ea02f5418227fd2a459aa954f0c7b8c9b3421ca99d173900177`;
raw evidence `/tmp/nostrc-ci-followup-20261021/` is temporary.
`.2`–`.5`, `.9`, `.31` and other unmet scopes remain open.
**Version assessment: no component bump; internal CI/test/docs only.**

## Earlier snapshots retained

All following content, including its dated “current” headings, is unchanged
historical evidence. Use the newer explicit snapshot above for refreshed counts.

## Current API refresh — 2026-10-19

**Frozen snapshot: `2026-10-19T04:10:01.512997Z`.** Sequential read-only API
capture window `2026-10-19T04:09:10.542307Z`–`2026-10-19T04:10:01.512997Z`;
per-query UTC windows and response hashes are in the
[current run ledger](dependency-aware-testing-ci-2026-10-18-run-ledger.md#current-canonical-data).
This refresh covers API-visible run/attempt/jobs/artifacts, not new production,
CI or test implementation, a live dashboard, or an atomic server snapshot.

### Current totals and dispositions

| Evidence | Observed at this snapshot | Acceptance disposition |
|---|---|---|
| Shadow workflow roots / attempts | **9 / 9**, all attempt 1: 7 failed pushes, 1 cancelled manual, 1 pending push | Failed/cancelled/pending history retained; no retry counted as a comparison |
| Actual nested broad accounting | **7 failures**, 1 prelaunch selection failure, 1 incomplete call | **0/2 successful complete broad roots**; never count parent colors, subjobs or skipped jobs |
| Repository context since 2026-10-15 | **160 roots**: 147 push, 9 schedule, 4 manual; **0 PR events** | **0/20 qualifying reviewed distinct PR comparisons**; no legacy/selective pair accepted |
| Artifact inventory | **292 API-visible**, **100 selected small archives**, 192 other archives not downloaded | 59 historical archives revalidated unchanged + **41 new**; no binaries/installers executed |
| Reusable broad / worker / release standalone histories | **0 / 0 / 0** | Not an execution count: real nested broad jobs exist; no exact-tag publication validation is established |

All four workflow histories and repository context are paginated and match their
API totals. Every returned root's attempts `1..run_attempt` was enumerated, and
attempt/job source identities checked. Deleted/inaccessible history is not
inferred. Existing qualification policy is unchanged: **W11 NOT MET / W12 NOT
RUN**, `.5` stays in progress; no status promotion or closure follows this refresh.
W1 measurements, reviewed comparison/required-workload union and release/operator
acceptance remain missing, not replaced by diagnostics or another local test run.

### Newly reviewed source-bound roots

- [39194059430/1](https://github.com/chebizarro/nostrc/actions/runs/39194059430),
  source `5280ea324ec474e9d0644638565a2e5a01748b51`: **completed failure**.
  Its 47 nested broad jobs are 43 success, 3 failure, 1 skipped. Failures are
  [macOS core ASan/UBSan](https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872921493),
  [JSON fuzz](https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872963370),
  and actual [family accounting](https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117882302404).
  Fourteen small archives bind plan
  `fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4`.
  Aggregate [12162288486](https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162288486)
  requests `complete=true` but reports **`passed=false`,
  `complete_gate_eligible=false`**, 467 errors. Requested completeness is not
  successful coverage. The historical Debug configure failures still occur in
  `core.linux` and `groundhog.mdk08` at this source.
- [39194781902/1](https://github.com/chebizarro/nostrc/actions/runs/39194781902),
  source `2c5b18c23344b3a4c32faf2738e6c34064ab1af7`: **completed failure**, not
  the active parent described by the earlier two-lane observation below.
  Its 47 nested broad jobs are 40 success, 2 failure
  ([JSON fuzz](https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875216973)
  and [family accounting](https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117883493320)),
  4 cancelled Grotto variants (including explicitly advisory TSan), 1 skipped
  coverage report. Fourteen small archives bind plan
  `368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368`.
  Aggregate [12162865868](https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162865868)
  requests `complete=true`, but **`passed=false`,
  `complete_gate_eligible=false`**, 465 errors. No cancellation/skip is waived.
  Both previously verified lane ZIPs `12161152656` and `12161662149` were
  downloaded afresh; their archive/member hashes match the earlier observation.
  Resolved and phase configuration are Debug; `core.linux` now fails preaudit
  and `groundhog.mdk08` fails capability admission, not configure agreement.
- [39196963896/1](https://github.com/chebizarro/nostrc/actions/runs/39196963896),
  source `e57ef16ed1aeff152d808aec407c1b6f86bfc6be`: parent **pending**, not
  unlaunched. It already exposes 45 nested broad jobs: 34 success, 1 failed
  [JSON fuzz](https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117883093746),
  7 in progress and 3 pending. Family accounting, coverage report and dependency
  gate/aggregate are **not yet returned**, not assumed permanently absent or
  successful. Thirteen small archives bind plan
  `237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a`:
  1 plan and all 12 selected lane receipts (3 success, 9 failure). A completed
  selected subset is not a completed broad root. Later status/uploads are
  outside this frozen capture and must be reviewed separately.

Exact-source broad workflow accounting requires every required family result to
be success. Actual accounting failures—not the root conclusion, successful
package/interop jobs, empty standalone history or aggregate `complete` request—
exclude the completed candidates. No full required-workload union is accepted.
Across all nine roots, **374 nested broad job observations** (315 success,
38 failure, 7 skipped, 4 cancelled, 7 in progress, 3 pending) remain job counts,
not 374 root integrations. No new fuzz/macOS failure cause is inferred from
conclusions alone; the older log/call-stack evidence below stays source-bound.

### Current selected receipt findings (latest source `e57ef16ed`)

These are emitted prebuild finding counts, **not distinct root-cause bug counts**.
All nine failed lanes remain incomplete/ineligible. The three successful lanes
are bounded static/standalone diagnostics, not full root/profile-union acceptance.

| Profile | Current retained observation | Existing scope |
|---|---|---|
| `core.linux` | Debug resolved/phase agreement; preaudit: 429 unclaimed registrations, 1,380 missing-impact findings, 2 generated-runtime-without-target findings | `.9` / `.31` |
| `core.linux.asan`, `core.macos.asan` | Each preaudit retains 3 unclaimed registrations | `.9`; no new nonmember admission |
| `groundhog.linux` | Preaudit: 408 unclaimed registrations, 9,475 missing-impact findings, 1 owner disagreement | `.9` / `.31` |
| `grotto.linux` | Preaudit: 402 unclaimed registrations, 9,104 missing-impact findings, 2 generated-runtime-without-target findings | `.9` / `.31` |
| `grotto.linux.asan_ubsan` | Preaudit: 321 unclaimed registrations, 330 missing-impact findings, 2 generated-runtime-without-target findings | `.9` / `.31` |
| `groundhog.linux.sanitizers` | Resolve refuses `diagnostic nonmember fresh registration partition differs` | `.9` / `.8`; exact existing policy only |
| `groundhog.mdk08`, `groundhog.mdk011` | Debug resolved/phase agreement; live peer/image lifecycle capability refusal | `.8` / `.23` |
| `libgo.linux.asan`, `libgo.macos.asan`, `static.policy` | Success receipts, respectively 33, 32 and 1 observed executions; `complete_gate_eligible=false` | Bounded diagnostics, not qualifying roots |

The prior `target_mapping` finding is no longer emitted in `core.linux`,
`groundhog.linux` or `grotto.linux.asan_ubsan` at this source. Other membership,
semantic/runtime and lifecycle failures remain; this is not a full-audit pass or
closure of the concurrent inventory/semantic-edge work. No inventory, code or
tests were changed or independently exercised in this docs-only refresh.

### Integrity, historical retention and verification limits

All **100/100** archive digests and sizes match the frozen API metadata; ZIP CRCs
pass and member hashes are retained. The set comprises 8 plan payloads, 7 failed
aggregate payloads, 56 normalized lane receipts, 9 historical raw failed audits
without payload plan/lane fields, and 20 historical log-only archives. Only the
normalized payloads assert full source/plan/inventory/lane identity; raw/log-only
archives bind their available fields plus API/run/source and attempt-suffixed
name/member-path provenance. Missing payload identity is explicit, never fabricated
or upgraded to coverage. All attempt suffixes are 1; backend retry counters are
not GitHub attempts. No binary execution, installations or broad raw-evidence
artifact retrieval was performed.

Fresh inputs/scripts/logs: `/tmp/nostrc-acceptance-refresh-20261019/`.
`capture.py`, `validate_artifacts.py --cached` and `build_refresh.py` check API
pagination/source/attempts, digest/CRC/payload identities, actual broad accounting,
root-deduplicated counters and historical hash equality. Their exact hashes,
API/workflow/result hashes and all 41 new archive/member hashes are in the ledger.
An initial local validator assumption failed on a historical raw audit's absent
`plan_id`; the corrected classifier records that limitation explicitly. Both
logs remain retained; no failed hosted attempt was rewritten. Raw local files
and GitHub artifacts are not permanent storage.

The original `2026-10-19T03:01:13.808922Z` ledger snapshot (reconciled
`03:11:57.005478Z`), all six prior failed/cancelled attempts, local receipts,
hashes and the earlier two-lane `03:40:29Z` observation remain historical below.
Historical protection/release-mode observations and local quality gates were
**not re-queried or rerun**; none is presented as new/current acceptance evidence.
This is documentation/evidence plus a `.5` comment only; no tracker export,
closure/status promotion, release/default/protection change, push or merge.
**No component version bump: documentation-only.**

## Historical criterion audit and follow-ups retained

Everything below is the original source/time-bound audit and follow-up text,
including its “current” wording and original local evidence/hashes. Use the
explicit newer snapshot above for current API-visible counters and run state.
The original implementation audit was not re-executed by this refresh.


**Audit date:** 2026-10-18. **Audited implementation:**
`2743f17354ac13527d8d8919f81abc711396c959`. This is an acceptance/evidence audit,
not another implementation wave or authority to change rollout policy.

The [run ledger](dependency-aware-testing-ci-2026-10-18-run-ledger.md) is the
canonical versioned run-count snapshot. Beads remains the task tracker. Earlier
[staged receipts](dependency-aware-testing-ci-2026-10-15-staged-acceptance.md)
retain their original sources, failures and limits; this audit does not relabel
old passes or discard old failures.

## Result and scope

| Bead | Reconciliation decision | Why |
|---|---|---|
| `nostrc-0cfsg.1` | **CLOSED — finite logical contract accepted** | Its stated logical-selector, Git-range, aggregation, explicit ownership/gap and real-adapter contract is proven below. W11 is not an extra prerequisite for this finite scope. No coverage/enforcement readiness is claimed. |
| `nostrc-0cfsg.2` | **KEEP OPEN — missing root acceptance** | The engine's behavioral regressions and actual standalone execution are proven; a clean current root profile's end-to-end configure/audit/build/selected-test receipt is not. Current hosted root configuration/input audits fail visibly. |
| `nostrc-0cfsg.3` | **KEEP OPEN — implementation/lifecycle incomplete** | Shell/adapter regressions pass, but actual safe native custody and original MDK lifecycle remain incomplete. Prepared-container diagnostics do not fulfill that scope. |
| `nostrc-0cfsg.4` | **KEEP OPEN — hosted/release acceptance incomplete** | Workflow contracts and actual broad execution exist, but selected-worker/full-union failures remain and no new exact-SHA release-validation execution is established. |
| `nostrc-0cfsg.5` | **KEEP OPEN — W1/W11/W12 owner** | This pass completes a source-linked status reconciliation and ledger backfill, not the representative measurements, accepted comparisons, successful broad integrations or approved cutover assigned in its description. |

Decisions use the existing descriptions/acceptance criteria, not a newly invented
universal requirement. A proven logical-selector criterion does not wait for
20 PRs; conversely, a passing miniature cannot prove a production root profile.
No scope is removed, silently transferred or closed merely because a follow-up
bead exists. Unresolved work remains explicitly tracked in the open scopes.

Status vocabulary: **PROVEN**, **MISSING EVIDENCE**, **IMPLEMENTATION INCOMPLETE**.
An observed negative that correctly refuses is evidence for a guard, not positive
coverage for the refused profile.

## Evidence collected in this audit

- From a clean isolated worktree at exact `2743f1735`, actual
  `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v`:
  **437/437 passed, zero skips, 151.176s, exit 0**. This includes real temporary
  Git repositories and CMake/build/CTest mini-projects, not 437 hosted runs.
- All six existing shell suites (`test-pre-push.sh`, `test-full-gate.sh`,
  `test-linux-gate-lifecycle.sh`, `test-linux-gate-smoke.sh`,
  `test-sanitizer-gate.sh`, `test-test-gate.sh`), four mandatory static checkers,
  and actionlint with ShellCheck passed against that same clean source.
- Complete API-visible histories for the four new workflow IDs: **6 shadow
  attempts**, no standalone broad/worker/release runs. Jobs and artifacts were
  fetched with pagination for every candidate attempt. All attempts returned by
  the history have attempt number 1; deleted/inaccessible history is not inferred.
- Repository-wide API context since 2026-10-15: **146 root runs**, comprising
  **133 pushes, 9 scheduled and 4 manual runs; zero PR events**. Pagination totals
  match returned records. This window starts before the new workflows' creation
  at 2026-10-16 01:11 UTC.
- **59 small plan/receipt/aggregate artifact archives** downloaded; all published
  archive digests match, ZIP CRCs pass and member hashes are retained. Every plan,
  aggregate and normalized lane with a source identity matches its parent run.
  Installer binaries were not downloaded/executed.

Raw capture directory: `/tmp/nostrc-acceptance-audit-20261018/`. The committed
ledger retains curated identities/verdicts/hashes and source links, not raw log
or API dumps. Local raw files and GitHub artifacts are not permanent storage;
future acceptance must not assume expired/unavailable bytes still exist.

## `.1` — selector policy, Git ranges and result aggregation

**Existing acceptance criteria:**

> Schema/range/golden/aggregation tests pass including rename/delete/submodule/shallow/newline paths, selected skips/cancellation/stale identities and release applicability; supported-root ownership is explicit, unsupported gaps visible; no success stubs; integration resolve/run delegated to real sibling module. Version: no bump for internal tooling; assess any public build change separately.

| Criterion | Status | Concrete evidence and boundary |
|---|---|---|
| Schema, deterministic impact/goldens, unknown-path broadening | **PROVEN** | `tests/selection/test_selector.py:8–138`: consumer changes do not select prerequisite tests; shared changes select consumers; malformed/cyclic/contradictory policy rejects; current inventory goldens and dirty identity/determinism are checked. All 20 cases pass. |
| Rename/delete/gitlink/shallow/newline and event ranges | **PROVEN** | `tests/selection/test_git_ranges.py:7–154`: 20 actual-Git cases cover both rename paths, base policy for deletions, gitlinks, a real shallow clone, Unicode/newline paths, force pushes, PR tested-merge identity, merge groups and multi-ref unions. |
| Missing/skipped/cancelled/stale result accounting | **PROVEN** | `tests/selection/test_aggregation.py:15–124`: 16 cases reject missing/duplicate/foreign lanes, bad source/plan/profile identities, selected nonpass outcomes, missing independent worker conclusions and hidden sanitizer failures; advisory/quarantine/retry distinctions remain visible. |
| Release applicability, not an actual release claim | **PROVEN** | `tests/selection/test_release_plan.py:26–105`: 12 real-Git/unit cases cover annotated/unknown/global/component applicability, source/version disagreement, dry runs and required package accounting. Current manifest verifies 17 independent components. The existing aggregate identity-negative method is confounded by an empty package map; independent guards are instead verified by the targeted passing-baseline probe below, not overclaimed from that method. |
| Supported-root path ownership explicit; unsupported gaps visible | **PROVEN** | `test_inventory_covers_all_tracked_paths_or_explicit_broadening` (`test_selector.py:124`) classifies every tracked path; current inventory is explicitly `enforcement_ready=false` with coverage gaps. This is path-policy correctness, not a claim of zero production semantic-input gaps. |
| Real sibling integration, no successful execution stub | **PROVEN** | `tests/selection/test_realization_run.py:99–134` executes the actual CMake/CTest backend and normalizes its actual receipt through checked-in `scripts/test-plan.py`. Checkout-binding/CLI modules add 11 passing cases. Production standalone evidence is separately attributed below. |
| Version decision | **PROVEN** | Internal logical testing tooling; no tracked shipped component change in this audit. No component bump. |

These logical modules account for 79 of the 437 passing cases; production
inventory tests and actual realization integration are additional, not silently
included in the 79. Closure is finite logical implementation acceptance, **not**
CI enforcement, broad completeness, all-profile population or W11 approval.

**Independent acceptance review:** reviewed the supplied criterion audit, complete
ledger and source tests; found no blocking issue with finite `.1` closure or the
0/20 / 0/2 accounting. It did not independently execute GitHub queries. One
specific evidence overclaim was corrected: the existing
`test_release_validation_exact_tag_sha_broad_identity` passes an empty package
map, so its negative assertions can reject for missing packages alone. No source
fix is claimed here. A targeted local probe at exact `2743f1735` first passed with every
required package marked successful, then independently changed `tested_sha`,
`tag`, `mode`, `inventory_hash`, `passed` and `partial_profiles`; all six refused
publication for mismatched broad exact-tag validation. This is one synthetic
contract baseline plus six adversaries, not a real publication, PR comparison or
broad integration. The structured probe/result hash is retained in the ledger;
strengthening the permanent regression is tracked under `.4`. No acceptance
scope or rollout requirement was removed.

## `.2` — CMake/CTest realization, runtime ownership and exact execution

**Existing acceptance criteria:**

> Fresh/stale/failed configure, root/standalone export, fixture/reset evidence, model/GUI split, unexpected skip, dirty input cache and missing edge tests pass; real focused configure/build/test smoke runs documented. Zero silently unowned supported registrations, gaps explicit. No production-target refactor or new platform/harness. No version bump unless actual shipped contracts change.

| Criterion | Status | Concrete evidence and remaining gap |
|---|---|---|
| Fresh/stale/failed configure and exact-source freshness | **PROVEN** | `tests/selection/test_realization.py:6–52`, source-identity/toolchain/compatibility modules: real miniature generates, stale/missing replies refuse, failed/external configure invalidates reuse and dirty identities change fingerprints. |
| Fixtures/reset, model vs GUI, single exact invocation, unexpected skip | **PROVEN** | `test_realization.py:54–94` and `test_realization_run.py:15–77`: primary/support closure, one indexed CTest invocation, reset-surviving attempt evidence, GUI skip as failure and absent postbuild audit refusal. |
| Missing source/runtime/header edges are detected rather than ignored | **PROVEN** | `tests/selection/test_realization_inputs.py:5–66`: actual compiled header discovery, omitted-test missing edges, missing/deleted runtime files, empty runtime directories, dirty identity and external symlinks. A failed real audit remains failed. |
| Real standalone configure/build/audit/execute smoke | **PROVEN, bounded** | Current hosted run 39132650260 retained macOS standalone libgo **32 executions** and Linux standalone **33 executions**, registration/input audits passed. Artifacts 12138776972 and 12138414686. Their `complete_gate_eligible=false` is preserved; excluded stress coverage is not invented. |
| Real root end-to-end selector smoke and complete supported runtime ownership | **MISSING EVIDENCE / IMPLEMENTATION INCOMPLETE** | Latest current-source root lanes fail configure or preaudit, detailed below. Ordinary full-root builds/439 native tests from 6299 are valuable separate evidence, not an admitted selector/audit receipt. Root/standalone finalize text assertions at `test_realization.py:97–104` explicitly do not simulate full native exports. |
| Boundaries/version | **PROVEN** | No new platform/harness or shipped target refactor in this audit. Existing resource fixes keep their prior component version assessment; this audit is documentation/evidence only. |

**Next closure evidence:** repair actual authored root configuration and audited
membership/semantic/runtime mappings under existing `.31` / `.9`; obtain an
honest fresh root configure → resolve/audit → build → full postbuild audit →
selected CTest receipt. Unrelated W11 sample counts are not a new prerequisite
for `.2`; current root failures are the concrete missing implementation evidence.

## `.3` — plan-driven local gates and sanitizer parity

**Existing acceptance criteria:**

> All existing and new shell regression cases pass; real focused integration uses shared logical plan/resolve/run; unknown changes broaden; selected missing capability fails rather than skipping; cross-checkout driver lock and cancellation tests pass; sanitizer config parity verified. No component bump for internal gates.

| Criterion | Status | Evidence / blocker |
|---|---|---|
| Shell regressions, unknown broadening and capability refusal | **PROVEN** | All six shell suites rerun successfully; 437 cases include local command, sanitizer-route, prepared-profile and cancellation regressions. `scripts/test-gate.sh:243–250` refuses unsupported capabilities and wrapped non-CTest checks before launch rather than fabricating execution. |
| Ordinary shared-plan execution and separate sanitizer phase contracts | **PROVEN, bounded** | Actual mini-project/command/phase tests execute the shared APIs. Existing explicit shared sanitizer route preserves authored jobs/configuration/environment/input agreement. This does not establish all production profiles. |
| Safe native/wrapped custody and original MDK cross-checkout image/run lifecycle | **IMPLEMENTATION INCOMPLETE** | `.23` / `.8` remain open. `scripts/mdk-gate.py:327–349` deliberately returns nonzero `cleanup_unverified` after otherwise successful commands and always reports `cleanup_complete=false`. Prepared locked routes and 13 historical contained custody cases do not prove the original image-build/arbitrary native-tree path. |
| Real full shared sanitizer profile parity | **MISSING EVIDENCE** | Current hosted Groundhog sanitizer selector refuses its stale exact registration partition. Narrow nonmember admission cannot erase semantic/runtime/input gaps or authorize a different architecture. |

**Next closure evidence:** finish the specifically assigned custody/lifecycle
work and production sanitizer parity; exercise two checkouts, partial/cancelled
image builds, escaped stubborn children and sibling survival with retained raw
status/cleanup evidence. No sleep-based cleanup, PID-name matching, weaker
assertions, successful lifecycle stub or selected-only audit fallback.

## `.4` — reusable CI, broad tiers and exact-SHA release gates

**Existing acceptance criteria:**

> Workflow lint/static and event/result routing tests pass including merge_group, selected skipped/cancelled, validation-only/unknown/annotated/legacy tags and branch/tag dry runs. Existing publishers cannot publish partial validation; no new publishing prefixes or platforms. Broad replacement is executable; retained temporary duplicate triggers are explicit. No version bump for CI-only changes.

| Criterion | Status | Evidence / blocker |
|---|---|---|
| Lint, event/result/tag routing and nonpublishing branch/tag contract tests | **PROVEN, contract level** | Current 36 workflow contract cases and 12 release-plan cases pass; actual actionlint/ShellCheck passes. These are not actual protected-merge or tagged publication executions. |
| Broad replacement launches real legacy workloads | **PROVEN, execution only** | Five nested broad calls reached actual family accounting. Latest run 39132650260 has 47 nested broad jobs: 44 success, 2 failures (JSON fuzz and family accounting), and 1 skipped fuzz coverage-report job. Success of package/interop/BP subjobs is not full broad acceptance. |
| Complete same-source selected CI/family acceptance | **IMPLEMENTATION INCOMPLETE** | Latest selected root/Grotto/Groundhog/MDK workers remain failed for specific configuration, input and custody reasons. Aggregate correctly has `passed=false` and `complete_gate_eligible=false`. |
| Exact-SHA/all-applicable publisher validation is actually established | **MISSING EVIDENCE** | No new release-validator standalone record; reusable calls cannot be inferred absent merely from that fact. No qualifying exact-tag broad validation/publisher gate execution was found in the audited shadow/repository window. Windows branch build successes are not publication-validation attestations. Repository release-mode variable lookup returns 404; checked-in fallback remains disabled. Enforced release routing must be exercised nonpublishing before approval. |
| Temporary duplication/platform boundaries/version | **PROVEN** | Legacy triggers/schedules and publisher/platform prefixes remain; no protection or mode changes. CI-only work has no component bump. |

**Bounded permanent-regression follow-up (2026-10-21):** the earlier
identity-negative test gap is already repaired at migration base `096091aaa`
by integrated `3946ff8d4` (see the historical receipt below): all required package
jobs succeed, the valid baseline passes/publishes, and six independent mutations
fail with only `missing/mismatched broad exact-tag validation`. This continuation
extends that same permanent test to nine independent mutations (adding
`complete=false`, mismatched committed-source SHA and mismatched source kind),
plus seven independent required-field omissions. The unmodified baseline still
passes afterward. Existing missing/failed-package coverage remains separate.
These are release-aggregate unit-contract checks, not execution of the complete
source/profile/capability validation chain or actual publication. No production
release contract, workflow, inventory, mode, default or component version changes.
PR2 hosted run `39519271161` is not evaluated here; no outcome or W11 credit is
claimed. `.4` remains OPEN for wider acceptance.

Local checks on this bounded continuation: release-plan suite **12/12 PASS**;
full selection suite **468/468 PASS**, zero skips, 192.337s; `git diff --check`
PASS. Native macOS/arm64 configure and default build PASS with
`-DNIP55L_SECRET_BACKEND=libsecret`
`-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`. The initial configure
failed because this worktree's pinned `third_party/nostrdb` was uninitialized;
initializing that exact gitlink allowed the retry to pass, without source edits.
No native CTest, private-bus UI or hosted/release acceptance is claimed. One
bounded review found no actionable blockers. Raw local logs (temporary, not
hosted attestations): `/tmp/nostrc-release-identity-20261021/`.

**Next closure evidence:** repair selected worker coverage, resolve broad JSON
fuzz failure under `.29`, and demonstrate the release validation/publisher
contract at exact source with every applicable artifact result, retaining
explicit advisory, validation-only and fork/merge-group behavior. Do not enable
`enforce`, remove legacy gates or create/publish a release as a side effect of
this audit.

## `.5` — operational baseline, progress and integration acceptance

**Existing acceptance criteria:**

> Source-grounded baseline and exact commands/receipts recorded; current plan/progress accurate and AGENTS/testing instructions consistent with actual staged behavior. Integrated quality gates and review findings recorded; external rollout prerequisites remain open tracked work, never fake completion. Internal tests/docs no component bump.

| Criterion / assigned phase | Status | Evidence / blocker |
|---|---|---|
| Source-linked implementation status, commands and retained failures | **PROVEN for this checkpoint** | This criterion matrix, actual current-source local gates, source-bound hosted artifacts and ledger preserve distinct native/diagnostic/hosted results. Older dated receipts stay historical. |
| Actual repository protection/status baseline | **PROVEN, read-only snapshot** | Master reports `protected=false`; classic protection API explicitly says `Branch not protected`; effective branch rules and repository/inherited rulesets return empty arrays. Token has repository admin permission. No settings changed. This is not approval to add/remove protection; merge-queue feature availability was not independently queried, and no effective merge_queue rule was returned. |
| Current local/CI/release policy documented consistently | **PROVEN for staged policy** | Defaults stay legacy, publisher source fallback disabled, incomplete/negative receipts ineligible. Ledger links and refresh obligation are added to operational docs/AGENTS in this pass. |
| W1 representative cost baseline and W11 comparison/performance evidence assigned in description/plan | **MISSING EVIDENCE** | No complete seven-condition, five-sample baseline. Actual accepted counters now verified **0/20 PR comparisons and 0/2 broad integrations**, not inferred from unit counts. Repo window has no PR events at all. |
| W12/approval and external prerequisites accurately tracked | **PROVEN as status accounting, not completion** | W11 NOT MET / W12 NOT RUN. `.5` remains the owner of this ledger and assigned W1/W11/W12 work; recording those phases as incomplete does not complete them. |

The plan's original acceptance still requires five samples per representative
condition, median/range/IQR, 30% Grotto-only and 25% isolated non-MLS Groundhog
median runner-minute savings, zero unrelated live MDK and its 10% regression
limits. It requires 20 representative PR comparisons and two successful complete
broad integrations, reviewed omission accounting, release/protection checks and
operator approval before cutover. This audit changes none of those requirements.

The repository currently works through pushes/manual/scheduled events, so the
PR counter cannot advance by counting those events as PRs. Deliberately exercise
representative PR comparisons, or obtain an explicit amendment permitting a
specified equivalent historical-range replay protocol. Do not invent 20 dummy
PRs or silently relabel pushes to satisfy the quota.

## Concrete current hosted blockers, not generic pending runs

Source: [run 39132650260](https://github.com/chebizarro/nostrc/actions/runs/39132650260)
at `2743f1735`. Lane receipts and archive/member hashes are in the ledger.
These are prebuild emitted findings, **not counts of distinct root-cause bugs**.

| Profile / job | Retained failure | Follow-up owner |
|---|---|---|
| `core.linux`, `groundhog.mdk08` | Configure rejects authored Debug versus realized empty configuration. Current profile configure arguments omit `-DCMAKE_BUILD_TYPE=Debug`; do not weaken the independent agreement check. | `.31` / `.9` authoring |
| `core.linux.asan`, `core.macos.asan` | Each preaudit reports 3 unclaimed registrations: `MockRelayUnit`, `concurrency_subscription_shutdown_asan`, `test_json_envelope`. | `.9`; existing exclusion/admission policy remains binding |
| `groundhog.linux` | 408 unclaimed registrations, 9,473 missing-impact findings, 1 owner disagreement and 1 target mapping finding. | `.9` / `.31` |
| `grotto.linux` | 402 unclaimed registrations, 9,104 missing-impact findings and 2 generated-runtime-without-target findings. | `.9` / `.31` |
| `grotto.linux.asan_ubsan` | 322 unclaimed registrations, 330 missing-impact findings, 1 target mapping and 2 generated-runtime-without-target findings. | `.9` / `.31` |
| `groundhog.linux.sanitizers` | Resolve refuses `diagnostic nonmember fresh registration partition differs`. | `.9` / `.8`; only exact authorized policy may be reconciled |
| `groundhog.mdk011` | Capability refusal: live peer/image lifecycle requires existing guarded adapter. | `.8` / `.23` |
| Broad JSON fuzz | Required job 117691683008 reports LeakSanitizer 514 bytes / 15 allocations through `json_parser_load_from_data` at `fuzz_json_event.c:55`, exit 77. Retained input is `leak-7396472d70f69483739531904dd1db7271706023`. Fuzz coverage-report is skipped; family accounting fails. No skip is counted as success. | `.29`; current log observed, no new fix or upstream attribution claimed |

Approving more nonmembers alone cannot erase missing semantic/runtime/target
findings. Production fixes must preserve full audits and scope/architecture
binding. Root build/type, membership and custody failures are not remedied by
another unmodified 437-case rerun.

## Keeping the count from now on

The ledger stores run/attempt/source identities, actual nested accounting,
normalized lane findings, artifact hashes and explicit dispositions. Its displayed
counters are checked against the structured data. A refresh is required before
reporting W11 progress or proposing bead closure, not every time any CI job ends.
That avoids a documentation-commit/CI/ledger-commit feedback loop.

A future session owns fetching and reviewing new candidates under `.5`; the
repository owner is not expected to keep a manual tally. The ledger is not a CI
auto-updater or proof that future/expired artifacts were reviewed. No counter
advances from a green subset, retry, diagnostic/native test run or merely
`complete=true` in an unsuccessful aggregate's requested-mode field.

## Follow-up repairs after the frozen ledger snapshot

The following local repairs are separate from the historical hosted results above;
they do not revise or erase those failures or advance W11 counters.

- `24d361f19607ea8de9a703054c89390bf1ce7ce8` adds
  `-DCMAKE_BUILD_TYPE=Debug` to `core.linux` and `groundhog.mdk08` in
  `tests/selection/inventory.json`. `AuthoredDebugConfigureContracts` in
  `tests/selection/test_profile_contracts.py` executes miniature real CMake
  configurations, verifies Debug cache/realization/phase agreement, and proves
  missing or contradictory Release arguments still refuse without a realization
  receipt. Independent configuration guards, capabilities and membership are
  unchanged. A separate macOS/arm64 minimal cache/codemodel probe passed for both
  argument lists; this is not production Linux, full-profile or hosted proof.
- `3946ff8d4` integrates the permanent `.4` regression correction in
  `test_release_validation_exact_tag_sha_broad_identity`: a passing publication
  baseline with all required package results precedes six independent validation
  mutations, each rejected with the exact identity/eligibility error. Existing
  missing/failed-package tests remain. This replaces the earlier confounded test
  evidence, not the historical audit-only probe or actual release acceptance.
- At integrated code SHA `3946ff8d49a4d31c89bf5d35e6646b54a5a2eab1`,
  `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v`
  passes **439/439, zero skips, 147.860 seconds**. Log SHA-256:
  `9f06926b96fb675904f8b8bbfe9daf9e00d8e108d3b856da8f002023bc61dfe7`.
  All six existing gate shell suites, four static checks and actionlint pass.
  After initializing pinned submodules in the clean worktree, native macOS/arm64
  root configure and default-all build pass with
  `-DNIP55L_SECRET_BACKEND=libsecret` and
  `-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`, build parallelism 4.
  Groundhog remains default-off; no full-root CTest execution, production Linux
  selector admission or hosted success is inferred. The minimal probe JSON hash
  is `f055bc89fb31b38186f07a06ad6a03ae9fe4288449092a981bcb2ea749004eb4`.
  Logs are local diagnostic retention, not durable hosted artifact acceptance.
- The independently reviewed three-file change has no identified blocker. `.31`,
  `.4` and their parent acceptance beads remain open. A new source-bound hosted
  run and its artifacts must be reviewed before claiming the configure blocker
  resolved in production. Membership, runtime/target mappings, sanitizer parity,
  MDK lifecycle, exact-tag publication validation and rollout prerequisites remain.

### Bounded hosted follow-up: configuration repair

At `2026-10-19T03:40:29Z`, run
[39194781902](https://github.com/chebizarro/nostrc/actions/runs/39194781902)
was still active. Two completed attempt-1 lane receipts bind committed source
`2c5b18c23344b3a4c32faf2738e6c34064ab1af7` and retain both resolved and phase
configuration `Debug`. The previous configure mismatch is no longer their failure.
Both remain failed, `complete=false`, `complete_gate_eligible=false`:

| Profile | Receipt artifact | Verified archive SHA-256 | Current failure |
|---|---|---|---|
| `core.linux` | `12161152656` | `6ce38875cc72169731aabdd4e3734003cd1dbf4484c2e54b1eae89d721979331` | Full prebuild input audit; original findings retained |
| `groundhog.mdk08` | `12161662149` | `099737fc758330b9aaaab114386c71520d16bad47664f4e25e7f2f9e0dc7cd3e` | Live peer/image lifecycle requires existing guarded adapter |

Both downloads match GitHub's published artifact digest and pass ZIP CRC checks.
This is a **two-lane observation**, not a complete run-ledger refresh or a successful
profile/integration. The frozen ledger snapshot and its counters are unchanged;
no W11 progress or bead closure follows. `.31` retains the wider remaining scope.

### Wrapper input/target mapping follow-up

Commit `224209d37838f2a28fd5baaaab248e8df57e53cf` corrects two actual
`target_mapping` findings from hosted source `2743f1735`, run `39132650260`:
artifact `12138577637` is GNostr/GrottoASan; `12138633221` is relayd/GroundhogLinux.

- `tests/CMakeLists.txt` now mirrors app-enable options and GTK/GLib availability
  before registering GNostr wrapper tests. `tests/` precedes `apps/`, so checking
  `TARGET gnostr` there would be premature. Explicitly disabled apps no longer
  register an impossible executable; enabled apps retain sanitizer commands,
  settings and timeouts. The enabled ASan descriptor names real target `gnostr`,
  schema source/generated input and required display capability.
- `apps/relayd/tests/CMakeLists.txt` describes `relayd_bind_iface_integ` with real
  target `nostrc-relayd` and `run_bind_iface_integ.sh`; its temporary configuration
  is generated inline by that script. Neither wrapper command changes.
- Existing `tests/selection/test_realization_inputs.py` covers enabled/default,
  each disabled option and missing dependency states, unchanged sanitizer command
  contracts, File API mappings and real schema/target dependency closure.
  Review correction `abcf04bfd6c0dc74264d06278a8f64834ef19b40` pins the miniature
  fixture to Ninja/Release and proves ambient Debug/Ninja Multi-Config cannot
  change its descriptor/configuration contract. No inventory input addition is
  needed because this existing test file is already authored.
- Actual full-source Linux/arm64 configure/CTest-export/File API/Ninja probes
  record Groundhog 422 registrations (relayd present), GrottoASan with GNostr
  disabled 336 (GNostr absent), and GNostr enabled 400 (GNostr present). The
  corresponding export SHA-256 values are
  `215f3c675933269150219777036c0399b44b21f40b6ba78a585e0f9ebbffa11d`,
  `7afa0116f0b7d4fcee2bb1e01155fa37f8756b7ba49d0c7dce9e2e1a67dffd11`, and
  `360f4fb9904c7e1692e55b42aebed0ecdb1c3e31943509a9e863b50d284afa48`.
  Local checkpoint `/tmp/nostrc-wrapper-mapping-20261019/checkpoint.json`
  (SHA-256 `33e19b3484fdca9bc51848a5a299d6e6b1a713067ff97c1d2c7e057792dccf4c`)
  records arguments, image, pins, source/file identities and setup failures.
  This is **configure/export/graph evidence only**: no Linux product build,
  application execution, full input-audit pass, x86 profile admission or hosted
  acceptance. No exclusions, nonmember admission or audit guards are changed.

At final code SHA `abcf04bfd6c0dc74264d06278a8f64834ef19b40`, independent
`PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v`
passes **444/444, zero skips, 154.531 seconds**. All six shell gate suites, four
static checks and actionlint pass. The native macOS/arm64 default-all root
configure/build passed on the unchanged CMake/product bytes at `224209d37`, using
`-DNIP55L_SECRET_BACKEND=libsecret` and
`-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`, build parallelism 4;
Groundhog is default-off and the latter variable is unused in that configuration.
No native full-root CTest or application execution is claimed. Bounded independent
review found only the fixture portability issue corrected above.

`.9`, `.2` and the wider acceptance scope remain open. Other semantic, membership,
missing runtime inputs and lifecycle gaps are not repaired by these two mappings.

### Guarded NIP archive semantic-impact repair

Code SHA `c6c757b375339db0b685855f4413714707ab9b8f` adds seven prerequisites to
`nostr.gobject`: `nip04`, `nip06`, `nip11`, `nip19`, `nip44`, `nip46`, `nip49`.
They match the guarded eight-archive loop at `nostr-gobject/CMakeLists.txt:240–244`
(two archives belong to NIP-49) and the forwarding contract at root
`CMakeLists.txt:428–462`. Fresh File API links show the actual executable archive
closures; the isolated `test_nostr_gobject_event` consumer supplies the helper
backtrace without competing direct origins. Deduplicated Groundhog links may
retain another valid direct origin; those origins were not discarded.

The existing component graph is a profile union: absent optional targets may
conservatively over-select. This is a specific production prerequisite family,
not catchall ownership; test vectors remain check-local. No profile, registration,
capability, exclusion, nonmember or audit-engine policy changes.

On the **same fresh** 422-registration / 552-target Groundhog Linux/arm64 graph,
applying the original versus repaired inventory changes missing-impact findings
from **9,475 to 9,234**: **241 removed, zero added**, covering 54 registration/node
pairs. Unclaimed registrations remain 408 and owner disagreement remains 1;
both audits fail. The original-inventory comparison is counterfactual metadata
on the new graph, not an earlier-source configure or hosted rerun. Explicit
`x86_64` to `aarch64` diagnostic override is not authored-profile admission.
Separately, replaying frozen hosted receipts removes 218 Groundhog / 132 Grotto /
53 Grotto-ASan findings; those replay figures are not fresh hosted results.

Local checkpoint `/tmp/nostrc-semantic-nip-links-20261019/checkpoint.json`, SHA-256
`102b8dc7b431bdd064e28a9bae43f365f276fbfa58383267f14cdaa8e0d0a18e`, retains
source/input hashes, arguments, raw failed audits, link backtraces and setup
failures. Parent verification independently checks source/evidence hashes and
set difference (241 removed, none added). Regression witnesses cover each NIP
source, retain unrelated libgo savings, and fail without the authored family.
Independent **445/445 selection tests pass, zero skips, 163.983 seconds** at that
code SHA; six shell suites, four statics and actionlint pass. Native macOS/arm64
root default-all configure/build passes with libsecret/secret-service safety
flags and build parallelism 4; Groundhog remains default-off. No native full-root
CTest, Linux build/execution, postbuild/header audit, hosted/W11 or release
acceptance follows. Bounded independent review found no blockers. `.9` and the
wider acceptance beads stay open.

### Ordinary Groundhog store-key membership repair

Code `61d7ecb2af1f61230c60ace365ec5d1b41e3e158` adds ordinary `groundhog.linux`
membership to the existing `sanitizer.groundhog_store_key` and
`sanitizer.groundhog_store_key_keyring` checks. Their historical IDs are retained;
profile instrumentation remains ordinary versus sanitizer. Both exact CTest names
are already included by the unchanged source workflow filter at
`.github/workflows/groundhog-ci.yml:272–276`. Profiles, nodes, dependency edges,
commands, fixture properties, capabilities, exclusions and audit guards are
unchanged. The new test/export fixture are authored `selection.contract` inputs.
Review correction `59dad1a077d5e7fb4e1ecac10742888dae6e7655` makes the regression
reject unknown, empty and unknown-node classifications before checking ownership
coverage; explicit recognized global inputs remain allowed. The negative test
exposes the previous vacuous empty-subset assertion.

On the same fresh committed-source Groundhog graph, comparing the exact original
`fb7335cb0` inventory with the repaired inventory removes **five findings, zero
added**: two unclaimed registrations (`groundhog-store-key`,
`groundhog-store-key-keyring`), two missing `groundhog.store` source edges for
`gh-store-key.c`, and one missing `shared.tests` edge for `nostrc-test-bus.c`.
Groundhog remains failed: **406 unclaimed + 9,231 missing-impact + 1 owner
disagreement = 9,638 findings**. The Grotto audit is unchanged and failed:
**400 unclaimed + 8,971 missing-impact + 2 generated-runtime = 9,373 findings**.
The comparison is counterfactual inventory on the same fresh graph, not a hosted
rerun. Linux `x86_64` to `aarch64` overrides occur only in diagnostic copies.

A real nonroot Linux/arm64 selected-target build and direct CTest diagnostic under
private D-Bus/Xvfb passes both primaries and both required fixture setups
(`groundhog-test-gst-registry`, `groundhog-test-homes`), with unchanged CTest
properties. This is **not** backend `run_resolved`, a passing input audit,
selected-gate receipt, all-target Linux build, postbuild/header audit, authored
x86 admission, or hosted/release acceptance. Store-source edits retain their
pre-existing MDK consumer lanes; the repair does not claim those lanes disappear.
No Grotto or unrelated libgo memberships are added.

**Execution boundary needing a policy decision:** fresh ordinary Groundhog and
Grotto exports each contain 33 precisely owned libgo registrations, but all 33
fall outside their respective source workflow test filters. Presence alone does
not authorize enrollment or execution. They remain unclaimed; no ordinary-profile
nonmember policy was approved or implemented in this pass. Any exact policy
proposal requires separate owner review and must preserve not-executed status and
ineligibility for complete/release promotion. No such policy change is authorized
by this membership repair.

**Baseline and launch diagnostics retained:** unchanged native 445-case baseline
first reports 8 failures and 8 errors. Hash-only phase validation detects cached
parent `XPC_SERVICE_NAME` differing from inherited child value `0`; that is the
only environment-key difference, and the child environment reproduces the stored
phase hash. Cause JSON SHA-256:
`28a0e64fc1acec7e1bc39bd41979a855c8db381252e6d27dcb6c1f1eb0c3ae19`.
An explicit `/usr/bin/env XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1` launch
passes the unchanged 445 baseline with no parent/child environment differences.
The guard and environment hashing remain unchanged. A separate shell-assignment
launch exits 133 before any test output; the same failure reproduces with
`/usr/bin/true`, so it is a retained launcher failure, not a test-suite result;
its underlying launcher cause is not diagnosed. The tested command uses
`/usr/bin/env`, not that assignment form. Linux root setup also has one retained
custody-test failure because its deliberate `User=0` mutation is no mutation under
root; the intended nonroot `ci` operator baseline passes 445 cases.

At `61d7ecb2a`, the child records 452/452 controlled native and 452/452 nonroot
Linux cases passing; parent independently records 452/452 native, zero skips,
152.766 seconds. Those runs are **not relabeled** as full verification of the
later test-only review correction. On corrected source
`59dad1a077d5e7fb4e1ecac10742888dae6e7655`, parent independently runs
`/usr/bin/env XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v`:
**453/453 pass, zero skips, 150.113 seconds**. Native default-all build, six shell suites,
four statics and actionlint pass; build uses libsecret/secret-service safety flags,
parallelism 4 and default-off Groundhog. No native full-root CTest is claimed.

Local checkpoint `/tmp/nostrc-registration-membership-20261019/checkpoint.json`
(SHA-256 `28fe90c0fc2436fc370cd6cc306f351b500ae710dcc9e9c50ee0bcc5faa1128d`)
binds the corrected source and eight focused regressions. Its immutable earlier
checkpoint `checkpoint-61d7ecb2a.json` (SHA-256
`19359b878854ecaa1093615959bef9d84fde85ea007bce2548f0417728ba0f3c`)
retains the configure/audit/build/direct-execution evidence, original failures,
setup failures and source identities. Parent independently verifies evidence
hashes and the five-removed/zero-added set difference. No W11 counter advances
from these diagnostics; `.9`, `.2` and wider acceptance remain open.

## Approved ordinary libgo subset admission

Owner approval is implemented by code `48af17055f17d88f0392ab716cc93ae4644bd3d5`.
`diagnostic_nonmember_subsets` now has two exact records: the 33 reviewed libgo
registrations independently in `groundhog.linux` and `grotto.linux`. Their authority
is `reviewed_diagnostic_nonmember_subset_v1`; the original sanitizer full-partition
contract is unchanged. This is an implemented backend/adapter/aggregation path,
not a proposal or permission to omit the rest of either build tree.

Admission checks exact profile/workflow hashes, full registration universe,
primary/fixture closure, normalized candidate contracts, toolchain and reviewed
source controls. Fresh source/inventory/profile/phase/configuration/generation/
registration/descriptor/target/audit bindings remain independently checked.
Omitted real target closures are still built and fully source/Ninja/header/runtime
audited as required. Only matching unclaimed-registration findings change.
All omitted names remain `not_executed`, `complete=false`,
`complete_gate_eligible=false`; other failures still stop the lane. Grotto's
independent build-2/test-1 contract is enforced. Standalone build output retains
omission accounting; stripping either incomplete flag refuses before build.

The ordinary subset's standalone **`--phase test` is explicitly unsupported** and
refuses before CTest; use `--phase all` for physical prerequisite-build proof.
No new retained-build custody extension was fabricated. Non-subset and legacy
sanitizer phase behavior remains unchanged. Independent genuinely complete broad
validation keeps its existing release route, but incomplete ordinary receipts,
nested records, stripped flags and disguised profile evidence cannot be promoted
through that route.

### Actual source-bound x86 proof

A repository-Dockerfile image was built for actual Linux/amd64 execution through
Rosetta on the ARM64 host, not by changing an ARM64 profile label. `uname` and GNU
compiler target report `x86_64` / `x86_64-linux-gnu`; the 27 executables per profile
have ELF64 `e_machine=62`. The 33 registrations share **29 transitive build targets**
per profile, including their libraries. All 29 target-closure builds pass. This is
emulated x86 configure/build/audit evidence, not hosted/native-hardware performance
or outer arbitrary-tree custody acceptance.

Initial missing `iproute2`/Node prerequisites produced incomplete 421/459-name
exports; those attempts remain retained. Restoring those actual prerequisites
produced the reviewed universes of **422 Groundhog / 461 Grotto** names, consistent
with hosted artifacts. The older ARM64 Grotto witness had 459, not 461; it was not
silently substituted. Historical archives lacked complete realization properties,
transitive mappings and postbuild proof, so they were not used alone for admission.

Reviewed record/export provenance is source `32f756b734deb72e41cdb16b9a3089c2839ca642`;
independently fresh configure/export/build/preaudit/postaudit admission proof is
source `6cc11141326aa56b457fec7d6cf6b94b4b354047`. The final flag/phase corrections
are source `48af17055`; the earlier full profile proof is **not relabeled** as a
new execution on that SHA. All 192 reviewed graph-control source hashes per record
still match final code. Parent verification independently recomputes the exact set
differences below and confirms full input hashes/header entries are unchanged.

| Profile | Prebuild findings before → after | Postbuild findings before → after |
|---|---:|---:|
| `groundhog.linux` | 9,638 → 9,605 | 22,259 → 22,226 |
| `grotto.linux` | 9,375 → 9,342 | 21,704 → 21,671 |

Each comparison is on the same fresh graph and removes exactly the **33 named
unclaimed findings**, with **zero added findings**. Both full audits still fail.
The actual CLI records `lane_failure_v1` at preaudit with build/test unexecuted;
its diagnostic eligibility is false. The separate manual omitted-target builds
are not claimed as successful gated execution or complete profile coverage.

Immutable local handoff:
`/tmp/nostrc-ordinary-subset-fresh-x86-20261019/checkpoint-final.json`, SHA-256
`af62989d8e79f3637f4bc8f7c1718ce5892c55d672ea4cef366bbf2fe910d216`.
It retains original setup/build/audit failures, final source/file/receipt hashes,
image/Dockerfile/toolchain evidence and a hashed generation/build archive. Tracked
`tests/selection/fixtures/ordinary_subset_x86_exports.json` preserves normalized
reviewed exports; local archived builds are historical evidence, not current
attestations. Reuse always requires the normal fresh checks.

### Verification and limits

Parent independently runs **466/466 selection tests, zero skips**, on `48af17055`
with `/usr/bin/env XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1`. Native default-all
build, six shell suites, four statics and actionlint pass; libsecret/secret-service
safety flags are used, Groundhog remains default-off. Bounded review found two
integration defects (missing standalone-build flags and late test-only refusal),
both corrected with adversarial regressions. The earlier 463-case run's six
publication errors are retained; the fix preserves independent complete broad
validation while still rejecting incomplete ordinary evidence. No failed attempt
is counted as successful coverage.

The explicit diagnostic records are admitted, but **local/CI defaults, original
workflows, schedules, protections and release mode are unchanged**. No W11 credit,
complete/release eligibility, full-root CTest, general native/wrapped custody, or
migration completion is claimed. `.9`, `.2`, `.4` and `.5` retain their wider unmet
acceptance scope. The existing run ledger remains its dated snapshot; a new PR
requires separate source-linked comparison review before any counter can advance.

**Version assessment:** this audit, ledger, tracker reconciliation, internal CI
profile authoring and test-only changes require no shipped component bump.
