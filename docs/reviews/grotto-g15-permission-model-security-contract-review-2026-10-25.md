# G15 permission model — focused security/contract review

## Context / Scope

- Comparison: `master` **9e184be8666a06e7d867570a37a53a90c31f1817** → `origin/grotto/g15-permission-model` **cc472a42f94ee80e09aa45f170d285e210d243d2**.
- The supplied worktree's HEAD equals the reviewed branch. A read-only `git ls-remote` also confirmed that the pushed branch points to that SHA.
- Authority: `docs/investigations/grotto-approval-persistence-and-gates-2026-10-25.md`, RC1–RC4, Investigator Findings §5/Constraints, and owner-amended Recommendations 1–12, especially lines 487–500. Review is limited to the ten requested subjects.
- Interpret the owner amendment literally: ask-by-default limits **tier auto-allow**, not explicit allows; Full is a versioned routine preset, not authority for custom kinds. A first scoped grant can still be needed for an ask-by-default kind. Do not repair a UI mismatch by widening Full or restoring an always-confirm class.
- No product/test/configuration/tracker edits, hook installation, commits, fetches, or pushes. This report is the only worktree addition. Scratch probes/builds are outside the repository. No broader relay audit or release qualification is claimed.

**Disposition:** four confirmed **Medium / P2** contract/honesty findings below. No Critical/High permission bypass was confirmed in the reviewed paths. This is not a clean acceptance verdict: literal precedence and the advertised reusable-choice proof still have gaps.

## Findings — severity ranked

### F1 — Medium / P2, confirmed by execution: failed trust-and-approve hides the saved Full preset

**Locations:** `nips/nip55l/src/glib/signer_approval_service.c:1259–1275,438–440`; `apps/grotto/daemon/grotto_clients.c:161–175`; `apps/grotto/src/approval-controller.c:699–706,841–849`.

`trust-routine` durably commits Full/preset-version 1 before backend execution. A backend failure leaves that authority in place, but `finish()` reports only a boolean failure to the decision caller. The controller turns that into “The signer could not complete the decision”; the actual dialog feedback says to review the pending inbox before retrying. Neither response explains that the preset **was saved**, and the failed request has already left the inbox. Retaining a successfully committed preset need not be wrong; hiding that partial outcome violates the requested failure semantics.

**Executed reproduction:** a scratch wrapper around the branch's real-daemon/controller fixture submitted invalid NIP-44 ciphertext, then invoked Control1 `DecideRequest(..., "trust-routine")`. Decryption failed; the decision returned `(false)` without error detail; `ListClients` still reported Full/preset-version 1. Log: `/tmp/nostrc-g15-review-probes/failure.log`. No fake backend or production source modification was used. The generic UI wording is source-confirmed, not a GUI click-through reproduction.

**Recommendation:** represent/report “access preset saved; this request failed” distinctly. Do not imply rollback or invite a blind retry. Add a production-path failure test for the response and displayed feedback.

### F2 — Medium / P2, confirmed by execution: the evaluator does not implement the specified defaults/flow precedence

**Locations:** `nips/nip55l/src/glib/signer_permissions.c:495–505,589–614`; production caller `nips/nip55l/src/glib/signer_approval_service.c:818–821,468–492`.

Two differences from Recommendation 1 remain:

1. The ask-by-default return at line 498 precedes **first-party defaults** at line 499. It therefore limits more than tier auto-allow. With an identified principal, no rows, `first_party_default=TRUE`, and kind 5, the evaluator returns ASK, not the specified default ALLOW. The production caller really supplies this flag from the executable/device/inode-checked built-in-grant path; a configured `event` default is affected. Whether a deployed build configures that default was not established.
2. `evaluate_flow()` calls the complete evaluator, including first-party/tier fallback, **before** examining bounded flow consent. A preset/default ALLOW bypasses flow accounting. A scratch size-1 encryption-flow probe admitted one unique request through the first-party default and then admitted another unique request through the still-unspent flow after that default was removed. This is not a demonstrated unauthorized signature—the first request had independent authority—but it is not the requested flow-before-default ordering/accounting.

