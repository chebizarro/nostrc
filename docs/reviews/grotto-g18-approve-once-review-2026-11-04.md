# G18 approve-once — security and consent review

## Scope and evidence

- Reviewed `master` **da2e4d62bb41537ad750e24892b415d603cb05e5** → branch HEAD **ac3f273d95d178e56c9bfe24a37d43480d11a1fc** (23 changed files). `git ls-remote origin refs/heads/grotto/g18-approve-once` returned that exact HEAD. The local `origin/grotto/g18-approve-once` tracking ref was absent; no fetch or branch substitution was performed.
- Work performed inside `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-6aee75b2-grotto-g18-approve-once-c2040b1b`. This report is the only repository-content write. No implementation/test changes, commits, pushes, remediation or issue-status changes.
- Read bead `nostrc-u6v55.29`, the owner-amended Recommendations in `docs/investigations/grotto-approval-persistence-and-gates-2026-10-25.md:486-502`, and W1's verified-versus-claimed caller treatment. Scope is only the seven requested areas. The protocol-smells checklist was applied only to the changed AUTH/consent boundary, not expanded into a relay audit.
- **Confirmed/static** means established by source and production call path, not reproduced exploitation. **Plausible** identifies an unexecuted scenario/inference. No builds, application launches, daemon tests, UI tests, VM deployment or hosted CI ran in this review. Existing test code was inspected; bead-reported passes are not independent execution evidence. `scripts/install-hooks.sh`, `bd onboard`, `bd prime`, `bd show`, Git comparison/remote identity checks and read-only source inspection did run.

**Verdict: changes requested.** The main allow-path is implemented, but deny preservation through NIP-46 pairing, denial propagation to waiting calls, and effective revocation/permission scope need correction. No raw-key/conversation-key export addition was found. No critical finding is claimed.

## Severity-ranked findings

### F1 — High / P1, confirmed/static: NIP-46 pairing can replace an explicit deny with a declared allow

**Evidence:** `nips/nip55l/src/glib/signer_approval_service.c:1161-1166`; `nips/nip55l/src/glib/signer_permissions.c:958-979`, especially `:971-975`; row identity at `:138-153`.

The D-Bus declaration path filters tokens through `declared_token_policy()` and omits denied scopes before calling the writer (`signer_approval_service.c:940-957`). The new NIP-46 connect completion instead passes its canonical tokens directly to `signer_permissions_grant_tokens()`. That helper copies the records and unconditionally writes an allow for each exact principal/identity/operation/kind/relay scope. Decision is not part of the row ID. An existing deny for, for example, kind 13 is therefore overwritten rather than retained.

**Precondition:** an explicit pairing review for a principal/identity with an existing matching deny, such as pre-existing policy before pairing or a later explicitly reviewed pairing. An ordinary automatic reconnect does not take this branch. The pairing answer authorizes the new set, but the specified invariant is that deny still wins; this is not an explicit unblock decision. Broad/wildcard denies with different row IDs still win, so the defect is exact-row replacement, not every deny. A network reproduction was not run.

**Required boundary:** apply deny preservation inside the common declared-set writer or equivalently on every caller, including NIP-46. Test a pre-existing exact deny through actual pairing consent.

### F2 — Medium / P2, confirmed/static: “Block app” disables declared grants but does not remove the set

**Evidence:** `nips/nip55l/src/glib/signer_approval_service.c:1457-1481`; `apps/grotto/daemon/grotto_control.c:650-654`; `apps/grotto/daemon/grotto_clients.c:161-175`. Contrast actual revoke at `grotto_control.c:735-743`.

The dialog's Block app decision reaches `grotto_clients_decide()`, which sets `blocked=true`; neither that callback nor the decision branch clears permission rows. The evaluator correctly denies a blocked client (`signer_permissions.c:514-520`), so this is **not signing while blocked**. However, the whole `source=declared` set, including any-relay AUTH, remains durable and can become effective again when the app is unblocked. That fails the requested removal semantics. RevokeClient does cancel the principal and clear all its rows, and is not subject to this finding.

**Required boundary:** distinguish a reversible block with retained authority from revocation; the requested Block app behavior must retire the declared set, not merely mask it. Test block → unblock and list persisted rows, not only the immediate denial.

### F3 — Medium / P2, confirmed/static: a denied declaration does not resolve its waiting calls as denied

**Evidence:** daemon `signer_approval_service.c:473-486,904-924,1053-1075`; Groundhog `gnome/groundhog/src/identity/gh-signer-nip55l.c:218-223,292-303,523-531`; controller `gnome/groundhog/src/app/gh-account-controller.c:386-391`.

