# Groundhog social lists plan: critique

Date: 2026-11-08

## Context and scope

Reviewed `docs/plans/groundhog-social-lists-2026-11-08.md` ("plan") against the generated response in `prompt-exports/oracle-plan-2026-11-08-010859-deep-plan-groundhog-6572.md` ("export", from line 247; the composed prompt and file dump above it are context only). Both were read in full. Owner decisions D1–D4 are treated as settled.

Code was spot-checked only at the seams named below. Paths without a top-level directory are under `gnome/groundhog/`. `plan:N` and `export:N` are line numbers in those two files.

Each finding is tagged with the brief's category: **[1]** lost from the export, **[2]** under-specified or contradictory, **[3]** disproved by code or replaceable by a simpler design, **[4]** absent from both documents. Category 5 is the Questions section.

## Summary

The architecture holds up: shared codecs, one list service and local-first storage are the right shape. §8's deliberate changes are justified by the code (see "Checked, no finding"). The main risks are concentrated in four places:

1. Draft sync can overwrite newer text and can bring back deleted conversations (H1, H2), and its v13 table cannot hold the state the algorithm needs (H3).
2. `social_hidden` is applied at admission and paging, but not at the other places that read or count messages (H4, H5).
3. Stage 4 cannot start without inventing relay destinations and AUTH purposes the plan never states (H6).
4. The "NIP-46 is excluded from relay denial" and "kind 3 is read-only" guarantees have no mechanism behind them (H7, H8).

H3 is the most urgent to fix before implementation, because migrations are append-only.

## Findings

### High

#### H1. Enabling sync lets an old local draft overwrite a newer remote one [4]

- **Plan:** enabling sync marks every existing draft dirty (plan:240). A publication uses `created_at = MAX(now, last_created_at + 1)` (plan:878). A clean receiver replaces its text with any newer revision and keeps no conflict copy (plan:900).
- **Code:** `conversations.draft` has no timestamp (`src/store/gh-store-schema.c:38`), so a draft's age is unknown.
- **Failure:** device A holds a year-old draft for room X. Device B published a recent one. A enables sync, A's draft gets `created_at = now`, and it wins on B. If B is clean, B's newer text is lost with no conflict copy. Two different `d` values reach the same result through plan:904.
- **Correction:** on enable, do not mark drafts dirty until import reaches EOSE on every configured 10013 relay. Treat a pre-existing draft as age-unknown: if a remote revision exists for the room and differs, run conflict handling and do not publish the local text as newest. Publish only rooms with no remote revision. Add this case to Stage 8.

#### H2. Forgetting a conversation leaves its remote draft, which can then bring the conversation back [4]

- **Plan:** a tombstone exists only on the enqueue path (plan:927–929). Import ignores only blocked rooms (plan:895).
- **Code:** `gh_store_forget_conversation()` clears the draft (`src/store/gh-store.c:4327`, `:4366-4370`) and truncates the WAL so the content is gone now (`:4375-4377`). No sync state is touched. `gh_store_conversations_set_draft()` finds the existing row and writes the draft without checking `forgotten_before` (`src/store/gh-store-conversations.c:984-992`).
- **Failure:** the user deletes a conversation, or uses Request Block's forget. The encrypted draft stays on the private relays for up to 30 days. Another device's later revision, or a re-import, puts draft text back in the deleted room, and plan:750 then lists the room because it has a draft.
- **Correction:** the forget transaction sets `cleared_at` and records blank events for every known `d`, on the same terms as plan:929. Import rejects any revision with `created_at <= forgotten_before`. Whole-account forget (`gh_store_forget_account`, `gh-store.c:2064`) cannot queue anything, so its confirmation text should say remote drafts remain until they expire.

#### H3. `draft_sync_state` cannot represent the algorithm the plan specifies [1][2]

§8's removal of `composer_drafts` is justified, but the removal also took out state that §3.7 still depends on.