Both observations ran against the branch's permission implementation using its test target's compile/link inputs: `/tmp/nostrc-g15-review-probes/policy-probe.log`.

**Recommendation:** separate explicit-row/admission checks from preset fallback, then apply bounded flow before first-party/tier fallback; confine ask-by-default to tier selection. Preserve deny/error precedence. Add tests with **no remembered allow** so an allow row cannot mask the ordering defect (the current first-party assertion at `nips/nip55l/tests/test_signer_permissions.c:373–377` already has such a row).

### F3 — Medium / P2, confirmed: a reusable trust choice is offered where it cannot suppress the matching request

**Locations:** `apps/grotto/src/ui/approval_dialog.c:477–486,548–554`; `nips/nip55l/src/glib/signer_approval_service.c:1271–1275`; `apps/grotto/tests/test-permission-repeat.c:141–157`.

The dialog exposes/enables “Trust this app for routine operations” for every rememberable request, including custom kinds. On a custom request the server saves Full and approves **once**, without saving the custom-kind grant. The real-daemon repeat test explicitly asserts that a second request of the same custom kind prompts again. This correctly preserves Full's custom-kind exclusion, but directly fails the requested rule that every **offered reusable choice** suppress the next matching request. The branch test encodes the exception rather than proving that rule.

**Recommendation:** do not broaden Full. Do not offer this as a reusable remedy for an uncovered current request; either restrict it to covered requests or explicitly distinguish the one-time current approval from the unrelated routine preset. Add the real widget → controller → daemon → next-request test. The precise-scope “Always allow” choice remains the appropriate reusable option for a custom kind.

### F4 — Medium / P2, confirmed in source: the flow's displayed remaining time uses a different anchor

**Locations:** `nips/nip55l/src/glib/signer_approval_service.c:365–367`; `nips/nip55l/src/glib/signer_permissions.c:562–576,718–735`; `apps/grotto/src/ui/approval_dialog.c:555–557`.

Actual consent is bounded by the shared `FlowWindow.first_seen/deadline`, retained for the principal/identity/transport/flow ID. But `flow_remaining_seconds` is calculated from **this request's** `received_at + 600`, and GTK describes that as the remaining time “from first arrival.” A later request in an already observed flow can therefore advertise almost 600 seconds when the real window has little or no time left. A shorter prior consent deadline is also ignored. The enforcement window is not extended; this is an honesty defect and can make the offered flow decision fail despite the displayed time.

**Recommendation:** report remaining time from the same daemon-owned window/deadline used to authorize consent. Add a staggered-request snapshot/UI assertion. Existing first-seen tests validate enforcement, not this summary field.

## Requested contracts — what is supported, and what is not

### 1. Evaluator correctness

- **Confirmed:** explicit denies are scanned before allow/default/tier decisions (`signer_permissions.c:472–495`); app block also denies before allow. The normal/flow path cannot turn DENY or ERROR into ALLOW (`:597–600`). No new always-confirm override of remembered allows remains.
- **Confirmed:** null/empty/`claimed:` principals and initial `connect` return ASK before any allow/default/tier (`:492–495`). New reusable connect allows are refused (`:446–448,755–760`). The service's existing-pairing exception is separate and applies only to reconnect (`signer_approval_service.c:822–825`), not initial pairing.
- **Cited test for the requested kinds:** rewritten `/trust-tiers/live-policy`, `nips/nip55l/tests/test_signer_permissions.c:354–366`, iterates **every** default ask-by-default kind: remembered allow → ALLOW, deny → DENY. It includes **5, 9734, 13, 1059, 0, 3, 10002**; **22242** uses a granted canonical relay and rejects the other relay. The real-daemon `signer/permission-repeat` matrix (`apps/grotto/tests/test-permission-repeat.c:207–229`) repeats all these kinds plus ordinary/custom examples, checks actual signatures, changes connections, and repeats after daemon restart. Both tests passed here. This is representative/all-listed-kind coverage, not exhaustive execution of all 65,536 kinds.
- **Confirmed:** current Full rejects unknown/custom kinds through the routine allowlist (`signer_permissions.c:440–448,501–505`); tests at `test_signer_permissions.c:342–366` and `test-permission-repeat.c:141–157`. F2 is the remaining literal-precedence mismatch; F3 concerns offered choices, not an unknown-kind bypass.