Daemon `finish()` releases parked requests on every declaration outcome, without passing the outcome. `release_parked()` re-enters normal policy resolution: after deny-once, an ASK scope can announce its own review. A user who denied the one consolidated review can consequently get the per-kind reviews it was meant to replace. Groundhog likewise launches every waiting operation when the declaration object is freed, including on ApprovalDenied; its controller merely logs declaration failure.

**Important limit:** denial alone does not manufacture an allow. A daemon-parked ASK call still needs other authority or another positive answer. But Groundhog parks *all* ordinary calls: if a client already has permission for kind A and requests additional kind B, denying that declaration still launches a waiting A call, which can execute under its old grant. This is a **plausible runtime scenario, statically supported**, not a demonstrated unauthorized-signature bypass. It violates the requested “never auto-execute on a denied declaration” lifecycle contract.

No deny path here writes `needs-review`, `policy-review`, dormant or allow rows: `decide()` explicitly excludes declarations from remembered row writes (`signer_approval_service.c:1401-1412`). That invariant passes despite the propagation defect.

**Required boundary:** terminate the declaration's associated waiters on denial; retain the explicit UnknownMethod compatibility route separately. Add negative declaration tests with both new and already-authorized waiting operations.

### F4 — Medium / P2, confirmed/static: any-relay AUTH is real authority, but the review does not name it

**Evidence:** `signer_permissions.c:971-975,505-512`; `signer_approval_service.c:865-870`; UI `apps/grotto/src/ui/approval_dialog.c:128-145,166-183,1028-1035,1046-1062`.

G18 intentionally grants kind 22242 with `relay=*`, not with the exact relay shown in a single AUTH request. This is an explicit exception to W7/G15's relay-scoped design: Recommendations item 1 still says canonical-relay scope (`docs/investigations/grotto-approval-persistence-and-gates-2026-10-25.md:489`), while new item 13 authorizes any-relay declarations (`:502`). It is consistent with the owner's approve-once direction **only as an explicitly disclosed broader delegation**, not as unchanged relay-scoped authority.

The compact capability says “relay sign-in”; Details says “Relay authentication (kind 22242)” and duration. Neither says **“authenticate to any relay this app chooses.”** NIP-46 pairing copy refers to the displayed relays and requested permissions, which also does not explain this wildcard. The scope permits an app to associate the pinned identity with arbitrary relays without another review, a privacy/linkability and impersonation-at-relay risk. AUTH event validation still checks relay/challenge/content/freshness (`signer_approval_service.c:1320-1324,977-982`); that does not prove the app's relay choice or challenge provenance is trustworthy.

**Required boundary:** disclose any-relay scope in the one decision, including NIP-46 pairing. This is not a request to restore per-relay prompts against the owner's direction.

### F5 — Medium / P2, confirmed/static: declared `get_relays` is reported granted under a scope its real entrypoint never uses

**Evidence:** `signer_permissions.c:896-900,971-975`; `signer_service_g.c:444-447`; `signer_approval_service.c:34,1282,1347,984-987`; identity matching `signer_permissions.c:505-506`.

RequestPermissions accepts `get_relays` and writes it with the declaration's pinned npub. GetRelays/GetRelaysForApp has no identity argument and its gate operation is explicitly identity-less. The later operation therefore evaluates with empty identity and cannot match that pinned row. For an ask-by-default client with no independent preset/wildcard allow, the review can say the permission is saved and the declaration reply can list it as granted, yet the next real relay-list call asks again. It also cannot park behind a declaration because `pending_declaration()` requires an npub (`signer_approval_service.c:904-905`). Groundhog's 27-token set does not include this operation, so its lifecycle test cannot reveal this.

**Required boundary:** reconcile the declared scope with the identity-less API without falsely advertising persistence or silently broadening unrelated identity authority; test the actual named entrypoint after declaration.

### F6 — Medium / P2, confirmed/static: concurrent requests for one undeclared kind can produce multiple extension reviews

**Evidence:** `signer_approval_service.c:810-818,904-905,1073-1119`; extension flag `:404-409`.

An ordinary extension only gets the `declared_set` flag; it is not a pending declaration. Pending-declaration parking cannot find it. Joining is keyed by the complete event arguments, not the permission scope. Two different events of the same undeclared kind while the first review is pending can therefore each announce an extension review, if separated beyond the 100ms prompt limiter and within the pending-count limits. A persistent answer to the first can later retire other authorized pending requests, but does not prevent their earlier announcements.

**Required boundary:** one pending extension decision per principal/pinned identity/permission scope, while preserving the separate events' integrity and cancellation. The existing single-event extension test does not establish this concurrency property.