- **No local revision:** `composer_drafts` carried `updated_at` and `local_revision` (export:765). The surviving table (plan:633) has neither, yet "a newer local revision supersedes" and "a revision changed during signing discards the result" (§3.7 cadence) both need a token to compare.
- **One pending slot, many events:** plan:633 has one `pending_event_json` per room, but plan:927 queues a blank for *every* known identifier, and a head revision can be pending alongside them.
- **No per-target outcomes:** `social_publications` and `report_publications` have `targets_json`/`outcomes_json`; drafts do not, although plan:883–885 needs acceptance tracking for an exact revision.
- **Contradictory write path:** plan:882 requires saving the draft and marking it dirty "in one transaction". plan:1342 says to "signal the draft sync service after each local save", and plan:1309 lists no draft change for `gh-store-conversations.c`. The transaction lives inside `gh_store_conversations_set_draft()` (`src/store/gh-store-conversations.c:969-998`), called from `src/app/gh-send-ui.c:397`.
- **No wake-up path from enqueue:** `gh_store_enqueue` runs at three sites (`src/app/gh-outbox.c:2028, 2131, 2193`), and nothing tells `GhDraftSync` a tombstone was committed.
- **Correction:** add `local_revision INTEGER NOT NULL DEFAULT 0` to `draft_sync_state`. Replace `pending_event_json` with a child table keyed `(room_key, d)` holding `event_json`, `targets_json`, `outcomes_json` and `state`. Do the dirty/revision update inside `set_draft`'s existing transaction and use the signal only to wake the scheduler. Name the post-commit notification for enqueue. State that blank events carry `k=14`, or the `#k:["14"]` filter at plan:889 will never return them.

#### H4. "Follow the normal persistence path" for `HIDE_RETAIN` corrupts room state [2]

- **Plan:** plan:724 uses the normal path with `social_hidden=1`. Recomputation is specified only for policy changes (plan:742 onward).
- **Code:** the normal NIP-17 path sets `.title`, `.unread = !own` and `.request_state = PENDING` (`src/store/gh-store-conversations.c:375-378`), then updates read state (`:405`), and overwrites `commit->hidden` with the room's block state (`:416`). `gh_conversation_store_admit()` returns `HIDDEN` before `sync_unread` (`src/app/gh-conversation-store.c:321-322`).
- **Failure:** a hidden message raises the stored unread count without the model noticing, renames the room from a hidden subject, and advances activity. The counts then disagree until the next recount.
- **Correction:** specify a `HIDE_RETAIN` admission variant for each backend: `unread = FALSE`, no title update, no activity advance, no read-state change, and `commit->hidden = TRUE` set last. It still needs a conversation row, and Requests queries must apply the same "no visible messages" omission as plan:750.

#### H5. Search and most unread recounts ignore `social_hidden` [4]

- **Plan:** plan:742 names only restore and paging queries. plan:1308 lists `gh-store.c` for the draft change alone.
- **Code:** `gh_store_search_message_rooms()` searches every stored body (`src/store/gh-store.c:2316-2320`). Unread is recomputed from `messages` at `:2705`, `:4192`, `:4262` and `:4437`, on the read-move and expiry/purge paths.
- **Failure:** searching a word hidden through kind 10000 still returns the room. After any read move or purge, hidden messages return to the unread badge.
- **Correction:** add `m.social_hidden = 0` to search and all four recounts, add those to the `gh-store.c` row at plan:1308, and cover them in `groundhog-message-policy`.

#### H6. Destinations and AUTH purposes for the list family are never stated [2]