### 2. Migration safety

- **Confirmed:** v2 dormant default/custom-configured ask-kind allows migrate to **`policy-review`**, retaining absolute expiry, including zero, and provenance (`signer_permissions.c:409–425`). The 90-day deletion applies only to decision **`review`**, not `policy-review` (`:420–421`). The migrated file must save successfully before it replaces live records (`:425–429`). `policy-review` alone is not an allow (`:485–496`); independent explicit authority is a separate decision, not activation of that row.
- **Passing test:** `test_policy_review_migration`, `nips/nip55l/tests/test_signer_permissions.c:516–568`, proves inactive expiry-0 rows, preserved deny, reopened store, survival after a synthetic **400 days**, explicit activation, and relay-scoped activation of old kind-only AUTH. This is a real file/reopened-policy test, **not** a daemon-restart migration/UI test.
- **Preset gate proved:** missing `preset-version` is durably written as **0** (`apps/grotto/daemon/grotto_clients.c:72–82,119–125`); old Full maps to **`SIGNER_TRUST_PREVIOUS_FULL`** only when its version is 0 (`:145–147`). Its old ceiling remains behind the ask-by-default check (`signer_permissions.c:498–501`); accepted upgrade sets version **1** (`grotto_clients.c:204–215`). Passing `test_preset_migration` (`apps/grotto/tests/test-clients.c:275–303`) proves old Full permits custom 65535 but asks for profile metadata, shows “Previous access policy,” then excludes 65535 after explicit upgrade. GTK provides an explicit upgrade confirmation (`apps/grotto/src/ui/page-client-detail.c:380–395`).
- **Confirmed:** old conversation-key rows are deleted even before row parsing (`signer_permissions.c:403–406`), including legacy groups (`:334–335`). Legacy import remains **one-time**: an existing v2/v3 file is authoritative regardless of pre-marker state; later legacy input is sanitation-only (`:370–399`). Existing migration/no-reimport tests ran as part of `signer/permissions`.

### 3. Removal completeness and nostr-seal

- Exact searches of XML, service dispatch/context dispatch, gate enum, C ops/header, Grotto UI/controller/history/grants, and nostr-seal found no remaining callable `NIP44DeriveConversationKey(+ForApp)` or active `nip44_conversation_key` scope. The remaining literals are **purge/rejection code, negative tests, historical investigation/review material, and explicit removal/release notes**—not residual grant authority. Literal absence everywhere would be incompatible with testing/purging old rows.
- Both D-Bus names return UnknownMethod in `nips/nip55l/tests/test_signer_dbus_contract.c:965–973`; `nip55l_dbus_contract` passed here. The real-daemon repeat suite independently checks the original name (`test-permission-repeat.c:255–257`).
- nostr-seal now calls existing **`NIP44DecryptB64ForApp`** under ordinary `nip44_decrypt`, because the stanza plaintext is a binary 32-byte file key (`gnome/nostr-seal/src/nseal-signer.c:80–104`). This is the binary-safe NIP44Decrypt lane, not conversation-key export. **`nostr_seal_signer` passed here**: CLI seals and opens a file, compares all 30 binary bytes, and checks denial/nonrecipient behavior (`gnome/nostr-seal/tests/test-nseal-signer.c:108–153`). File-format version is unchanged.

### 4. AUTH