### F7 — Medium / P2, confirmed/static: Groundhog's new local waiting queue has no count bound

**Evidence:** `gnome/groundhog/src/identity/gh-signer-nip55l.c:535-610`, especially allocation/copy at `:586-605` and enqueue at `:609`; release at `:523-531`; D-Bus timeout at `:15-16,470-473`.

Daemon parked requests retain the global **256 request** bound and a **300-second** deadline (`signer_approval_service.c:774-784,1337-1348`). They bypass the smaller announced-review limits, but are not unbounded globally. In contrast, Groundhog's new `waiting` array accepts every validated ordinary call, retaining its task, input and parsed event without a queue-count/aggregate-byte cap. The 330-second declaration call timeout bounds the D-Bus wait once sent, not the number of retained local calls. The public facade also creates a Pending per call; no protective count check was found on the reviewed call path.

**Impact/precondition:** a burst of app work during a slow review can grow memory before reaching the daemon's cap. No memory-exhaustion experiment was run. This is a local availability/lifecycle defect, not a claim of remote arbitrary code execution.

**Required boundary:** bound this new queue and fail visibly on overflow. Keep binding cancellation and the old-daemon flow fallback.

## Requested boundary checks

### 1. Declaration containment

**Passes on the attested local path:** RequestPermissions uses the same caller-resolution machinery as ForApp (`signer_service_g.c:142-158`), not a principal supplied in the declaration. For attested non-browser callers the supplied `app_id` is a display claim, not authority. The gate normalizes the selected stored identity to an npub and pins the backend selector (`signer_approval_service.c:1127-1135`); later backend operations recheck it (`:1202-1205`). Parking compares principal and npub (`:909-912`). A different attested principal does not inherit rows merely by supplying another app's name.

**Explicit platform limitation, confirmed/static, inherited:** strict “attested principal only” is not true on all supported development paths. On non-Linux buses without caller PIDs, `unattested=true` (`signer_caller.c:562-573`); `signer_caller_for_claim()` constructs `claimed:<app_id>` directly (`:394-402`). G18's eligibility guard rejects only bare `claimed:`, not nonempty claimed names or `unattested` (`signer_approval_service.c:125-127`). Another same-user process choosing the same claim therefore shares the declared principal on that path. This extends an existing named-claim limitation into the new set API; it is not a newly introduced Linux attestation bypass. W1 labels it unverified, which is honest UI but not principal isolation. A claim that native macOS tests prove attested-principal separation would be incorrect. No cross-process attack was executed.

The token allowlist accepts only concrete sign_event kinds, public reads, message encrypt/decrypt and zap decrypt (`signer_permissions.c:895-921`); connect admission, ping, raw export and conversation-key export are not declared capabilities. SetPermissions ignores supplied source metadata and preserves `source=declared` only from an existing identical allow row (`:847-887`); row validation requires that provenance for `relay=*` (`:132-135`). It cannot mint a new wildcard simply by naming source/relay. The test attempts wildcard manufacture from a fresh store (`test_signer_permissions.c:684-708`), although not with a forged source field too.

Ordinary RequestPermissions preserves deny rows and reports blocked entries (`signer_approval_service.c:940-957`); actual evaluation returns matching denies before allows/presets (`signer_permissions.c:505-520`). Repeated sets with a new ASK token require review (`signer_approval_service.c:875-887`); already-covered sets write nothing when auto-admitted (`:956`). Thus an existing client's *new unauthorized* scope is not silently granted by merely repeating this API. Prior tier-authorized scopes are already authority, not a new bypass. Exceptions/limits: F1, F2 and the inherited claimed-name path above.

### 2. Parking

Daemon count/time bounds are present (F7). `connection_closed()` collects and cancels all continuations for the lost connection, including unannounced parked ones (`signer_approval_service.c:1597-1613`). Groundhog free/select cancels the declaration and ordinary calls (`gh-signer.c:57-63,108-117,133-142`); the queued operations share cancellation and binding generation. The identity test includes free-with-declaration-and-waiter cancellation (`gnome/groundhog/tests/identity/test_identity.c:597-613`). Source supports binding-loss cancellation; it was not executed here. Denial leaves no dormant rows but does not propagate one negative answer to all waiting calls (F3). Local queue-count bound is absent (F7).

### 3. Extend-set reviews

A single undeclared custom/ASK kind raises the ordinary review with `declared_set=true`, a headline about an additional permission and **Always allow** selected by default (`approval_dialog.c:865-868,890-923`). Allow once remains an explicitly temporary alternative; until-revoked is not disguised as once. Ordinary reusable success writes the exact scope. Repeated declarations cannot silently grant new ASK scopes. This is not blanket permission for every possible future kind: prior routine presets can already authorize their covered operations. Concurrent same-kind requests are the gap in F6. The inspected lifecycle tests exercise one extension with **one-time** consent, not persistent-extension repeat suppression.