- **Plan:** read-plan entries carry a publish policy (plan:488), but nothing says where kinds 10000, 10006, 10030, 10992 or the sets are read from or published to (plan:228–229). Only 3, 10013 and 10063 have stated sets (10009, 10002 and 10050 keep their existing ones), and 10008's publish targets are missing too. This is the central privacy decision in Stage 4.
- **Reports and badges:** plan:1063 says "ephemeral challenge AUTH", but callers name a purpose and never pick an identity (`src/app/gh-auth-policy.h:13-14`). The charter row promises report and badge scopes (plan:1218), while plan:1323 adds only private-sync purposes.
- **Destination checks:** plan:942 and plan:1323 imply the policy checks destinations. `gh_auth_policy_decide()` takes only a purpose (`gh-auth-policy.h:91`) and the policy holds no relay list.
- **Correction:** add a per-kind table of read sources, publish targets and AUTH purpose. Recommended defaults:
  - Lists and sets: NIP-65 write relays, `OWN_LIST_DISCOVERY` to read, `OWN_LIST_PUBLISH` to write. 10009 stays as it is. (See Q4.)
  - Reports, 10008, 10063: reuse `PUBLIC_POST`, already defined as ephemeral on own NIP-65 write relays (`gh-auth-policy.h:28`). Treat PD-6's "scopes" as socket isolation and name those labels separately.
  - Badge lookups: `CONTACT_DIRECTORY`.
  - Private-sync URLs: follow the `SELF_WRAP` precedent, where the caller guarantees the URL (`gh-auth-policy.h:46-48`) and `ACCOUNT_PURPOSE_FILES` restricts who may use it (`tests/check_privacy.py:319`). Drop "destination checks" from plan:1323.

#### H7. Nothing implements "NIP-46 connections are excluded from relay denial" [2]

- **Plan:** plan:973 excludes signer-control connections; §4.4 lists only the transports.
- **Code:** NIP-46 uses the same default scope and publish constructors as everything else (`src/identity/gh-nip46-session.c:512-515, 782-784, 1294-1296`). Those constructors route through transports installed process-wide (`src/relay/gh-relay-guard.c:252-256`). A guard handle knows only the URL and scope (`:292`), and `open_inner` checks only mode and URL (`src/net/gh-relay-net.c:84`).
- **Failure:** adding a bunker's relay to kind 10006 cuts off the account's signer, including the signing needed to undo that edit, or the exclusion is quietly left out.
- **Correction:** add an explicit connection class to `GhRelayScope` and `GhRelayPublish` (for example `set_signer_control()`), set only at those three NIP-46 sites, checked by the deny hook, and restricted by a `check_privacy.py` rule to `gh-nip46-session.c`. Add `gh-nip46-session.c`, `gh-relay-scope.c` and `gh-relay-publish.c` to §4.4.

#### H8. Kind 3 has no owner, no read-only policy, and its library is off by default [2]

- **No read-only value:** the publish policies are `AUTO_WHEN_SYNC_ENABLED`, `AUTO_ALWAYS` and `EXPLICIT_ONLY` (plan:488). Kind 3 must never be published (plan:230), and 10002/10050 are also tracked with an external sole editor (plan:1356). None of the three values expresses that.
- **Not in the registry:** kind 3 is absent from the §3.2 schema table (from plan:342), so it is unclear whether it is a service document at all, or which bounds and states apply.
- **Build dependency:** plan:770 calls `nostr_nip02_parse_follow_list()`, but `ENABLE_NIP02` defaults to `OFF` (`NipOptions.cmake:8`) and nothing else in the repository's CMake, packaging or script files turns it on. plan:1288 and §5.3 item 1 mention only NIP-37/56 and NIP-44/51.
- **Correction:** add a `READ_ONLY` policy that the service refuses to publish, and assign it to 3, 10002 and 10050. Add kind 3 to the registry, or state that it is a bespoke reader. Add NIP-02 to the `NipOptions.cmake`/generator row, the Groundhog link list and §5.3.

#### H9. Stored signed events have no defined restart behaviour [4]

