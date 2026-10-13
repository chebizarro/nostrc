# G07 approvals — focused security / UX-contract review

## Context / Scope

**Verdict: REQUEST CHANGES.** The core authorization boundaries are largely preserved, but pending-review reachability, notification naming, deadline recovery and stale-action behavior do not satisfy the specified contract.

Reviewed `master` **`eaf62ae4c436e649888817b5cce7aafc5321ca8a`** against `origin/grotto/g07-approval-controller` / worktree HEAD **`08ce0943c1064bb1f83465d55e6ff4f96f6e5dfc`**. Master is the merge base; the range contains exactly `f5813c02e` and `08ce0943c`. File:line references below describe that branch, not the main checkout.

Worktree: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-65c7f39d-grotto-g07-approval-controller-6b775ca2`.

Contract: `docs/plans/grotto-signer-ux-2026-10-12.md` §5.5 in full (`201–225`), §5.4 Trust tiers (`187–195`), §5.3 summary sanitisation (`155`), §5.1 quit semantics (`135–137`), OC-1 (`19`), and §9 controller/integration rows (`527–528`) plus real-dialog requirements (`544`). The checked-in §9 table has no separate `signer/approval-dialog` row; its prose and named suite supply that requirement.

Only the seven requested topics were examined. This is not a daemon/transport-wide security audit, G10 dashboard review or release-readiness claim. No implementation edits, issue-status changes, commits, pushes or merges were performed. This report is the sole worktree addition; documentation-only, **no component version bump**.

## Findings — severity ordered

### F1 — P1, CONFIRMED: deferred requests and notification-less requests have no production inbox

**Evidence:** `apps/grotto/src/approval-controller.c:850–859, 1018–1026`; `apps/grotto/src/main_app.c:271–292, 322–329, 386–393, 479–480`.

`Not now` correctly leaves the daemon request PENDING and marks it deferred, excluding it from subsequent notifications. However, a search of the complete production `apps/grotto/src` tree finds **no caller** of `grotto_approval_pending_group_new()`. The launcher presents management and reconciles; it does not present pending reviews. The only request-review action has an `s` parameter and is wired to notifications, not a visible inbox entry point. Thus a single deferred request loses its only ordinary review affordance until expiry. An externally invoked action can recover it, but that is not the promised Home inbox.

The same gap makes notification-service absence operationally significant: the controller emits `notifications-changed`, but main subscribes only to `message` and `reconciled`. The widget's unavailable-notifications description is never hosted. Even when hosted later, it listens only to `items-changed` (`1024`), so service availability changes alone will not refresh its description.

**Recommendation:** host the existing pending group in reachable foreground UI and surface notification unavailability, including service loss/reappearance without a request-model change. This requires approval reachability, **not** G10's remaining dashboard features. Test `Not now → reopen → same pending request`, and absent/lost notification service with a live request.

### F2 — P2, CONFIRMED: notification titles fall back to raw principal identifiers

**Evidence:** `apps/grotto/src/approval-controller.c:97–108, 158–161`; `nips/nip55l/src/glib/signer_approval_service.c:304–314`; `nips/nip55l/src/glib/signer_caller.c:75–84, 212–214, 437–453`.

The title selector correctly prefers the user label and rejects unverified claimed names. But for a verified request without an `app_id`, it returns the **principal**. Supported executable callers can therefore expose `exe:/home/<user>/<private-directory>/<binary>`; an unlabeled NIP-46 client exposes its full `nip46:<transport-public-key>` fingerprint. Neither is a client label or a verified application name. This exceeds §5.5's title allowlist and unnecessarily reveals process-path/client-key metadata in the notification service/history. It is **not** evidence that the signer identity or a private key is disclosed.

**Recommendation:** when no permitted label/verified application name exists, use `Unverified app`; retain principal/path/fingerprint details inside the review. Add verified-but-nameless executable and NIP-46 notification cases, alongside the existing unverified-name case.

### F3 — P2, CONFIRMED / reproduced: decision failure plus snapshot failure loses the known expiry timer

**Evidence:** `apps/grotto/src/approval-controller.c:309–322, 659–683, 723–744, 399–402`.

Applying the pre-decision metadata while SUBMITTING removes the request's expiry timer; the final decision call also removes it. If `ApproveRequest` fails, the request becomes UNAVAILABLE and recovery depends on `ListRequests` successfully reinstalling a timer. A failed recovery list only emits a message. It leaves the request, notification cleanup and application hold waiting for another signal/reconcile/owner change, **past an already-known authoritative deadline**.

**Local reproduction:** an out-of-tree harness compiled the actual controller source and reused the branch's private-bus fixture. It supplied a one-second request, failed `ApproveRequest`, then failed `ListRequests`. After the original deadline it asserted: request still present, state UNAVAILABLE, `expiry == 0`, pending count 1, outstanding request hold 1. No repository test or production code was altered. This proves a GUI lifecycle failure, not that the daemon signs after expiry; the real daemon's deadline checks remain intact.

**Recommendation:** preserve bounded cleanup at the original deadline independently of snapshot success. Never refresh that deadline during recovery or retry an uncertain decision automatically. Add this double-failure case to the committed controller suite.

### F4 — P2, CONFIRMED: stale notification actions can wait forever for reconciliation

**Evidence:** `apps/grotto/src/main_app.c:276–292`; `apps/grotto/src/approval-controller.c:422–424, 437–454, 905–930`; foreground routing at `main_app.c:256–268, 386–393`.

Notification Deny always queues an ID and waits for `reconciled`. With no daemon owner or an incompatible daemon, reconciliation returns immediately and never consumes the action or emits the required stale-request message. A failed list has the same missing terminal-action handling. A specific Review against an incompatible owner likewise stores `wanted_review` and waits on a reconciliation that cannot run. These cases do not deny or approve a substitute request, but fail the required bounded activation/message behavior.

When the normal stale message does occur, main presents the existing management window without explicitly navigating to Home; an already-open Settings/other page stays there.

**Recommendation:** terminate pending action handling on absent/incompatible owners and snapshot errors, with the stale/unavailable explanation and visible Home/management presentation. Preserve the exact requested ID; never use the next request as a fallback. Exercise the real `main_app.c` action wiring, not just controller APIs.

### F5 — P2, CONFIRMED: open-dialog stepper position/count become stale

**Evidence:** `apps/grotto/src/approval-controller.c:207–230, 861–879, 957–972`; `apps/grotto/src/ui/approval_dialog.c:431–438`.

Position, total and Previous/Next sensitivity are computed only when a review opens. Completion of another grouped request, or a new same-group arrival, changes the model without updating that presentation. Navigation itself recomputes live IDs. For example, a dialog displayed as `2 of 2` has Next disabled; a third same-group request arriving while it is open remains unreachable through Next. Removing another member leaves an inaccurate count and potentially enabled navigation with nowhere to go.

**Recommendation:** refresh navigation metadata on pending-model changes, maintaining the current request ID. Do **not** automatically add new arrivals to the approved selection: freezing the batch's checked-ID set is a useful unseen-operation boundary. Add arrival/completion-during-review cases, not just a static `set_position()` assertion.

### F6 — P3, PLAUSIBLE / conditional: AUTH relay paths are not proven token-free

**Evidence:** `nips/nip55l/src/glib/signer_approval_service.c:270–279`.

The producer rejects URL userinfo, query and fragment, but copies an accepted AUTH relay URL's complete path. A credential-bearing relay path would therefore enter `event_summary.auth_relay`. The source proves path preservation; **no supported deployment carrying a token in its relay path was demonstrated**. This is a residual sanitisation question, not a confirmed leak of a real pairing token.

**Recommendation:** add a credential-bearing-path negative case and establish fail-closed handling if that URL shape is supported. Do not silently truncate/change the exact AUTH relay or claim that URI parsing alone establishes the absolute “never tokens” guarantee.

## Contract checks that hold, and bounded limitations

### Information disclosure and notification decisions

- Body construction (`approval-controller.c:110–125, 158–166`) uses fixed operation text plus registry-derived human kind/numeric kind. It does not consume identity, content, reason, relay or token. Claimed-name text is not used for the title. F2 is specifically the extra raw-principal fallback.
- Lock observation defaults to **locked** (`1106`); absence/disappearance keeps generic title/body (`607–626`); lock changes republish (`585–604`). The single-request fallback contains no client/operation details. GNotification has no portable hide-content hint here, so the explicit generic fallback is the protection.
- **Minor lock-transition limitation:** multi-request updates share the two-second summary throttle (`169–177`); an existing count-bearing summary can remain until that timer runs after locking. This is source-confirmed delayed genericisation, not a demonstrated client/content disclosure on a locked desktop. Linux/Shell acceptance is still needed.
- Single-request notification actions are only Review and Deny once (`164–166`); summary has default Review plus Review only (`175–180`); **no notification Approve action** exists. Main submits notification Deny as `(allow=false, remember=false, ttl=0)` (`main_app.c:282–286`). Dialog Remember may also remember a denial, as §5.4 permits; do not conflate that with notification Deny once.
- Summary coalescing uses a notification-rate timer, **not** a debounce used to discover/authorize batches (`169–182`).
- `event_summary` uses valid UTF-8, bounded public text and control-byte replacement (`signer_approval_service.c:217–226`). Event bodies are emitted only for public note/article/comment and reaction value; known DM/seal/gift-wrap/file-message/NIP-46 bodies are suppressed, as are custom-kind bodies and arbitrary tag values (`228–296, 326–336`). Target/recipient identifiers are validated 64-hex public IDs. AUTH challenge tags are never copied. Encrypt/decrypt/conversation-key summaries expose operation/peer metadata, not input/output bodies or derived keys (`332–335, 527–540`). Tests cover the six private kinds and unsafe AUTH userinfo/query URLs. F6 qualifies the stronger token-free assertion.

### Lifecycle and exact hold/release accounting

- Subscriptions are installed before owner watching triggers status/list calls (`approval-controller.c:1118–1136, 518–531`). Catch-up uses authenticated Control1 `ListRequests`, not legacy `ListPendingRequests`; decisions still use the preserved Signer API.
- IDs deduplicate inside one owner generation (`333–345`). Completion creates a tombstone even before insertion (`207–211`). Reset increments generation, cancels owner work, invalidates/removes every request, clears owner-scoped maps/tombstones (`493–510`). Metadata callbacks require matching generation and request object (`248–252`). Completion-before-info/snapshot and owner loss/reappearance are tested.
- `resolve()` is the single terminal request cleanup (`207–230`): cancel metadata; remove expiry; detach decision callbacks and force-close the matching review; withdraw its notification; clear `held` before release; remove the model/table entry. Duplicate completion finds no request and cannot release it again. Closure disconnects callbacks before forcing the dialog closed (`197–205`), avoiding accidental Deny on daemon completion.
- There is **no periodic request polling**. Reconcile is driven by owner changes, invalidation signals, activation/review and pre-decision authoritative metadata. Owner/request cancellables are passed to metadata/list/status/decision calls (`369–371, 433–435, 527–529, 741–744, 766–768`). Caller-task cancellation is checked before decision dispatch (`693–700`); after dispatch, cancellation is not a promise to undo daemon execution.
- Deadlines are anchored to fetch start plus `remaining_seconds`, with an absolute `expires_at` fallback, and clamped to the existing deadline (`291–305`). **No catch-up request is given a fresh 300 seconds.** However, the specified existing-TTL/liveness compatibility path is not implemented: missing both fields makes the request UNAVAILABLE with no timer (`300–322`). That is fail-closed for decisions, not an approval bypass, but bounded unknown-age cleanup/compatibility tests remain absent. F3 separately demonstrates loss of a deadline that was known.

Every actual application hold is recorded by `hold()`; `release()` only performs a real release when the recorded count is positive (`74–87`). On the owning main context, each such decrement consumes one acquisition, and no path can decrement below zero:

| Acquisition | Exactly-once normal release | Shutdown/late-callback handling |
|---|---|---|
| Bootstrap (`1118–1119`) | `release_bootstrap()` clears its flag before release (`512–516`), called on appeared/vanished | Stop also uses the same flag (`1065`) |
| One per newly inserted request (`341–342`) | `resolve()` clears `r->held` before release (`222–225`) | Reset resolves all requests; duplicate completion has no request |
| List snapshot (`430–432`) | `fetch_free()` for the one call context (`253–258, 420`) | Cancel on reset/stop; stop drains remaining recorded holds |
| Owner-status fetch (`524–526`) | `status_done → fetch_free` (`456`) | Same cancellation/drain behavior |
| Trust mutation (`897–899`) | `tier_done → fetch_free` (`889`) | Same cancellation/drain behavior |

Stop is idempotent, unsubscribes/unwatches, removes the summary timer, resets requests, cancels work and drains residual holds (`1037–1067`). Fetch flags are not cleared by that aggregate drain, but late `fetch_free()` calls encounter a zero count and cannot perform a second application release; no new normal work is admitted after stop.

**Dynamic proof:** the temporary harness wrapped the actual `g_application_hold/release` calls, ran all nine macOS-enabled controller cases plus the deadline fault and stop-with-list-in-flight/late-reply case: **11/11 passed, 55 real holds and 55 real releases**, with an assertion rejecting release counts exceeding acquisitions. This proves accounting for those exercised paths; F3 is a liveness/deadline failure, not a double-release defect.

Ctrl+Q maps to application quit (`main_app.c:489–492, 652–653`). Shutdown stops the controller and withdraws notifications without sending decisions/revoking grants/stopping the daemon (`294–303`, controller reset/resolve). Pending requests therefore retain daemon TTL semantics. Relaunch catch-up works through the new controller; stale-action exceptional paths are F4. Ordinary dialog close-denies is explicitly expected by §9 prose (`544`) and is not a finding.

### Review dialog and batch authorization

- The live review gets `identity` directly from daemon metadata, with no account dropdown/first-account lookup (`approval_dialog.c:345–375, 473–475`); controller decisions ignore the UI identity argument (`approval-controller.c:809–819`) and recheck pinned scope (`268–277, 714–735`).
- Deny gets initial focus; no default Approve widget is installed; Approve rejects default activation (`approval_dialog.c:325–335`; `.blp:64–66`). Only explicit button or documented Ctrl+A invokes approval. Both `do_finish` and `finish_now` enforce readiness/sensitivity (`85–111`); selected-once does too (`166–170`). Actual Enter/key-event behavior still lacks committed test coverage.
- Not now clears the decision callback, emits deferred, and closes without calling `ApproveRequest` (`146–154`), retaining PENDING. Its local semantics pass inspection; its user-visible recovery is F1.
- Remember sends legacy `ApproveRequest(sbbt)` (`approval-controller.c:741–744`); the daemon remembers the pinned operation and **exact event kind** (`signer_approval_service.c:745–747`). Trust changes separately call Control1 `SetClientTrust(ss)` (`approval-controller.c:891–903`), with their own confirmation and explicit separation copy (`approval_dialog.c:212–234`). No request decision silently raises trust. The existing Forever dialog supplies a sentence naming scope and duration, with Cancel default (`109–133`); the spec does **not** require typing that sentence. Forever cancel/confirm/stale-response tests are missing.
- Groups match all four fields: principal, daemon identity, transport and flow ID (`approval-controller.c:776–783`). Missing flow IDs permit presentation grouping only; selected approval requires an explicit nonempty flow (`839–842`). Batch rows enumerate operation, kind and peer, start unchecked, and submit only the checked, displayed IDs (`approval_dialog.c:393–420, 166–182`). Controller rechecks each selected ID's READY state and group, then submits individual `allow=true, remember=false, ttl=0` decisions (`844–847`). Future arrivals are not swept into the selection; no remembered permission/batch wildcard is created. Dynamic cross-scope/unseen-arrival tests are still missing. F5 concerns navigation truth, not widening batch authority.

### GTK/libadwaita floor and Blueprint parity

No 1.6+ widget/API was found in the changed controller/dialog. The template uses `AdwDialog`, `AdwToolbarView`, `AdwHeaderBar` and established GTK widgets. The alert dialogs/async choose, focus, force-close and follows-content-size APIs are available at 1.5: [AdwAlertDialog documentation](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1.5/class.AlertDialog.html), [AdwDialog documentation](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1.5/class.Dialog.html), [follows-content-size](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1.5/property.Dialog.follows-content-size.html).

A local syntax check of both production files passed with `ADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5`, `GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14`, matching minimum macros, and unavailable/deprecated API diagnostics treated as errors. This uses newer installed headers, **not** an actual floor-runtime build.

Compiling `approval-dialog.blp` reproduces the committed `.ui` **except for the compiler's final newline**. Object/property/template content is in sync; this is not a semantic Blueprint drift finding. No source/generated file was rewritten.

## Coverage gaps vs §9

| Suite | Existing relevant proof | Remaining focused gaps |
|---|---|---|
| `signer/approval-controller` | Real controller + private-bus I/O fixture; dedup/holds, completion-before-list/info, expiry/tombstones, owner loss/reappearance, fresh decision metadata, changed identity, missing required preview (`test-approval-controller.c:218–390, 497–514`) | Committed F3 double failure; status/list/info errors; missing deadline/compatibility cleanup; external cancellation and stop with in-flight callbacks; different unique owner plus late decision replies; stale real app actions. `fail_decision` exists (`18, 115–117`) but no registered test sets it. Linux notification test covers claimed-name suppression, single/summary actions and generic lock fallback, but not verified nameless callers, real Deny delivery/persistence, availability changes or withdrawal acknowledgments (`430–494`). |
| `signer/approval-integration` | Real daemon + private bus + real controller; signature result, remembered same-kind call, another kind prompting, sanitized per-kind summaries, deny. Linux-display path clicks the actual dialog and changes trust separately (`test-approval-integration.c:110–176, 179–321`) | Does not run actual `main_app.c` startup/actions or host the pending widget; no no-notifications/Not now/reopen flow, selected-batch authorization, dynamic stepper, Forever or Ctrl+Q/relaunch/stale-notification flow. A different **required-confirmation** kind prompting is weaker than an assertion that the persisted grant is exact-kind; inspect/assert stored scopes or another non-required kind too. |
| `signer/approval-dialog` | Real template/dialog; one callback, disabled Approve handler, pinned identity/Remember duration, Unicode, completion detachment, sanitized previews, initial focus/default-widget absence, explicit unchecked batch selection (`test-approval-dialog.c:134–293, 311–324`) | No injected Enter on unrelated controls/shortcut-sensitivity test, Forever confirmation/cancel/completion race, trust-confirmation cancellation, or live-model stepper updates. The batch test is dialog-only, not controller/daemon proof of transport/identity/flow separation or unseen future operations. |

### Verification performed

- `git diff --check master...origin/grotto/g07-approval-controller`: passed.
- Existing branch `_build` points to this exact worktree; reviewed source timestamps precede its relevant built objects/tests. Focused CTest selection for `nip55l_approval_service` and the three approval suites: **6/6 CTest entries passed**, including two isolation fixtures. No fresh full build was claimed.
- **Important:** macOS `signer/approval-dialog` output is `1..0 # SKIP UI disabled`; its “pass” runs no dialog assertions. Integration reports `headless controller (platform UI skip)`; controller's notification case is compiled out on Apple (`test-approval-controller.c:391–496, 511–514`). These are not Linux GTK/Shell/notification acceptance.
- Out-of-tree source-level fault/accounting harness: **11/11**, with F3 reproduced and **55/55** raw hold/release calls. Temporary evidence: `/var/folders/mt/rcd9cp6j61sbdjzt0r576yv40000gn/T/grotto-g07-review-9mmu9tr6/{review.c,run.log}`; not committed and not a replacement for missing regression tests.
- API-floor syntax guard and Blueprint parity checks passed as qualified above. No developer keychain configuration was changed; the existing build is configured for libsecret/Secret Service.
- Relevance-only `find-lines` queries narrowed plan/producer/caller reads (15 requests, 128,877 input tokens, approximately USD 0.0054). Their line suggestions were checked directly; no helper judgment determined a security finding.
- The optional Oracle second opinion received stale baseline context rather than the frozen branch patches and was excluded as branch evidence. Findings above rely on direct branch source and the stated local checks.

## Recommendations

Fix F1–F5 before treating G07's requested approval contract as complete; add the narrow failure/reachability/privacy/navigation regressions identified above. Keep the demonstrated no-notification-Approve, explicit-flow selected-ID boundary, pinned identity, exact-kind Remember and separate trust mutation. Resolve F6 only against supported relay-URL semantics; do not invent a credential-redaction contract or widen this review into transport policy.

Run the real dialog and notification/action integration on Linux at GTK 4.14/libadwaita 1.5. The local headless successes and source/API checks cannot close that acceptance gap.

**REQUEST CHANGES**