### 4. AUTH any-relay

Intentional broader delegation, not exact-relay isolation. Wildcard provenance restrictions and ordinary AUTH syntax validation remain. Exact matching relay-specific denies still win over the wildcard allow at actual AUTH evaluation. Risk disclosure is insufficient (F4); the review does not name the app's freedom to choose any relay. No fresh relay connection or live NIP-42 exchange was tested.

### 5. Groundhog signing-set spot-check

In this table, `src/` and bare `gh-*.c` paths are relative to `gnome/groundhog/`; libmarmot paths are repository-relative.

`gnome/groundhog/src/identity/gh-signer.c:242-266` contains **24 signing kinds + nip04_decrypt/nip44_encrypt/nip44_decrypt**, not “every numeric kind mentioned by the app.”

| Requested area | Source-grounded result |
| --- | --- |
| NIP-29 9000–9021 / 10009 | Templates produce 9, 9021/9022, 9000/9001/9002/9005/9007/9009 (`src/nip29/gh-nip29-template.c:290,377-430,547,566,628,640`); group list is built and sent to the account signer (`gh-nip29-group-list.c:643-664`). These are declared; unused intervening numbers need not be declared. Outbox reaches account signing (`gh-nip29-outbox.c:650-664`). |
| MLS 443/444/445 | 443 is deliberately no longer produced/accepted (`libmarmot/src/credentials.c:27-28`); key packages use 30443 (`:783-793`, `libmarmot/include/marmot/marmot-types.h:28-36`) and reach the account signer (`src/mls/gh-mls-service.c:8279-8309,8319-8349`). 444 is an unsigned welcome rumor (`:6116-6133`), sealed via NIP-17. 445 uses the MLS/ephemeral signing path, not the account signer; `libmarmot/src/messages.c:703-705,737-743` constructs/signs the ephemeral envelope. 450 identity proof is sent through the account controller (`gh-mls-service.c:7189-7198`); 450 and 30443 are declared. |
| 13 | Seal constructed with kind 13 and sent to the account controller (`src/app/gh-nip17-envelope.c:237-256`). Declared. The outer wrap is signed with an ephemeral key (`:277`), so 1059 is correctly absent. |
| 22242 | AUTH template constructors (`src/relay/gh-relay-auth.c:151,209`), account-auth signer integration (`src/app/gh-account-auth.c`). Declared. |
| 0 / 3 / 10002 / 10050 | 10002 relay-list and 10050 inbox-list construction/signing are present (`src/app/gh-relay-list-setup.c:172`, `src/app/gh-inbox-setup.c:242-261,342`), and declared. No Groundhog account-signing producer of 0 or 3 was found in the inspected caller/template searches; the app reads profiles/contacts, which is not signing them. Their absence is not treated as a confirmed missing-declaration bug. Grotto's separate profile-management path is not Groundhog account signing. |
| 5 | MLS deletion constructs kind 5 and submits the account signature (`src/mls/gh-mls-service.c:8542-8558`, also `:8749-8767`); declared. |

Groundhog sends its declaration on each Grotto binding (`gh-account-controller.c:404-412`), but does **not necessarily re-raise a review**: daemon coverage is evaluated first and a covered set completes silently. Existing-grant/restart API behavior is tested, not a full repeated Groundhog launch. Old-daemon UnknownMethod releases ordinary calls; their existing context/capability machinery still selects CallWithContext where supported (`gh-signer-nip55l.c:335-389,438-451,460-473`). Thus G11 flow consent remains the per-request fallback rather than being removed. The new test demonstrates transport fallback against a fake old signer, not end-to-end real old-daemon G11 consent. F3/F7 concern the new waiting adapter, not an invented need to declare rumors/relay-owned events.

### 6. Dialog, W1 honesty/accessibility and libadwaita floor