- **Plan:** signed events persist before sending (plan:524, plan:883) and failures retry "the same stored signed event" (plan:918), yet generation validity is checked "before every external effect" (plan:467). After a restart or account switch the generation differs, so as written, stored events can never be sent. Nothing specifies a scan at bind.
- **Related gap:** disabling sync "cancels… unstarted signed publications" (plan:243), but the rows remain, so a later enable could send a stale signature.
- **Precedent:** `GhNip29GroupList` keeps its publication state in memory and clears it at unbind (`src/nip29/gh-nip29-group-list.c:955`), so there is no existing rule to inherit.
- **Correction:** define a bind-time resume for `social_publications` and pending draft events. Before sending, re-validate under the current generation: same account, sync still enabled, event still newest for its address, target still allowed. Discard anything stale and re-sign from current state. Disabling sync deletes unsent events but keeps tombstones.

### Medium

#### M1. Follow eligibility contradicts itself and has no lookup trigger [2]

- plan:786–788 makes every followed account eligible, while plan:803 limits lookups to *surfaced* followed peers. The first would leak what the second forbids.
- plan:791 says following grants no "relay lookup privileges", which contradicts plan:803 and the PD-8 amendment (plan:1219).
- `GhContactDirectory` has no notion of "surfaced": its rescan covers NIP-17 non-requests and MLS DMs only (`src/app/gh-contact-directory.c:1149-1172`), and runs and `profiles` are `accepted`-only (`:60, :532, :636, :897`).
- A followed Request has no refresh trigger, so D3's avatar could wait for a 2–30 minute run (`:15-16`).
- **Correction:** use `accepted OR own OR (followed AND surfaced AND master)`. Reword plan:791 as "grants kind-0 lookup only". Name the input that feeds Request and roster peers to the directory. Route a newly surfaced followed Request through the existing U(5,60)s single-contact path (`:17-18, :648`).

#### M2. Reactions have durable storage the plan does not account for [2]

- plan:754 defines display only, and plan:1347 lists the in-memory store. `GhReactionStore` has its own admit delegate (`src/app/gh-reaction-store.h:15`; `src/store/gh-store-reactions.c:263`) and durable `reactions`, `pending_reactions` and `reaction_tombstones` tables (`src/store/gh-store-schema.c:377-401`). `gh-store-reactions.c` appears nowhere in §4.
- Whether a hidden `p`'s reaction is stored or dropped decides whether unhiding restores it. A pending reaction applied later can bypass the policy.
- **Correction:** recommended rule: store it and filter at projection, so it stays reversible, and apply the same filter when a pending reaction resolves. Add `gh-store-reactions.c` to §4.2.

#### M3. List bounds would reject ordinary follow lists, if they apply [2]

- plan:377–379 caps events at 128 KiB and 4,096 tags, and oversized revisions become unreadable (plan:384). A `p` tag with a relay hint is roughly 100–120 bytes, so about 1,100–1,300 follows hits the cap. The transport itself allows 1 MiB frames (`src/net/gh-relay-soup.c:12`).
- **Failure:** an active user's avatars disappear, and the only sign is a state on a list the user never edits in Groundhog.
- **Correction:** give read-only kind 3 its own limit, at the frame size with a tag cap, or state the degraded behaviour and surface it.

#### M4. The `GhNip29GroupList` migration does not fit the adapter contract [2]

- Its list is derived from room state, not edited, and removal is time-based: a group left here is removed "unless the list is newer than that change" (`src/nip29/gh-nip29-group-list.h:35-42`). The service's remove rule has no such comparison (plan:518). "Retain application-specific reconciliation" (plan:554) names no hook.
- It re-reads when relays or settings change (`gh-nip29-group-list.c:962-998`) and waits for own-list discovery (`.h:32`), but the service exposes only `start(read_plan)` (plan:479). 10013 changes need the same.
- It lives in `GhAppOutbox` per store (`.h:14`), while the service belongs to `GhSocialLists`; the construction order is unstated (plan:1321).
- **Correction:** add a pre-publish `derive_operations` adapter callback, an `update_read_plan()` call and a "wait for dependency" state, and state the construction order.

#### M5. Nothing says the hide policy is active before messages arrive [4]

