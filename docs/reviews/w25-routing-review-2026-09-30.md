# W25 review: slice M, routing rotation, group-id safety, SelfRemove upgrade (nostrc-ms4d, nostrc-scki, nostrc-8ndz)

- **Reviewer:** independent peer reviewer (AGENTS.md "Peer Review")
- **Branch reviewed:** `marmot/w25-routing` at `db9abf8a`, three commits on `08d35b4e` (master)
- **Review branch:** `review/w25-routing` (this document only)
- **Date:** 2026-10-01 (file named for the W24 review series)
- **Verdict:** **CHANGES-REQUIRED.**
  - **What holds:**
    - ms4d's routing privacy is sound. A relay is asked only for the addresses it carried, and the test proves it: reverting that rule fails the test.
    - Publishing goes only to the signed current routing.
    - The retention record is written in the Commit's own store transaction, and it survives a restart (also revert-proven).
    - The subscription fan-out a malicious admin can force is bounded.
    - Tor and network mode apply to the new scopes.
    - scki covers current ids and aliases, refuses for good, and fails closed on storage errors.
    - The live MDK 0.11 rotation case is a real run.
    - All gates are green.
  - **H1 (required):** the *automatic* SelfRemove upgrade must not ship before nostrc-zbmb is fixed. I recommend fixing zbmb in this slice.
    - After the upgrade, every Groundhog legacy group requires SelfRemove.
    - libmarmot then still commits an Add of a KeyPackage that lacks it (my probe: `rc=0`). OpenMLS/MDK members and the invitee reject that Add, so the group splits.
    - The alternative: ship the API, and keep the automatic trigger off until zbmb lands.
  - **M1 (should fix with H1):** the background Commit makes the admin's own changes fail with BUSY, and the copy for that is untrue. This slice's own GUI test hit the race.
  - Everything else is Low or Nit.

| Commit | Beads | Change |
|---|---|---|
| `9ddbc29d` | nostrc-scki, nostrc-8ndz | libmarmot 0.12.0 (unreleased). welcome.c `routing_id_held_elsewhere()` (current id or alias of another held group) checks on accept, and on arrival for adopted Welcomes. groups.c `marmot_get_self_remove_requirement()` / `marmot_require_self_remove()`: a legacy GroupContextExtensions Commit adding 0x000a. Adopted groups are refused. Tests in test_protocol, test_adopted and test_commits |
| `4d2846e8` | nostrc-ms4d, nostrc-scki, nostrc-8ndz | Groundhog follows signed 0x8004 routing. Earlier addresses are kept in `meta` `mls-routing:` and read per address set. `GH_MLS_SERVICE_ERROR_ADDRESS_TAKEN` and its copy. The automatic upgrade. driver-0.11 `update_routing` and harness case `routing-rotation`. MDK 0.8 case `groundhog-requires-self-remove` |
| `db9abf8a` | nostrc-8ndz, nostrc-ms4d | The GUI group-info test now waits for the automatic upgrade. Relays never placed in a scope count as never-subscribed |

No code or beads were changed. Scratch work:

| Location | Contents |
|---|---|
| `/tmp/rv-w25-routing-probe` | `zbmb_probe.c` (an Add of a non-SelfRemove KeyPackage into a group that requires SelfRemove), and the revert backups |
| `/tmp/mdk-v0.8.0` | a shallow clone of MDK v0.8.0 (`575ae29d`), for the CHANGELOG and LCD rule |

Docker hygiene:
- The live MDK 0.11 run used a private tag built from this worktree's `driver-0.11` (`nostrc-mdk-interop:0.11.0-rv-w25m`), removed afterwards.
- The MDK 0.8 run used the existing `:0.8.0` image via `MDK_INTEROP_DRIVER`. `driver/` is unchanged on this branch, so I did not rebuild the shared tag.
- The volume list before and after is identical (269 volumes). The sanitizer gate reused its shared volumes.