- **Confirmed:** one `auth_relay()` parser is used at submission, policy recheck and summary (`signer_approval_service.c:241–274,311–312,812–816,1139–1147`). It requires kind 22242, empty content, timestamp within ±600 seconds, exactly one two-field relay tag, and exactly one nonempty two-field challenge tag. Canonical relay, not challenge, is the reusable scope.
- `signer_permissions_canonical_relay()` preserves **ws versus wss**, nondefault port and encoded path, lowercases scheme/host, normalizes omitted root/default port, and refuses credentials/query/fragment (`signer_permissions.c:90–104`). Canonicalization tests and real-daemon other-relay/malformed/stale tests passed (`test_signer_permissions.c:570–577`; `test-permission-repeat.c:231–253`). Cross-scheme/nondefault-port/path **grant isolation** is not all exercised end to end.
- **No provenance overclaim:** the dialog explicitly says the relay is claimed by the app and the connection cannot be verified (`approval_dialog.c:582–585`). Each request supplies its own event/challenge; no saved challenge or cached AUTH response is used to authorize a later challenge. **Do not read this as duplicate-challenge rejection:** no one-use challenge registry exists, and the GUI repeat fixture re-submits the same challenge (`test-approval-integration.c:576–578,631–633`). A literal one-use interpretation remains an unproved contract, not a confirmed security flaw; [NIP-42](https://github.com/nostr-protocol/nips/blob/master/42.md#protocol-flow) allows a challenge to remain valid for a connection. Relay challenge/connection validation is not proved by this signer-only review.

### 5. Atomic trust-and-approve

- **Confirmed:** the production controller sends one Control1 `DecideRequest` mode (`approval-controller.c:769–777`), not `SetClientTrust` plus a second approval RPC. The service checks request/deadline/pause/rememberability and denying policy before synchronous preset mutation, owns the request during policy notifications, then executes through the normal policy check (`signer_approval_service.c:1233–1237,1256–1278,858–881`). This is a single server decision, not a transaction capable of rolling back external cryptography.
- Control1 checks a freshly identified **approver** before dispatching it (`grotto_control.c:658–676`); owning the GUI name is not authority. macOS keeps its documented UID-only development limitation; no Linux-strength executable attestation is claimed there.
- `test-permission-repeat.c:116–140` proves one trust decision completes its request, retires an eligible second pending prompt, and enables subsequent routine publishing. Linux-only GUI source also exercises the real trust button and matching zap repeat (`test-approval-integration.c:671–692`). F1 is the confirmed missing honest partial-failure result; F3 is the custom-request exception.

### 6. Dialog honesty

- Compact review removes TTL/trust dropdowns and Forever confirmation; Allow once is preselected, the reusable choice defaults to until revoked, and 24 hours is the alternative (`approval_dialog.c:99–116,475–495`; `apps/grotto/data/ui/approval-dialog.blp:64–89`). Duration editing remains in Permissions, not compact review.
- Deny once now ignores the reusable-allow checkbox, making its tooltip true (`approval_dialog.c:113–116`; native assertion `test-approval-dialog.c:211–218`). Pairing offers **Pair application**, hides reusable connect/trust, and is tested (`approval_dialog.c:548–550`; `test-approval-dialog.c:477`). Named macOS claims are shown as unverified (`approval_dialog.c:466–469`); the real-daemon macOS branch verifies `verified=FALSE` (`test-permission-repeat.c:78–80`).
- Daemon row `state`/`reason` comes from actual policy, including inactive review/blocked allows and expiry (`signer_permissions.c:647–659`); GTK renders those fields directly (`page-client-detail.c:447–455`). This establishes the explicit-row effectiveness markers, not that every hardcoded preset-description row in GTK is an independent daemon verdict.
- **Not every reusable control has the demanded UI→daemon repeat proof.** Unit widget tests assert emitted modes/TTL; Linux integration adds real repeat proof for scoped allow and routine trust, but does not cover all reusable block/activation/flow actions. See F3 and coverage limits below.

### 7. Flow consent

- **Confirmed:** both encryption and seal consent are in-memory `Flow` entries only; `consent_room()` performs no row commit (`signer_permissions.c:710–737`). The flow key binds principal, pinned identity, transport, operation/kind and flow ID; size is capped, shared deadline is anchored at first observed arrival, and request IDs spend at most one slot each (`:509–521,562–614`). Scope/budget/restart/no-row and synthetic first-arrival expiry tests passed (`test_signer_permissions.c:395–436,475–513`); encryption+seal no-durable-grant coverage is in `:440–471`.
- F2 identifies fallback-before-flow accounting; F4 identifies the displayed anchor mismatch. Independent authority that already allows an operation bypasses the prompt; an ungranted ask-by-default seal can still prompt even for Full, consistently with the first-scoped-grant amendment. No blanket claim that all Full clients never see a flow review is justified.

### 8. Pending re-evaluation

- **Confirmed:** policy changes reevaluate resolved/announced pending requests and execute newly allowed ones (`signer_approval_service.c:1168–1189`). Denies atomically cancel the affected set before callbacks; queued automatic work also has revision fences and owner-thread rechecks (`:1002–1038,949–978`). Pause/generation checks remain before execution/completion.
- Passing real-daemon trust retirement: `test-permission-repeat.c:123–140`. Passing service tests cover deny, revoke, tier lowering, expiry and pause around queued/backend work (`nips/nip55l/tests/test_signer_approval_service.c:175–286,343–494`). The changed revoke test specifically demonstrates automatic grant retirement and cancellation before queued signing (`:270–284`).
- `signer_permissions_replace()` invalidates all matching principal/identity flow consent before commit (`signer_permissions.c:841–844`), so overlapping ephemeral consent does not survive a policy edit. Revocation retires ephemeral authority even if its later durable write fails (`:739–749`). This is an explicit conservative boundary, not selective per-row preservation.

### 9. Version / manifest and rewritten policy test

- `VERSION_MANIFEST.md:84–85` records the MINOR-class breaking nip55l removal folded into untagged **0.7.0**, and Grotto/nostr-seal changes folded into their unreleased **0.1.0** previews; it includes the breaking-removal/migration call-out. Version sources/release columns are not falsely updated as published releases. Header remains 0.7.0; no local or configured-origin `nip55l-v*` tags were found. This checks the untagged premise, **not** a package-registry proof of the stronger “never published” wording.
- `/trust-tiers/live-policy` was actually rewritten to honor remembered sensitive-kind allows and per-relay AUTH, exclude custom kinds from current Full, reject reusable connect/removed export, and preserve denies (`test_signer_permissions.c:342–380`). It passed, rather than merely existing in a plan.

### 10. Coverage gaps within this review's boundary

- Migration restart proof reopens the real permission store and synthetic clock; it does **not** start a daemon on a literal pre-upgrade profile and click Activate/Discard across restart. The daemon repeat matrix starts with new grants, so it cannot substitute for that migration proof. Custom configured ask-kind migration and old-Full preset persistence deserve representative restart tests, not an exhaustive migration programme.
- Native 24-case widget tests use fabricated request dictionaries and callback capture. On macOS, `test-approval-integration` intentionally runs the controller/daemon without the Linux GUI worker (`test-approval-integration.c:761–783`). Thus this run does not prove every reusable widget suppresses a real next prompt. Block-operation, block-app, Activate/Discard and flow need the focused end-to-end honesty assertions; unknown/custom trust currently has the opposite assertion (F3).
- AUTH has no end-to-end isolation matrix for scheme, nondefault port and distinct paths, no explicit summary/policy equality test for those variants, and no duplicate-challenge-refusal proof. The latter must not be invented as transport provenance or silently assumed to be a one-use relay contract.
- Identity drift explanation and daemon effectiveness rendering are source-wired; native fixtures exercise drift copy, but no actual two-identity default A→B review or real-daemon active/inactive/expired GTK marker test ran here. The prior investigation's A→B proof is not relabelled as fresh branch execution.
- F1's partial-failure state is now reproduced, but the branch has no regression test for its honest presentation. F4 lacks a staggered-flow summary test. No hosted CI, Linux GUI execution, installed-client/relay session or release acceptance ran in this review.

## Verification actually run

Fresh build root: `/tmp/nostrc-g15-review-20261025`; source root is the specified worktree at `cc472a42`. Debug, `BUILD_TESTING=ON`, `ENABLE_NIP55L=ON`, `NIP55L_TEST_TRUST_ENV=ON`, **`NIP55L_SECRET_BACKEND=libsecret`**, `ENABLE_NOSTR_SEAL=ON`; no developer-keychain mutation. Disposable keys/profiles and repository private-bus helpers were used.

1. Configure succeeded. Initial target command mistakenly named nonexistent `nostr-seal-cli`: Ninja refused before compilation (`/tmp/nostrc-g15-review-build.log`). Correct target is `nostr-seal`.
2. Corrected no-share build failed in the **unchanged** `gnome/nostr-seal/src/nseal-publish.c:272–278` stub on unused parameters under `-Werror` (`/tmp/nostrc-g15-review-build-targets.log`). No source or warning guard was changed. Grotto-only targets built separately. This no-share configuration remains failed/unverified; it is not made an extra G15 repair prerequisite.
3. Explicitly configuring the supported `ENABLE_NOSTR_SHARE=ON` variant and building only the focused targets succeeded (`/tmp/nostrc-g15-review-configure-share.log`, `/tmp/nostrc-g15-review-build-share.log`). Its prerequisites were built, not selected as unrelated test suites.
4. `ctest --test-dir /tmp/nostrc-g15-review-20261025 -R '^(signer/(permissions|trust-tiers|permission-repeat|clients|approval-integration)|nip55l_approval_service|nostr_seal_signer)$' --output-on-failure`: **9/9 entries passed**, including two test-home fixtures, **7 primary suites**, no skipped CTest entries (`/tmp/nostrc-g15-review-ctest.log`).
5. Separate `ctest ... -R '^nip55l_dbus_contract$' --output-on-failure`: **2/2 entries passed**, including its home fixture (`/tmp/nostrc-g15-review-dbus-contract.log`).
6. Native `test-approval-dialog`, `GNOSTR_SKIP_UI_TESTS` unset, animations disabled by the test, scratch private-bus wrapper: **24/24 cases passed**, exit 0 (`/tmp/nostrc-g15-review-probes/native-dialog.log`). This is not the macOS CTest GUI-skip route and is not GUI→daemon proof.
7. Scratch real-daemon trust/backend-failure probe and direct-policy precedence probes: exit 0 with the asserted counterexamples above (`/tmp/nostrc-g15-review-probes/`). Probes derive compile/link inputs from the fresh branch targets; they do not patch product/test source.
8. `git diff --check master origin/grotto/g15-permission-model`: passed. Read-only local/remote ref checks confirmed the comparison SHA; no nip55l release tag was found.

Earlier `/tmp/grotto-g15-*` logs were inspected only to locate build/evidence routes. Their build directory had been removed; none of their results is counted as this review's execution. Jev relevance filtering was advisory for selecting source ranges, never proof of a contract; two multi-command relevance invocations ended without second-command output and were run separately. Reported Jev usage: 34 requests / 260,398 input tokens / approximately USD 0.011 (unreported invocations excluded). The removed-export search was reduced from 23 hits to four candidate locations; exact production-surface searches were still checked directly. All conclusions above rest on cited source or the fresh executions.

## Recommendations / landing boundary

Address F1–F4 with the focused tests named there; preserve unknown-kind exclusion, explicit deny precedence, inactive migration, private-service isolation and sanitizer/warning guards. Complete only the missing contract-specific UI/migration assertions needed for the requested outcome. Do not widen this into a repository audit, baseline repair, relay-provenance claim or release qualification. No fixes or tracker mutations were made by this review.