- plan:975 orders the deny policy before network start, but plan:657 gives no binding point for `GhMessagePolicy`. Inboxes are built independently of the store bind (`src/app/gh-app-services.c:465-479`), and delegates arrive through `gh_conversation_store_set_account()` (`src/app/gh-conversation-store.c:181`).
- **Failure:** catch-up after unlock admits messages from a hidden `p` as `INCLUDE`. Because those are seen and stored normally, a drop can never apply to them.
- **Correction:** install the policy in the same call as the delegate, clear it with the delegate, and return `ADD_FAILED` if an account-bound policy has not loaded.

#### M6. The custom-emoji change has no send path for groups or reactions [1][2]

- §8's reason holds, but plan:1093 now covers NIP-29 and MLS while the delegate is text-only: `send(conversation, text, data, error)` (`src/app/gh-send-ui.h:94`). The export's rows for `gh-group-ui.c` and `gh-mls-ui.c` were removed along with group drafts, and §4 has no replacement.
- `GhReaction` holds only an emoji string capped at 64 bytes (`src/app/gh-reaction.h:23, 36-41`), so kind-7 tags have no carrier and a long `:shortcode:` is rejected. The export's guard about advertising unsupported reactions (export:1247) was dropped without a replacement.
- `GhWebKind` has no emoji or badge kind (`src/ui/gh-web-content.h:6-7`) and `gh-web-content` is not in §4, though plan:1095 and plan:1101 define two consent models. Rendering is listed only as "safe emoji fallback".
- Stage 10's "`emoji` tags inside private rumors only" (plan:1634) contradicts sending them on NIP-29 events (plan:1093).
- **Correction:** add a tag-carrying delegate send, a tag field on the reaction send path, a length check, the `GhWebKind` decision and a rendering row. Reword the Stage 10 check.

#### M7. The `GNostrMuteList` facade has no storage or account to bind [2]

- The adapter needs an atomic load/commit (plan:461) and `new(account, generation, …)` (plan:560). The facade is `get_default(void)` (`nostr-gobject/include/nostr-gobject-1.0/gnostr-mute-list.h:31`), takes a pubkey per fetch (`:84`), and `gnostr-mute-list.c` uses no persistence API. §5.3 item 4 (plan:1469) covers threading and binding only.
- Durable removal records and pending edits have nowhere to live for Gnostr.
- **Correction:** state that the Gnostr adapter is in-memory with session-only semantics, and that a fetch rebinds the account.

#### M8. Importing a draft has no path to the open composer or the room list [2]

- The conflict table depends on focus and programmatic update (plan:900–901), but plan:1342 adds only outward signalling. `load_draft()` is static (`src/app/gh-send-ui.c:406-407`).
- `draft_saved` must also change. Otherwise `on_draft_changed` drops a later edit that happens to match the old text (`:394`).
- `set_draft` creates only a database row, marked accepted (`src/store/gh-store-conversations.c:989-991`), so a new room does not reach `GhConversationStore` until the next restore.
- **Correction:** add a `GhSendUi` apply-remote-draft entry point and a focus query, and specify how an imported room enters the model.

#### M9. "Report Message" needs a visibility state that does not exist [3]

- plan:1033 requires explicit public visibility, with unknown treated as private. `is_private` is a plain boolean defaulting to false (`src/nip29/gh-nip29-group.h:124`; `src/nip29/gh-nip29-service.c:1498`), and a room with no metadata is reported as not private (`gh-nip29-service.c:382`).
- **Failure:** using `!is_private` would offer a public-tagged report for a group whose visibility is unknown, disclosing an event ID.
- **Correction:** add a predicate that requires received kind-39000 metadata and `!is_private`. Add the NIP-29 service files for it to §4.3.

#### M10. Background draft signing competes with the user on remote signers [4]