## Verification

| Check | Result |
|---|---|
| `cmake -G Ninja -DBUILD_GROUNDHOG=ON`, `ninja` (macOS 27 env) | clean; no warnings in touched files |
| `ctest -R '^marmot_\|groundhog-(mls\|privacy-mls\|store\|group-ui\|relay)\|gnostr-test-mls'` (49 tests) | 49/49 pass (`groundhog-store-key-keyring` skipped) |
| `python3 scripts/check-unsequenced-args.py` | pass |
| `scripts/linux-gate.sh --sanitizers` (ASAN+UBSAN+LSan, 52 tests incl. `groundhog-mls-service`, `groundhog-privacy-mls`, `groundhog-mls-ui`) | pass, no sanitizer reports |
| macOS ASAN+UBSAN: `test_commits`, `test_protocol`, `test_adopted`, `test_adopted_commits`, `test_mls_group` (the new parser and Welcome check; the Linux job does not run libmarmot's own tests) | pass (no LSan on macOS) |
| MDK 0.11 live: `control`, `adopted-commits`, `routing-rotation` | pass |
| MDK 0.8 live: all 12 cases incl. `groundhog-requires-self-remove` | pass |
| Revert: `routing_id_held_elsewhere()` always "not held" | `test_protocol` "Welcome refuses held current and alias routing ids" FAIL; `test_adopted_welcome_address_held` FAIL |
| Revert: `url_wants()` asks every read relay for every address (the cancelled draft's shape) | `mls-service/routing-rotation` FAIL at `client_frames_mention(&w.g, new_h)` |
| Revert: no `routing_load()` in `ensure_group()` | `mls-service/routing-retention` FAIL after the restart |
| Revert: `produce_require_self_remove()` returns UNSUPPORTED | `mls-service/self-remove-required` FAIL |
| zbmb probe: `mls_group_create()` with required_capabilities {proposals: 0x000a}, then `mls_group_add_members()` with a re-signed KeyPackage whose leaf lists no proposals | `rc=0`: committed (RFC 9420 §7.3 / §12.2 violated) |

## Summary by focus area

### 1. Privacy of routing (ms4d): holds

**Can an old relay learn the new address? No.**
- `group_subscribe()` (`gh-mls-service.c:2757`) builds one `GhRelayScope` per distinct set of h tags and `since` values that `url_wants()` (`:2705`) gives a URL.
  - A URL is asked for the current id only if it is in the signed current relays.
  - It is asked for an earlier id only if it was in that address's relays.
  - Each URL is in exactly one scope, so a dropped relay never gets a REQ naming the new id.
- The test asserts that no client frame to G mentions the new h. Reverting the rule fails it.
- AUTH stays `GH_AUTH_PURPOSE_MLS_ROUTING` (ephemeral).
- A relay kept across a rotation is asked for both ids. That is inherent: it carries both.

**Publishing goes only to the current routing.**
- Messages (`:4403`, `:4455`) and leaves (`:3556`) use `group->relays`, which for an adopted group is the signed 0x8004 list.
- Commits use `gh-mls-commits.c group_relays()`, which now reads `marmot_get_group_routing()` instead of the relay table that Commits never rewrite.
- A busy store fails the publish instead of falling back to the stale table.
- Groundhog has no producer of routing changes, so `routing_retire()` (called only for inbound Commits) is complete.

**Retention is bounded and survives restarts.**
- `process_event()` opens the `gh_store` transaction before `marmot_process_message()`, and `routing_retire()` (`:1582`) saves the record before `gh_store_commit()`. The record and the Commit therefore land together.
- `ensure_group()` reloads the record.
- `routing_prune()` (`:1652`) drops an address when any of these holds:
  - the group's epoch passes the epoch the address was left at (libmarmot reads one epoch back);
  - 7 days have passed;
  - libmarmot no longer lists the id as previous;
  - the id is current again.
- A failed store commit after `routing_retire()` self-heals at the next refresh.
- The tests cover a rotation mid-conversation, late messages at the old id, a rewrapped duplicate shown once with nothing held, a restart across the rotation, retirement by epoch, and the 7-day bound across a restart with a fake clock.

**Can a malicious admin cause a DoS or an IP leak? Bounded, and no worse than the protocol already allows.**
- Signed relays are capped at 16 (`MARMOT_NOSTR_ROUTING_MAX_RELAYS`), and the retired union is capped at 16.
- Every rotation is a new epoch, which prunes the previous earlier address. So at most one earlier set (16 relays) is read beside the current 16.
- The persisted record is capped at 16 addresses of 16 relays and 64 KiB.
- An admin can point members at relays it runs, which exposes clearnet IPs. That is inherent to admin-chosen routing (as with a legacy relay list), not a regression. Each rotation does cost a re-subscription.

**Tor and network mode are respected.**
- Every scope goes through `gh-relay-net.c open_inner()`: `gh_net_relay_url_allowed()` (ws/wss only, .onion only over Tor, ws:// only to loopback or .onion), plus a per-isolation-label Tor resolver.
- A scope that serves only earlier addresses gets its own label (`"%s-%u"`, `:2821`). On Tor, old relays therefore see a different circuit than the new ones. This is good.

### 2. scki: complete, permanent, mostly honest

- **Coverage.** `routing_id_held_elsewhere()` (`welcome.c:278`) checks the current id (`find_group_by_nostr_id`), then the alias table (`marmot_commit_find_group_by_alias`). That alias table is the one libmarmot routes by, so the check covers exactly the ids that could be misrouted.
  - It covers any record state, ended groups included.
  - A re-invite to the same MLS group is exempt.
  - Aliases older than the 16-entry history are no longer routed, so they cannot collide.
  - Two pending Welcomes with the same address: the second one is refused on accept, once the first is held.
- **Permanence.**
  - Adopted Welcomes: the address is signed, so the Welcome is refused on arrival and recorded as failed (never listed). `test_adopted` then asserts PREVIOUSLY_FAILED.
  - Legacy Welcomes are not opened on arrival. They are listed, then refused for good on accept via `refuse_welcome()`, before any state is written. The Groundhog test asserts the invitation then disappears.
  - Storage errors return the error, not a refusal (fail-closed and retryable).
- **Honesty.** The copy is honest except for N1 below.

### 3. 8ndz automatic upgrade: needs zbmb first (H1, M1, L1)

The libmarmot producer is correct:
- `exts_with_self_remove_required()` bounds-checks every read and keeps every other byte.
- It is refused unless every leaf advertises SelfRemove. The MLS layer re-validates every leaf (`group_context_extensions_validate`).
- The transaction is rolled back on failure.

MDK 0.8 compatibility is real. The live `groundhog-requires-self-remove` case shows:
- MDK 0.8 follows the GroupContextExtensions Commit.
- Its `leave_group()` then emits a SelfRemove PublicMessage, which Groundhog commits.

Choosing *automatic* rather than offering it in Group Info is the problem, for three reasons:

- **Without zbmb the upgrade turns a latent bug into a default-path split (H1).**
- **It makes the admin's own actions fail (M1).**
- **It overrides MDK's deliberate choice.** MDK v0.8.0's CHANGELOG (#261) says: "Empty-invitee ... groups stay permissive with `RequiredCapabilities = []` so a later `add_members` call can admit legacy key packages." MDK 0.11 offers `upgrade_group_capabilities()` only as an explicit API and never calls it on its own. Groundhog upgrades *every* legacy group it administers, MDK-made mixed groups included. After that, an MDK co-admin's attempt to add a legacy KeyPackage fails in OpenMLS ("Proposals are not acceptable"), and nothing tells them why.

My recommendation on the product question: the requirement is worth having, but not as a surprise standalone Commit.
- **New groups:** fold the requirement into the Add Commit that brings the second member, when every leaf including the joiners supports SelfRemove. `mls_group_add_members_with_extensions()` already exists. This is exactly MDK's creation-time LCD rule, applied at Groundhog's first Add. It has no separate Commit, no burst, no BUSY and no admin race.
- **Existing groups:** keep a one-time background upgrade, limited to groups this device created, run only when the group is live and idle, with a long random delay.
- All of this only after zbmb.

**Metadata leak (L1).** The content is an ordinary epoch change. The timing is not:
- After the first launch on this version, every eligible group commits within the departures jitter (1–15 s).
- Under Tor's per-group isolation, that burst on shared relays is the one remaining cross-group correlator.
- It is a one-time event per group.

**Races with other admins (L1).** The jitter window is `1000 + 250·n` ms, which is 1.0–1.5 s for two members. Two Groundhog admins online together will often both commit. One of the two Commits is superseded. That is harmless, but it adds visible kind:445 traffic.

### 4. Harness routing-rotation: a genuine live MDK run

- I built the image from this worktree's `driver-0.11` under a private tag and ran it.
- The peer reports `mdk 0.11.0 (cgka-engine, cgka-session), rev 946e0547`, `openmls 59e7d3b2`, profile marmot-adopted.
- `update_routing` uses MDK's own `NostrRoutingV1::new()` and `encode_nostr_routing_v1()`, through the real engine's `SendIntent::UpdateAppComponents` and `evolve()`, publishing at the prior routing. It is not a forgery.
- MDK's synced state moves from h `ef18cc…` on `:56216` to `ad27f7…` on `:56217`.
- MDK decrypts Groundhog's epoch-3 kind 9 at the new address, and Groundhog reads MDK's.
- The case asserts that Groundhog reads both relays, never publishes the new id to G, and never sends G a frame naming it.
- Limitation: late messages at the old address are covered only by the mls-service case, which uses a forged rotation. That is acceptable.

### 5. Memory safety

The sanitizer gate is green, and so is the macOS ASAN+UBSAN run of the libmarmot tests. On reading the code:
- `exts_with_self_remove_required()` checks every length against what remains, and frees both buffers on every path.
- `routing_id_held_elsewhere()` frees `owner` on all paths.
- `addresses_parse()` bounds the relay count and validates every URL.
- The `Address` and scope lifetimes are correct: `group_unsubscribe()` cancels every scope and clears `url_scope`, and finalize asserts it.

## Findings

### H1 (High, required): the automatic upgrade makes nostrc-zbmb a default-path group split

**Where:** `gnome/groundhog/src/mls/gh-mls-service.c:3428` (`upgrade_wanted`), with:
- `libmarmot/src/mls/mls_group.c:1713` (producer: only `mls_key_package_validate()`);
- `libmarmot/src/mls/mls_group.c:4314` (receiver: the same);
- `libmarmot/include/marmot/marmot.h:1206`. Its doc says "a later invitee's KeyPackage must advertise SelfRemove", a rule libmarmot does not enforce.

**Failure scenario:**
1. Alice (Groundhog 0.12) administers legacy group G with Bob (MDK 0.8, a current White Noise) and Carol (Groundhog). Every leaf supports SelfRemove.
2. Within 1–15 s of launch, Alice's client commits the requirement.
3. Alice invites Dave. Dave's newest KeyPackage lacks SelfRemove. He may be on a White Noise build older than MDK #236 (the legacy KeyPackages MDK's LCD rule exists for, whitenoise-rs #749), or on Groundhog with libmarmot 0.11.0 (whose leaves lack SelfRemove, per `test_commits.c:7489`), or have a stale KeyPackage still on relays.
4. libmarmot commits the Add. My probe confirms `rc=0`.
5. Carol's libmarmot accepts the Add.
6. Bob's OpenMLS rejects the Commit (RFC 9420 §7.3 / §12.2: an Add's leaf must support the required capabilities) and stays at epoch E. Alice and Carol move to E+1. The group is split for good.
7. Dave cannot join from MDK: the Welcome's tree fails validation. If Dave is on Groundhog with libmarmot 0.11, his first UpdatePath Commit is later refused by 0.12 peers (`leaf_node_validate()`). Either way he is a ghost member.

Before this slice, Groundhog-made groups never required SelfRemove, so step 4 was safe.

**Recommendation:** fix zbmb in this slice, before the automatic trigger lands. The fix is small:
- `group_extension_supported()` already exists (`mls_group.c:3220`), and `leaf_node_validate()` already uses it for UpdatePath leaves.
- Run it over the GroupContext extensions for each KeyPackage leaf at `:1713` (refuse with a distinct error and Groundhog copy, e.g. "X's app can't join a group that lets members leave on their own").
- Do the same for each inbound Add at `:4314`.
- Add a test of each direction and an MDK 0.8 check.

**Acceptable alternative:** land the libmarmot API, but default the Groundhog trigger off (or offer it in Group Info) until zbmb merges.

**Coordination:** the brief scopes libmarmot edits to `welcome.c` and the GCE producer. `mls_group.c` is not slice N's `commits.c`, so the zbmb fix should not collide with it.

### M1 (Medium): the background upgrade makes the admin's own changes fail BUSY, with untrue copy

**Where:** `gh-mls-service.c:3465` (`upgrade_fired` → `stage_change`), and `:3220` (`if (group->round || group->pending_commit)` → `GH_MLS_SERVICE_ERROR_BUSY`).

**Failure scenario:**
- Alice makes a group, Bob joins, and 1.0–1.5 s later her client starts the upgrade Commit. Over Tor or slow relays, it stays pending for seconds.
- Alice renames the group or invites Carol right away. She gets "Another change to this group is still being sent. Try again once it's done."
- She made no other change. `db9abf8a`'s own commit message records this: "a rename raced it as BUSY on the Linux gate". The GUI test now waits it out, but users cannot.

**Fix:**
- Preferably, fold the requirement into the Add Commit (see §3), so new groups never have a standalone background Commit.
- Otherwise, let a user change preempt or queue behind an automatic Commit that is still pending, rather than fail. At least, give BUSY honest copy when the pending change is Groundhog's own.

### L1 (Low): the upgrade fires before backfill, races co-admins, and bursts at startup

**Where:** `gh-mls-service.c:3428` (no read-state check) and `:3488` (the departures jitter, `1000 + 250·n` ms, capped at 15 s).

**Failure scenarios:**
- `resume_all()` schedules the upgrade while the group is still SYNCING. A client that was offline commits at a stale epoch. The Commit loses (earlier `created_at` wins) but is still published.
- Two Groundhog admins online together fire inside a 0.5 s window and often supersede each other.
- On first launch after the update, every eligible group commits within about 15 s. Relays shared by several groups see a burst that correlates the groups, even with per-group Tor isolation.

**Fix:** require `GH_MLS_READ_LIVE`, and use a long random delay (minutes to hours; nothing is urgent).

### L2 (Low): relays of an earlier address freeze the read cursor

**Where:** `gh-mls-service.c:2233` (`all_relays_answered()` now iterates `read_relays`), which gates `save_cursor()` at `:2415` and `:2637`.

**Failure scenario:**
- An admin drops relay R from the routing because R is dead (the usual reason).
- R stays a read relay for the old address until the next epoch or 7 days, settled as 2 (failed). The group's cursor stops advancing for that whole time. In a quiet group with no further Commit, that is the full 7 days.
- On every reconnect or network flip, the *current* relays re-send everything since the rotation, up to the page budget. Past that budget, `history-incomplete` is raised falsely.
- An earlier address already has its own fixed `since`, so it needs nothing from the main cursor.

**Fix:** gate the cursor only on the current relays, or on the relays whose scope includes the current h.

### L3 (Low): invite KeyPackage lookups skip only the current relays

**Where:** `gh-mls-service.c:3778` (`op->group->relays`). Verify (`:5160`) uses `read_relays` for the same nostrc-0bdg rule ("never on the group's relays: they would learn whom the group is about to add").

**Failure scenario:**
- A relay the group just dropped, which may be hostile, is still read for the old address.
- If it is also a discovery relay, Alice's invite looks up Dave's KeyPackages there, while her clearnet connection still holds the old-address REQ.
- The relay learns that the group at the old h is about to add Dave.

**Fix:** use `read_relays` at `:3778`, as Verify does.

### N1 (Nit): "another group you're in" is not true for ended groups

**Where:** `gnome/groundhog/src/ui/gh-mls-copy.c:110`.

An ended (left or removed) group's address also counts (`welcome.c:275`). Suggested wording: "…as another group on this device (one you're in or were in)…".

### N2 (Nit): `MARMOT_ERR_PROTOCOL_GROUP_MISMATCH` is mapped to ADDRESS_TAKEN for every operation

**Where:** `gh-mls-service.c:534`.

Today only the Welcome path reaches `marmot_fail()` with this error. The other sources, `commits.c:692` and `:1299`, are inbound Commit judgements. Mapping it in the accept path only would keep the invitation copy from appearing for any future operation.

### N3 (Nit): the tests mostly run with the app's new behaviour off

**Where:** `gnome/groundhog/tests/mls/mls-world.h:63` (`world_self_remove_upgrade` defaults to FALSE).

Most mls-service cases and the MDK 0.8 leave cases now exercise a configuration the app does not run. For example, `mdk-member-leaves` takes the Remove-request path in a two-member Groundhog group that the app would have upgraded within 1.5 s. That is fine for counting Commits, but say so in the harness README. Better still, run one leave case per direction with the upgrade on.

## Related (not this slice)

- nostrc-3ajb: libmarmot refuses an MDK 0.11 admin's GroupContextExtensions upgrade in an adopted group. Until it is fixed, that is a split path for adopted groups.
- nostrc-zbmb: see H1. I recommend raising it to block 8ndz's automatic trigger.

## Addendum: re-review of the fixes (tip `0c5b8ef7`)

- **Rebased on:** master `543ca0b6`.
- **New commits:** `3af3ba3f` (libmarmot: zbmb, and SelfRemove at the first Add), `342e93fb` (Groundhog: the 8ndz product change, M1, the Lows and nits), `0b52b24a` (L2 test hardening, docs, manifest), `0c5b8ef7` (MDK 0.8 `groundhog-leaves-mdk-admin` keeps a permissive group).
- **Final verdict:** **APPROVE-WITH-NITS.**
  - H1 and M1 are fixed and tested.
  - L1, L2, L3 and N1–N3 are fixed.
  - Two residual Lows (R1, R2) and one Nit (R3) remain. None blocks the merge.

### Verification

| Check | Result |
|---|---|
| `ninja` (macOS 27 env, `-DBUILD_GROUNDHOG=ON`, MDK harnesses on) | clean |
| ctest: libmarmot, plus Groundhog mls / privacy-mls / store / group-ui / relay / mls-ui / mls-media, plus gnostr mls (49 tests, `-j6`) | 48 pass, 1 skipped (keyring). `marmot_test_adopted_commits` aborted once. That is the known wall-clock flake nostrc-12m0 (the test is untouched here): 5/5 sequential and 60/60 six-way-concurrent reruns pass |
| `scripts/linux-gate.sh --sanitizers` (52 tests) | pass |
| macOS ASAN+UBSAN: `test_mls_group`, `test_commits`, `test_protocol`, `test_adopted`, `test_adopted_commits` | pass, no reports |
| MDK 0.8 live (13 cases, incl. `mdk-member-self-removes` and `groundhog-requires-self-remove`) | 13/13 pass |
| MDK 0.11 live (private tag built from this tree, since removed): `control`, `white-noise-welcome`, `adopted-commits`, `routing-rotation` | pass. `groundhog-invites-mdk` is its documented XFAIL (`unsupported`, nostrc-qp24.5.1) |
| **The split probe, rerun** (`zbmb_probe.c`, unchanged) | `rc=-54` (`MARMOT_ERR_KEY_PACKAGE_CAPABILITIES`): refused. It was `rc=0` before |
| Revert: the producer check in `add_members_staged()` (`mls_group.c:1722`) | `test_add_refuses_unsupported_key_package` FAIL |
| Revert: the receiver check in `process_commit_impl()` (`mls_group.c:4305`) | `test_inbound_add_of_unsupported_key_package_refused` FAIL |
| Mutation: `created_here()` true for every group with a record (joined ones too) | **no test fails** (R2) |
| Docker | volumes identical before and after (268) |

### Disposition of the findings

**H1, zbmb: fixed in both directions.**
- **Producer:** `add_members_staged()` checks every joiner with `key_package_supports_group()`, which reuses `group_extension_supported()`.
  - It checks against the GroupContextExtensions of this Commit when it has them, so an Add made together with a GCE is checked against the new list.
  - It refuses with the new `MARMOT_ERR_KEY_PACKAGE_CAPABILITIES` (-54) before anything is staged.
  - The legacy and adopted producers both map that error through.
  - Groundhog shows it as `GH_MLS_SERVICE_ERROR_INVITEE_UNSUPPORTED`, with honest copy ("…someone you invited uses an app that can't, so they can't join it. Nothing was changed.").
- **Receiver:** the pre-application validation pass refuses an inbound Add whose leaf does not support the epoch being entered (the last GCE proposal's list).
- Both directions are tested, and each test fails when its check is reverted (above).
- The `marmot.h` doc now describes a rule that libmarmot enforces.

**8ndz product change: as asked, with one inference caveat (R1).**
- **SelfRemove at the first Add.** `first_add_requires_self_remove()` (`groups.c`) folds the requirement into the Add Commit when all of these hold:
  - the group is legacy;
  - SelfRemove is not yet required;
  - only our leaf is occupied;
  - our leaf and every invitee advertise SelfRemove.
  
  This is MDK's creation-time LCD rule, applied at the first Add, with no separate Commit. A legacy invitee keeps the group permissive. `MarmotConfig.keep_first_add_permissive` (appended field, unreleased 0.12.0) opts out. MDK 0.8 follows the combined Add+GCE Commit live (`mdk-member-self-removes`).
- **Background upgrade.** Now only when `created_here()` is true, and only when `caught_up()`: the group is read live, every current relay has answered, and no backfill is pending or queued. The delay is a random 10 min to 6 h. A service-wide `upgrade_slot` keeps two groups' upgrades at least 30 min apart. `test_background_upgrade` covers the stagger. This also settles the timing burst and the stale-epoch Commit from L1. The co-admin race is now limited to groups we created, at a random time in a 6 h window.
- **The "created here" inference.**
  - **Groups created or joined by this build:** `set_origin()` writes `mls/o/<hash>`: 1 on create (`:4140`), 2 on accept (`:5279`). A joined group is therefore never marked created. Groups our account's *other* device created count as joined on this device. This is correct.
  - **Groups older than the record:** a fallback applies, and it can misfire (R1).

**M1: fixed.** `stage_change()` (`:3386`) queues the admin's change behind a Commit Groundhog made on its own (the `auto_change` flag, set for the upgrade and for automatic departures). It no longer returns BUSY. `queued_flush()` stages the queued changes in order once that Commit's round is reported and nothing is pending. Each queued change is re-checked with `check_change()` first, and a cancelled one is skipped. `stop_generation()` fails them as cancelled. `test_background_upgrade` renames while the upgrade is out, and the rename lands right after it. A user's own change still gets BUSY behind the user's own pending change, which is correct.

**L1: fixed** (see above).

**L2: fixed.**
- `all_relays_answered()` and the read state now follow the current relays only. An earlier address keeps its own `since`.
- In addition, a relay read only for an earlier address is dropped after 3 failures in a row. The counter resets on every re-subscription, so this is best-effort. The cursor fix is the one that matters.
- `routing-dead-earlier-relay` first makes that relay silent, and asserts the cursor still moves, then kills it.

**L3: fixed.** Invite KeyPackage lookups now avoid `read_relays` (`:4051`), as Verify does.

**N1: fixed.** The copy now reads "another group on this device (one you're in or were in)".

**N2: fixed.** ADDRESS_TAKEN is mapped only on invite-accept (`:5239`).

**N3: fixed.**
- The tests now run the app's behaviour by default.
- `world_permissive_groups` is set only by the two Remove-request cases (`mdk-member-leaves`, `groundhog-leaves-mdk-admin`), and the harness README says why.

### Residual findings

**R1 (Low): the fallback for groups without an `mls/o/` record can mark a joined group "created here".**

**Where:** `gnome/groundhog/src/mls/gh-mls-service.c:3631` (`created_here()`, the record-less branch).

The fallback says "created here" when *our account* holds leaf 0 and no member is `welcome_signer`. Two things make that unreliable:
- `welcome_signer` is true only while the welcomer's *device* is still at its leaf with the same key (`marmot-members.h:48`). It turns false once the welcomer leaves or rotates its key.
- libmarmot has recorded it only since `94062fdb` (2026-09-30). Every earlier join has none.

**Failure scenario:**
1. MDK user M creates a group, then leaves. Leaf 0 is now blank.
2. Admin A adds us before this build. The Add takes the leftmost blank leaf, so we get leaf 0.
3. Our join predates the `welcome_signer` record, or A later leaves.
4. After updating, we are an admin, and every leaf supports SelfRemove. `created_here()` returns TRUE.
5. Groundhog commits the background upgrade in a group someone else made. That is exactly what the integrator excluded.

The same happens when *another device of our account* holds leaf 0: the check compares the account, not this device.

**Impact:** Low. Encrypted groups are a development-only build option (`GH_FEATURE_ENCRYPTED_GROUPS`, OFF), so only existing dev installs hold record-less groups. The outcome is a GCE Commit that every member supports. Because zbmb is fixed, later legacy invitees are refused with honest copy rather than splitting the group.

**Fix:** drop the fallback and treat a record-less group as "joined" (conservative; no shipped install has such groups). Or at least compare this device's leaf (its signature key) instead of the account, and persist the inferred origin.

**R2 (Low, test gap): nothing tests that a joined group never gets the background upgrade.**

Making `created_here()` return TRUE for every recorded group (joined included) passes `groundhog-mls-service` and all 13 MDK 0.8 cases. The window is shortened only where a test sets it, and no case has Groundhog as an admin of a group it joined, with the window shortened.

**Fix:** add one case. An MDK- or Groundhog-made permissive group makes us an admin, with the window at milliseconds. Assert that no GCE Commit comes from us, while the creator's own upgrade still does.

**R3 (Nit): a test hook is compiled into the production library.**

**Where:** `libmarmot/src/mls/mls_group.c:1656`. `mls_test_allow_unsupported_adds` is a writable global in every build, declared only in the internal header. It defaults to false and no production code sets it. Even so, the switch that turns zbmb back off is better compiled out, e.g. behind `MARMOT_TEST_HOOKS` as the other test entry points are.

**Observation (not a regression).** Suppose an automatic Commit's round finishes with no relay answering (republished later), and meanwhile an inbound Commit supersedes it. Then the changes queued behind it wait until a later round of ours is reported, or the service stops. This is the existing "unanswered rounds wait" semantics: a user's own change in the same position would wait the same way.