- Blueprint specifies **360 × 560** content and one default-collapsed Details expander containing effect/reason/expiry/content/fingerprints/time (`apps/grotto/data/ui/approval-dialog.blp:8-16,91-111`; collapse on each snapshot at `approval_dialog.c:1107`). Main action, identity and choice remain outside; the decision footer stays outside the scroller. Capability groups and registry-backed detail names are real code (`approval_dialog.c:89-183`). F4 is the scope-disclosure exception; F5 is a non-effective declared capability.
- New 360×640 tests inspect scroller extent, mapped content, collapsed Details and visible decision controls (`test-approval-dialog.c:862-932,943-976`). They allow 2.5px focus-padding difference. These are representative English fixtures, not proof for every translation, larger text or 1.5 runtime. They were **not run here**.
- W1 S1 fix is retained: remote NIP-46 says verified **transport key; app name unverified**, including contradictory metadata; claimed local names remain unverified; browser bridge is identified (`approval_dialog.c:734-758`, retained caller-label tests `test-approval-dialog.c:630-660`). No regression to “Verified local app” for NIP-46 was found. Existing keyboard/flow/batch/no-primary-consent tests remain registered (`:1017-1046`); accessible flow labels are asserted at `:407`. Dedicated ui-accessibility registration remains (`apps/grotto/tests/CMakeLists.txt:627-628`). New UI tests do not independently prove every declared-set choice against the real daemon.
- **Explicit `content-height: 560` is acceptable at the 1.5 API floor.** Pinned [libadwaita 1.5.0 source](https://raw.githubusercontent.com/GNOME/libadwaita/1.5.0/src/adw-dialog.c) defines content-height and follows-content-size since 1.5; the latter defaults false. Automatic mode uses a bottom sheet at max-width 450px or max-height 360px; size is constrained by the parent. A fixed preferred height plus scrolled overflow is supported, not use of a newer-only property. `apps/grotto/CMakeLists.txt:34` requires 1.5. This source check is **not** a native libadwaita 1.5 layout run.
- **Header case:** Review Request, Details, Signing As and the action/buttons use header capitalization. The personalized “Groundhog wants to use your identity” / “wants an additional permission” headings are complete informal sentences, for which [GNOME HIG explicitly permits sentence capitalization](https://developer.gnome.org/hig/guidelines/writing-style.html#heading-style). No capitalization-only blocker is raised.

### 7. Tests actually present, and limits

| Claim | What is present |
| --- | --- |
| Real-daemon 27-op, one-review lifecycle | `nips/nip55l/tests/test_signer_dbus_contract.c:1191-1363`, invoked at `:3736-3745`. Declares 27 entries, watches exactly one review, parks join+encrypt, checks signature/decrypt results for representative group/seal/AUTH-two-relays/lists/deletion/custom calls, and re-declares silently. It does **not execute every one of the 27 operations**, nor launch Groundhog itself. |
| Service lifecycle/parking | `test_signer_approval_service.c:606-716`: allow path, two parked calls, repeat coverage, partial explicit-deny result, malformed/anonymous rejection and declared custom-kind trust. Missing: denied declaration with parked calls, parking saturation/time expiry, loss-of-binding with daemon-parked declarations, concurrent declarations/extensions, already-authorized local waiters after denial. Generic lifecycle limits/cancellation exist elsewhere, but are not declaration-specific negative proof. |
| Extension | Contract `test_signer_dbus_contract.c:1313-1339` and service `test_signer_approval_service.c:668-677` check one custom-kind extension, selecting one-time approval. UI `test-approval-dialog.c:977-994` checks persistent default copy but then chooses Allow once. Missing: real dialog/daemon persistent-extension repeat and concurrent same-kind different-event reviews. |
| Persistence/migration/provenance | `test_signer_permissions.c:612-702` covers new declared rows, wildcard AUTH, restart, management round-trip, clear and fresh-store wildcard refusal. Existing `:538-593` tests v2→policy-review migration, preserved deny, removed conversation-key rows and exact-relay activation. Missing: old-client rows → denied/allowed *declaration* transition, NIP-46 deny preservation, block/unblock set removal, source-field forgery, get_relays effectiveness. No new dedicated G18 migration test connects these pieces. |
| Groundhog | `gnome/groundhog/tests/identity/test_identity.c:544-613` covers declaration arguments/identity/claim, waiting, fake UnknownMethod fallback, malformed token and free cancellation. It uses a fake service. A successful declaration result with empty grants is accepted by this test; no integration proves full granted/denied-result interpretation, repeated app launch, queue bound or negative declaration behavior. |
| Dialog/a11y | Four new cases plus the retained W1 tests above. Apple CTest UI route is explicitly disabled/skipped (`apps/grotto/tests/CMakeLists.txt:227-231`); bead claims a separate native 37/37 run. That claim was read, not re-executed or promoted to GNOME 46/1.5 proof. |

## Delivery limits

This is a static, scoped review artifact, not a merge/deploy approval or VM acceptance receipt. The report does not dispute the bead's recorded positive runs; it identifies negative paths those tests do not establish. No unrelated baseline failures or repository-wide follow-up programme is added. Because only this report was added, there is **no shipped-component version bump**; branch version-fold decisions were read, not modified.