- `sign_event` is interactive (`src/identity/gh-nip46-session.c:1355`) and the interactive queue always drains first (`:595-597`). Every draft revision and blank would use it, up to once per room per 60 s, and `GhSignerContext` never reaches NIP-46 (plan §2.7), so a prompt arrives with no explanation.
- The list service also has no decrypt bound, unlike drafts (plan:893). The bulk queue refuses work beyond 256 (`:16, :1405`).
- **Correction:** run automatic draft signing in the bulk lane or a dedicated lane. Bound the service's concurrent NIP-44 work. See Q3.

#### M11. The stage table has wrong dependencies [2]

- Stage 5 depends only on 3 (plan:38), but follow discovery needs the Groundhog adapter that lands in Stage 4 (plan:1522).
- Stage 3 edits lists through the service (plan:36), which requires the storage half of that same adapter.
- Reports and badges depend on Stage 8 only for "schema v13" (plan:42–43, plan:1595), which ties them to draft sync. The migrations are append-only, but nothing forces them to be shared.
- **Correction:** move the adapter's storage half to Stage 3. Have Stage 5 depend on 4, or state a bespoke reader. Give reports and badge cache their own migration so Stages 9 and 10 depend only on what they use.

#### M12. §8 understates what removing the manifest gives up [1][3]

- The removal is justified, but §8's "one added property, stopping a rare stale copy after concurrent creation" (plan:1669) and the known limit (plan:933) are too narrow. A device learns a `d` only by decrypting, newest 64 per bind (plan:893). A second `d` therefore also appears when a user types before import finishes, when the signer is unavailable, or beyond the 64-event bound.
- **Correction:** broaden the known limit, and require import to reach EOSE before minting a new `d` for a room.

#### M13. A hide-account edit does not need a content scan or a global mask [3]

- plan:732–733 masks content and scans in batches for every policy change, on all rooms. `p` matching needs only `messages.sender_pubkey` (`src/store/gh-store-schema.c:52`).
- **Correction:** for a diff containing only `p` entries, including the report flow's Also Hide, do one indexed `UPDATE` plus recount in a single transaction, with no masking phase. Add a `sender_pubkey` index to v12. Keep the scan for `word`, `t` and `e`, and state whether its mask is global or per-room.

#### M14. Relay denial misses NIP-11 HTTP requests [4]

- §3.8 enforces only WebSocket paths (plan:957–973). `GhNip29Service` fetches the group relay's key over HTTP (`src/nip29/gh-nip29-service.c:1106`), so a denied relay still receives a request from the client.
- **Correction:** check the deny policy before that fetch and report the relay as policy-denied.

### Low

| ID | Cat. | Finding | Evidence | Correction |
|---|---|---|---|---|
| L1 | [2] | `entry_key` is undefined, and the same `p` in public and private parts collides on the primary key. `social_servers` is not stated to be derived, so there are two sources of truth with the 10063 document. | plan:607, 610, 613, 997 | Define the key (tag, NUL, normalized value), state which part wins, and say `social_servers` is derived. |
| L2 | [3] | Moving the `commit.hidden` check above the duplicate branch affects every hidden commit, including seen-only duplicates that now return `DUPLICATE`, and `gh-send-ui` treats `HIDDEN` as failure. | plan:720; `src/app/gh-conversation-store.c:314-322`; `src/store/gh-store-conversations.c:418`; `src/app/gh-send-ui.c:183` | Apply the early return only when disposition is not `INCLUDE`. |
| L3 | [2] | "Blocked/hidden accounts are ineligible" treats Block as per-account, but Block is per room. | plan:822, 168 | Say "accounts whose only direct room is blocked", or drop "blocked". |
| L4 | [4] | Blossom step 1 copies the global list into any account with no state, including accounts created later, while step 5 says pre-existing accounts only. The key is never cleared. | plan:1001, 1004, 1005 | Import only for stores migrated from below v11; set `blossom_imported=1` on fresh stores. |
| L5 | [3] | Two stale references. `nostrc-98se` is described as unfixed, but mute and block already withdraw. The `GSETTINGS` citation is off. | plan:168; `src/app/gh-conversation-actions.c:4-8, 18-19`; plan:177; `tests/check_privacy.py:217` | Correct §2.7 and consider closing the bead. |
| L6 | [1] | The export's "switch flushing" test was dropped from Stage 8 and is not in §8, though the behaviour remains. | export:1761; plan:1607; `src/app/gh-send-ui.c:470` | Restore it. |
| L7 | [2] | No version decision for `gnostr`, a separately versioned component whose mute behaviour changes through the facade. AGENTS.md asks for the decision to be recorded. | plan:1261–1262; `VERSION_MANIFEST.md:29` | Record it, even if it is "no bump". |

## Questions that would change the design

**Q1. Does the kind-3 read run when `load-profile-pictures` is off?** The plan says follows are read with sync off (plan:230) and that master-off startup "fetches nothing" (plan:38). If the read runs, a default install gains a new REQ and the Stage 5 check must be scoped to pictures and kind 0. If it does not, "You follow this account" (D3) disappears for master-off users. This decides Stage 5's gating and `/privacy/avatars/master-off-all-surfaces`.

**Q2. Should the first import of an existing remote kind 10000 need confirmation?** Enabling sync can apply a list curated in another client, and `p` entries then permanently drop new messages. This does not reopen D4's meaning of hide. It only asks whether the first import shows a preview, which adds a step to Stage 4 or 6.

**Q3. On a signer that approves each signature, is automatic draft sync wanted?** If not, Stage 8 needs a capability check and a manual "Sync Now" mode. If yes, what should a prompt say, given that NIP-46 receives no context?

**Q4. May private list events go to discovery relays?** 10009 does this today (`src/nip29/gh-nip29-group-list.h:27-29`). Extending it reveals more about list activity; limiting to write relays reduces availability on a new device. The answer fills in H6's table.

**Q5. Is one dead relay meant to stop automatic list sync indefinitely?** plan:537 requires every source to settle successfully, matching `GH_NIP29_GROUP_LIST_PARTIAL` (`gh-nip29-group-list.h:68`). Across many document kinds, that means a repeated per-document confirmation. A quorum or an "ignore this relay" option would change the service's state machine, so it should be decided before Stage 2.

## Checked, no finding

**§8 reasons confirmed against the code:**

- The manifest's public `d` would fingerprint the client (charter lines 206, 227).
- Group drafts are tracked by `nostrc-2lpm`, and no reply target is stored today (`src/store/gh-store-schema.c:25-40`).
- The onboarding switch concerns kind 10002, not 10009 (`data/ui/gh-onboarding-view.blp:674-677`), and 10009 publishes automatically (`gh-nip29-group-list.h:10-47`).
- The kind-3 source set matches `wanted_urls()` (`gh-nip29-group-list.c:848`).
- The corrected generator and Flatpak paths exist.

**Candidates disproved:**

- `mls_messages` does not keep plaintext for dropped messages; it stores only the inner event ID, with the plaintext, tags and author left out (`src/store/gh-store-marmot.c:1130-1155`).
- The export's "no separate post-enqueue clear" was safe to drop, because the composer already reports nothing after queueing (`src/ui/gh-composer.c:1263`).
- `GhNip29GroupList` has no durable removals needing migration (`gh-nip29-group-list.c:955`).
- Refusing to publish after a partial read is existing behaviour, not a regression.
- All 33 named Groundhog test targets checked are registered, along with `test_nip30`, `test_nip51`, `test_nip58` and the two Gnostr targets.

## Recommended order of correction

1. Before Stage 2, answer Q5 and add the service API changes from H8 and M4.
2. Before Stage 4, write the H6 table (with Q4).
3. Before v12, apply H4, H5 and M13.
4. Before v13, apply H1, H2 and H3; H3 changes an append-only schema, and the other two depend on the same tables. Apply H9 before Stage 4, since `social_publications` lands there.
5. Fix the stage table (M11) at the same time as the first of these edits.
