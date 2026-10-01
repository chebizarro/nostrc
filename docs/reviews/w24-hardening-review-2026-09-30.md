# Review: `marmot/w24-hardening` (W24 slice C: nostrc-dkiq, nostrc-w285, nostrc-c7ho, nostrc-2lrz)

- **Reviewer:** independent peer review (AGENTS.md), read-only detached worktree at `81efdc50`
- **Commit:** `81efdc50` fix(libmarmot): W23 MDK review hardening (libmarmot 0.11.0 -> 0.12.0, groundhog 0.11.2 -> 0.11.3)
- **Base:** `a07dd1c6` (master is now `82a615e4`, which adds one docs-only commit)
- **Brief:** `/tmp/w24/C.md`. **Background:** `docs/reviews/w23-mdk-interop-review-2026-09-30.md` (M1, L1-L3)

## Verdict: **CHANGES-REQUIRED**

There is no Blocker or High. The security-relevant parts hold:
- A network peer cannot reach the libmarmot 0.10.0 decoder on bytes it chose.
- The new PrivateMessage-Commit tests exercise the real `marmot_process_message()` → `private_commit_open()` path. Mutation shows they catch the signature/wire-format, AEAD, padding, ratchet-restore and account-proof checks.
- A peer can push our created_at floor at most 60 s ahead of our clock. The push does not accumulate across Commits.

Three Medium findings each break a guarantee this commit claims, and each has a concrete failure that I reproduced on `81efdc50`:

- **M1 (c7ho):** adding a member to a group whose stored GroupData is still in the 0.10.0 layout yields a Welcome that every 0.12 joiner refuses for good. The joiner's leaf stays in the tree, but nobody holds it. With 0.11's Welcome decoding the same run joins.
- **M2 (2lrz):** our own events push created_at ahead of the clock without bound: 300 messages leave the next Commit 302 s in the future, and the lead persists across restarts. The 60 s cap applies only to peers.
- **M3 (w285):** a transient storage error while reading the KeyPackage private key still refuses the invitation permanently. The commit message, README and manifest all say only missing raw data does.

Each fix is small and local (see the findings). Five Low and three Nit items follow.

## Gates run (macOS 27, `source /tmp/nostrc-macos27-env.sh`)

| Gate | Result |
|---|---|
| `git submodule update --init --recursive`; `cmake -G Ninja -DBUILD_GROUNDHOG=ON`; `ninja` | PASS (2464 steps; only the existing "duplicate libraries" linker warning) |
| `python3 scripts/check-unsequenced-args.py` | PASS ("No call modifies and uses a variable in different arguments") |
| `ctest -R 'marmot\|groundhog'` | **95/95 passed** (skipped: groundhog-launch, -store-key-keyring, -background-gui, -notifier-gui; environmental) |
| `ctest` (full) | **444/444 passed** (skipped: test_nip5f_tcp plus the four above) |
| `scripts/linux-gate.sh --sanitizers` (on a second clean detached worktree at `81efdc50`) | PASS: "sanitizer tests passed, 49 run". It reused the existing `nostrc-linux-gate-asan-arm64` volume and created none |
| Host ASAN+UBSAN build (`-fsanitize=address,undefined -fno-sanitize-recover=undefined`) of `test_commits` (the 5 new tests), `test_protocol`, `test_marmot_interop` | PASS, no reports. The CI sanitizer job does not run these libmarmot binaries, so I ran them here |
| MDK vectors | `test_marmot_interop` finds `tests/vectors/mdk`. The message-protection case prints "application_priv and commit_priv unprotected and their signatures verified". `test_group_data_extension_version_1` runs |

### Mutation spot-checks (each applied alone, test binaries rebuilt, file restored with `git checkout`)

| # | Mutation | Result |
|---|---|---|
| D1 | No signature check for PrivateMessage Commits (`mls_group.c:3070`) | **Caught**: "sender data naming another member: Charlie got 0" and "refused Commit 1" |
| D2 | Header epoch check removed (`private_message_sender`, `mls_group.c:2942`) | **Survives** (all three PrivateMessage tests pass) |
| D3 | FramedContent epoch check removed (`commit_authenticate`, `mls_group.c:3064`) | **Survives** |
| D4 | D2 + D3 together | **Caught**: "a PrivateMessage Commit of another epoch" |
| D5 | Blank-leaf check removed (`mls_group.c:2954`) | Survives (the author documents it as layered) |
| D6 | Ratchet restore removed (`private_commit_open`) | **Caught**: "the honest Commit after a refused one" |
| D7 | Padding check removed (`mls_framing.c:1469`) | **Caught**: test_commits "non-zero padding" and the commit_priv interop assert |
| D8 | `from != sender_leaf` removed (`mls_group.c:3054`) | **Survives** |
| D9 | Leaf binding (account proof) disabled (`commits.c:169`) | **Caught**: "Add of Victor with Alice's proof: Bob got 0" |
| C1 | `group_data_of` uses the stored (0.10.0-tolerant) decoder for every GroupData | **Caught**: "a Commit's 0.10.0 GroupData is refused" |
| C2 | `group_data_of` never uses the stored decoder | **Caught**: authorize returns -84 for the unchanged legacy GroupData |
| C3 | Public decoder falls back to 0.10.0 again | **Caught** (test_commits and interop) |
| C4 | Welcome decodes GroupData with the stored decoder (`welcome.c:627`) | **Survives** (test_protocol and all of test_commits pass) → L2 |
| C5 | MIP-01 v1 short form refused | **Caught** (interop version_1) |
| W1 | Any welcome_data load error refuses for good (pre-fix) | **Caught** |
| W2 | `save_welcome` result ignored (pre-fix) | **Caught** |
| T1 | Observing a peer's Commit time removed | **Caught**: "Bob's Commit after Alice's" |
| T2 | No reservation (created_at = now) | **Caught** |
| T3 | Application message dated `marmot_now()` | **Caught** |

## Focus-area assessment

### c7ho: old layout only when byte-identical to stored state
- **Can a network peer trigger the old decoder? No.**
  - `marmot_group_data_extension_deserialize()` is MIP-01 only (`extension.c:388`).
  - `group_data_of(g, stored)` (`commits.c:92-109`) runs the 0.10.0 decoder only when `g` *is* `stored`, or when `g`'s single GroupData is byte-identical to `stored`'s single GroupData.
  - Every `stored` argument is state loaded from our storage:
    - `marmot_commit_authorize()` callers: `commits.c:1330` (the loaded `pre` of our own Commit), `:1394` (loaded `cur` against our own stored pending post), `:1501` (`stage_inbound`'s `parent`: the loaded current state or the retained parent).
    - `commits.c:373, 809, 1558, 1686`.
    - `groups.c:1039` and `:1249`, which also read our own state.
  - The Welcome uses the strict decoder (`welcome.c:627`).
  - By induction, legacy bytes in any stored state came from a libmarmot ≤ 0.10.0 of ours. A peer can only make us re-run the old decoder on bytes we already hold, by leaving GroupData untouched, which reproduces what we already parsed. ✔
- **New ambiguity from the v1 short form? No.** libmarmot always wrote version 2 in its own layout (`MARMOT_EXTENSION_VERSION 2` since `8a09026c`), and the short form applies only to `version == 1`. MDK 0.8 serializes `image_upload_key` unconditionally, as an empty `Vec` for v1 (`mdk-core/src/extension/types.rs` at v0.8.0 `575ae29d`), which matches the author's "both encodings". ✔
- **Can a legitimate post-Commit update from an old group be wrongly rejected?**
  - Commits that leave GroupData unchanged are authorized (test, plus mutation C2).
  - A rename or other metadata Commit by a 0.11/0.12 member re-encodes MIP-01 (`updated_group_data` decodes with `_stored` and serializes MIP-01). Receivers authorize it with `before` read by the legacy decoder. The nostr_group_id-immutability and admin checks compare decoded values. ✔
  - A Commit that *writes* the 0.10.0 layout is refused. Only a ≤ 0.10.0 client would send one, and 0.11 is already wire-incompatible with those. ✔
  - **The Add of a new member is broken (M1).** It is the one legitimate flow that fails.

### 2lrz: peer influence on our clock
- **No accumulation across Commits.** `marmot_observe_group_event_time()` (`marmot.c:146-156`) clamps a peer's created_at to *our* `now + 60` at observation time and only ever raises the floor. N hostile Commits dated arbitrarily far ahead therefore leave the floor at most 60 s ahead of our clock. Without new input the lead decays at 1 s/s. ✔
- **Our own events do accumulate** (+1 s per event, no cap) → M2.
- **Clock skew.** The cap is relative to our clock. A member whose clock is more than 60 s slow, or a committer that dates its Commit more than 60 s ahead, still gets epoch-(n+1) events from us dated before Commit n+1. This is an accepted trade-off rather than a regression: before 2lrz those events were dated at our clock anyway.
- **Privacy and timing:** see L4. The incremental leak is modest, but two new public signals appear.
- **Transactions:** the reservation and the observation are written inside the operation's storage transaction. A failed `observe` returns the error and frees the result, and the transaction rolls the Commit back, so the event is retried rather than lost (`messages.c:963-970`). The Groundhog snapshot excludes the label, and the store test checks that a rolled-back Commit leaves it raised. ✔
- `nostr_group_id` is immutable after creation (`commits.c:266-268`), so keying the floor on it is sound.

### w285: error classification
- The `welcome_data` read (`welcome.c:478-486`) is right: only `MARMOT_ERR_STORAGE_NOT_FOUND` refuses. `refuse_welcome()` now returns the `save_welcome` error and records nothing. `record_welcome_failure` keeps the transaction only if its own write succeeded, so a failed save leaves the Welcome pending. ✔
- **Two gaps:**
  - The KeyPackage lookup in the same function still turns any storage error into a permanent refusal (M3).
  - Two in-tree backends report I/O and allocation failures as NOT_FOUND (L1).
- Every other refusal reason is permanent by nature: MLS decode, MLS processing with the right KeyPackage, account-proof binding (`welcome_tree_bound` is pure computation), and GroupData format. ✔

### dkiq: do the negative tests exercise the PrivateMessage path?
- **Yes.** `private_commit_bytes()` builds a real `MLSMessage{PrivateMessage}`:
  - content_type `commit`;
  - sender data under `sender_data_secret`;
  - content under the sender's handshake ratchet;
  - PrivateMessageContent = Commit ‖ signature ‖ confirmation_tag ‖ zero padding;
  - FramedContent signed with wire format `mls_private_message`;
  - a confirmed transcript, key schedule and confirmation tag recomputed for a pathless Add.
- `private_commit()` wraps it in a MIP-03 kind:445 through `marmot_commit_build_event()`, and `expect_rejected()` feeds it to `marmot_process_message()`. That function routes via `is_handshake()` → `marmot_commit_process_inbound()` → `stage_inbound()` → `mls_group_process_commit()` → `commit_authenticate()` → `private_commit_open()`. `expect_rejected` checks both the exact error code and that MLS state, retained parent, epoch, name and next-epoch exporter secret are byte-identical afterwards. ✔
- The positive test applies the honest Commit at handshake generation 3 on two receivers and checks convergence and message flow. This proves the builder is faithful, so each negative differs from an accepted message by one property. ✔
- The OpenMLS `commit_priv` vector is checked with the same primitives `private_commit_open()` composes: sender data, `mls_private_message_decrypt_with_sender_data`, `mls_handshake_content_decode`, and wire-format-bound signature verification. It also checks that the content layout matches what test_commits.c builds. ✔
- **Where the "fails when its check is removed" requirement is not met:** single epoch checks, `from != sender_leaf`, the blank leaf, a past-epoch PrivateMessage Commit, and generation reuse being shown only synthetically. See L5.

## Findings

### M1 (Medium): Adding a member to a 0.10.0-layout group yields a Welcome every 0.12 joiner refuses for good
**Where:**
- Inviter: `libmarmot/src/groups.c:1035-1039`. `add_members_impl` reads the legacy GroupData with `marmot_group_data_extension_deserialize_stored` ("our own state: a 0.10.0 group's layout too") and ships the GroupContext unchanged in the Welcome.
- Joiner: `libmarmot/src/welcome.c:623-632`. The decode is MIP-01 only, then `refuse_welcome()`.

**Scenario (reproduced with a throwaway test on `81efdc50`):**
1. Start from a trio whose stored GroupData is in the 0.10.0 layout, as with any group made by libmarmot ≤ 0.10.0 that has had no metadata Commit since. Application messages still flow.
2. Alice calls `marmot_add_members(Dave)` → `0`, 1 Welcome. Bob applies the Add → `MARMOT_RESULT_COMMIT`.
3. Dave calls `marmot_process_welcome` → `0`, then `marmot_accept_welcome` → **`-84 MARMOT_ERR_EXTENSION_FORMAT`**. His pending list is now **0**: the Welcome is recorded FAILED.
4. Dave's leaf stays in every member's tree, held by nobody, until an admin removes it. The admins believe Dave was added.

With only `welcome.c:627` switched back to the stored decoder (0.11 behaviour), the same run ends `accept_welcome → 0`. This is therefore a regression in 0.12, for exactly the groups the commit message says still work ("so legacy groups still follow ordinary Commits"; the 0.11.0 README says "existing groups load").

Refusing a network Welcome in the 0.10.0 layout is what the brief asks for. The defect is that the inviter still produces one. The affected population is small (libmarmot ≤ 0.10.0 and encrypted groups were never released), but the result is a dead leaf and a lost invitation.

**Fix:** when the stored GroupData is not MIP-01, do one of the following:
- (a) fold a GroupContextExtensions proposal into the Add Commit that re-encodes the same GroupData as MIP-01 v2 (with required_capabilities). Receivers already authorize that: `before` is read by the legacy decoder, `after` is MIP-01, and the committer must be an admin, which an Add needs anyway.
- (b) refuse `marmot_add_members()` with `MARMOT_ERR_EXTENSION_FORMAT` before creating anything, so the app runs a metadata update first.

Test the Add on a legacy group end to end, and see L2.

### M2 (Medium): Our own kind:445 events drive created_at ahead of the clock without bound
**Where:** `libmarmot/src/marmot.c:124-138`. `marmot_next_group_event_time` sets `next = max(now, last + 1)` with no cap. Every application message reserves one (`messages.c:663`). `GROUP_EVENT_MAX_LEAD` (`marmot.c:143`) bounds only `observe`.

**Scenario (reproduced with a throwaway test):**
- Alice sends 300 application messages to a group in under a second. The last carries created_at **301 s** ahead of her clock, and her next Commit **302 s** ahead.
- Any sustained rate above 1 kind:445/s per group grows the lead by (rate − 1) s every second. Sources include a bot, a bridge, an outbox flushing a backlog after reconnecting, or reactions and receipts sent as application messages.
- The floor is persisted, so a restart does not reset it.
- Once the lead passes a relay's future-timestamp tolerance (strfry's default `rejectEventsNewerThanSeconds` is 900), relays reject every kind:445 the member publishes to that group until the wall clock catches up. That includes the pending Commit, whose signed event is stored and republished with that created_at.

The code comment ("a burst of events in one second runs ahead one second per event") acknowledges the drift but does not bound it.

**Fix:** pick one.
- (a) Keep the brief's "strictly increasing for everything" but bound it: when `last + 1 > now + GROUP_EVENT_MAX_LEAD`, return a retryable error (or make the caller wait) instead of dating further ahead.
- (b) Only Commits, ours and observed, raise the floor, and application messages are dated `max(now, floor + 1)` without raising it. The MDK failure mode is purely about epoch order: a Commit n+1, or a message of epoch n+1, read before Commit n. Ties among one epoch's application messages are harmless, because MDK reads those out of order. Option (b) relaxes the brief, so it needs the brief owner's agreement.

Add a test that a burst of N ≫ 60 messages leaves the next Commit within the bound.

### M3 (Medium): A transient storage error reading the KeyPackage private key still drops the invitation for good
**Where:** `libmarmot/src/welcome.c:514-520`. Any non-zero `mls_load("kp_priv")` counts as "not this KeyPackageRef", and the result is the `refuse_welcome(…KEY_NOT_FOUND)` at `:569-572`. The same happens at `:536-559` (`kp_full`): any error takes the "minimal KP" fallback, whose ref cannot match, and the result is `refuse_welcome(…MLS)` at `:586-587`.

**Scenario (reproduced with a throwaway test):**
1. Bob's `mls_load` fails once with `MARMOT_ERR_STORAGE`, for label `kp_priv` only.
2. `marmot_accept_welcome()` → **`-49 MARMOT_ERR_KEY_NOT_FOUND`**. The Welcome is saved FAILED with "matching KeyPackage private key not found", and `marmot_get_pending_welcomes()` returns **0**.
3. `marmot_accept_welcome_by_wrapper_id()`, which marmot-gobject uses, searches only pending Welcomes, so the invitation is gone even after storage recovers.
4. A retry with the stale in-memory Welcome object then *succeeds*, which shows the refusal was wrong.

This is the class w285 targets, about 40 lines below the read it fixed. The commit message, README and VERSION_MANIFEST all say "refuses a Welcome for good only when its raw data is missing", and that is not true. It is less likely than the first read failing, because it is the second read in the same transaction, but nothing rules it out: IOERR, NOMEM, Groundhog `check_readable`.

**Fix:** treat only `MARMOT_ERR_STORAGE_NOT_FOUND` as "not ours". Return any other error, so the Welcome stays pending and the transaction rolls back. Apply the same rule to the `kp_full` fallback. Extend `test_accept_welcome_storage_failures` with label-selective faults on `kp_priv` and `kp_full`.

### L1 (Low): w285's classification relies on NOT_FOUND discipline that two in-tree backends lack
**Where:**
- `libmarmot/src/storage_sqlite.c:1309-1310`: every `sqlite3_step` result other than `SQLITE_ROW` (BUSY, LOCKED, IOERR, NOMEM, CORRUPT, …) becomes `MARMOT_ERR_STORAGE_NOT_FOUND`.
- `libmarmot/src/storage_nostrdb.c:1554-1563`: an `mdb_get` error or a failed `malloc` becomes NOT_FOUND.

**Scenario:** marmot-gobject (and so gnostr) uses the sqlite backend (`marmot-gobject-storage.c:222`). An I/O error or OOM on the `welcome_data` read is still reported as "missing", and the invitation is refused for good. `BEGIN IMMEDIATE` makes BUSY unlikely inside the transaction, so in practice this means IOERR and NOMEM.

The new test uses only the memory backend, which is correct. The backend behaviour predates this commit, but 0.12's contract now depends on it.

**Fix:** sqlite: `SQLITE_DONE` → NOT_FOUND, anything else → `MARMOT_ERR_STORAGE`. nostrdb: `MDB_NOTFOUND` → NOT_FOUND, other codes → STORAGE, OOM → MEMORY. Add a storage-contract test.

### L2 (Low): No test pins the Welcome to the strict decoder
**Where:** `libmarmot/src/welcome.c:627`.

**Scenario:** mutation C4 (decode the Welcome's GroupData with `marmot_group_data_extension_deserialize_stored`) passes test_protocol and all of test_commits. The Welcome is the network input c7ho mainly targets, so a later refactor that "helpfully" uses the stored decoder there would go unnoticed.

**Fix:** add a Welcome-level negative: a GroupContext carrying 0.10.0-layout GroupData → `MARMOT_ERR_EXTENSION_FORMAT`. M1's fix needs the same fixture.

### L3 (Low): A joiner's first events are not ordered after the Commit that added it
**Where:**
- `messages.c:963-965`: the floor is raised only by processed Commits.
- `welcome.c` (accept) does not seed it.
- `groups.c:296`: the Welcome rumor is dated `time(NULL)`, not with the Add Commit's reserved created_at.

**Scenario:**
1. The inviter's floor is ahead of its clock, because of its own burst (M2) or because a peer pushed it by up to 60 s.
2. The Add Commit is therefore dated up to a minute ahead.
3. Dave joins and says "hi" within that window.
4. His epoch-(n+1) kind:445 is dated *before* the Commit that opens epoch n+1. An MDK 0.8 member reading in created_at order fails Dave's message and never retries it, which is exactly the failure 2lrz is about.

**Fix:** date the Welcome rumors with the Add Commit's reserved created_at, and on accept seed the joiner's floor from the rumor's created_at (clamped like `observe`).

### L4 (Low): 2lrz adds public timing metadata
**Where:** `marmot.c:124-156`.

Two new public signals. Both are visible to non-members (relays, or anyone fetching the group's kind:445 by `h` tag):
1. **Sender chains.** After a burst, a member's later events carry `previous + 1` and run ahead of their arrival time, while other members' events carry roughly the arrival time. An observer can link that member's events for as long as the lead lasts. Per-event ephemeral keys (MIP-03) exist to stop observers linking a sender's events.
2. **Epoch markers.** A Commit dated ahead (by a hostile or skewed member) is followed by `X+1, X+2, …` from every member that applied it and posts within the window. Observers learn which events were sent after that Commit, and roughly when each member caught up.

The incremental leak is modest. A relay usually links events by connection anyway, and created_at already carried a stable per-device clock offset before 2lrz. A hostile member gains nothing it does not already have as a member, and its push is bounded and identical for all members.

**Fix:** bounding our own lead (M2) shrinks both signals. Document the trade-off in the README's privacy notes.

### L5 (Low): Several single PrivateMessage checks can be removed without a test failing; the bead overstates mutation coverage
**Where:** `libmarmot/src/mls/mls_group.c:2942, 3064, 3054, 2954`. Tests at `libmarmot/tests/test_commits.c:4688-5058`.

**Scenario:**
- **Epoch checks.** Removing the header epoch check (`:2942`) *or* the FramedContent epoch check (`:3064`) alone passes all three PrivateMessage tests; only removing both fails. `mls_handshake_content_decode` rebuilds the FramedContent epoch from the header, so the two checks are one value by construction. The bead's "both MLS epoch checks each make a test fail" therefore cannot hold.
- **`from != sender_leaf` (`:3054`) survives.** Every caller derives `sender_leaf` from the same sender data. An MLS-layer call whose `sender_leaf` disagrees with the sender data would pin it.
- **Blank leaf (`:2954`) survives.** This is documented as layered behind the leaf-type check before signature verification.
- **Untested: a PrivateMessage Commit of an *earlier* epoch** (`epoch_skew = UINT64_MAX`). That is the realistic replay, and it goes down the retained-parent path.
- **Generation reuse is shown only synthetically.** The test reads the secret tree directly, and `private_commit_open()` never consumes handshake keys, so no production path reaches the guarded state. That is fine as an MLS-layer contract test, but it is not a replay test.

The brief's "each test must fail when its check is removed" is met for: signature/wire format, AEAD tag, padding, ratchet restore, account-proof Add, and epoch (as a pair).

**Fix:** add the past-epoch case and a `sender_leaf`-mismatch MLS-layer case, and correct the bead note.

### N1 (Nit): Manifest rows are outside the decision table
`VERSION_MANIFEST.md:163-165`. The three decision rows were appended after the "## Maintenance" bullet list, not to the table that ends before it (row at line 143). They render as text continuing the last bullet, outside the ledger the planned `scripts/tag_release.py` will read. Move them above "## Maintenance".

### N2 (Nit): A malformed floor row blocks a group for good
`libmarmot/src/marmot.c:102`. A `group_event_created_at` row that is not 8 bytes makes every `marmot_next_group_event_time` return `MARMOT_ERR_STORAGE`, so every send and Commit to that group fails and nothing repairs it. Treat a malformed row as absent (start from `now`) and overwrite it.

### N3 (Nit): `marmot_commit_build_event()` now bypasses the floor
`libmarmot/src/commits.c:881-888`. It survives as a wrapper dated `marmot_now()`, and no production code calls it any more (only test_commits.c). Remove it, or rename it so that skipping the floor is explicit.

## Versions
- **libmarmot 0.11.0 → 0.12.0 (MINOR):** correct under AGENTS.md's 0.x rule (a public decoder's accepted input narrowed, an error code changed). CMake, Meson and the manifest agree.
- **groundhog 0.11.2 → 0.11.3 (PATCH):** a comment, a label classification and a test. Conservative, given that earlier rows left Groundhog unbumped behind `GH_FEATURE_ENCRYPTED_GROUPS=0`, and in line with the brief.
- **marmot-gobject and gnostr:** no bump. This is justified: there is no source change, both link statically, both are unreleased, and the error-code change is noted.

The rows are misplaced (N1).

## What is good
- `group_data_of(g, stored)` is a minimal, auditable rule ("bytes we already hold"). It is tested in both directions.
- The PrivateMessage builder honestly recomputes the transcript, key schedule and confirmation tag. The positive test proves it faithful, and each negative changes one property and checks state is unchanged, not just the error.
- The Groundhog store test checks that a rolled-back Commit leaves the floor raised, which is the subtle property.
- The w285 refusal path is now consistent: a refusal is either recorded completely or not at all.
- The follow-ups are filed: nostrc-x215 (v1 image_key semantics), nostrc-qfer (upstream MDK retry), nostrc-d19g (stale floor rows).

---

## Addendum: re-review of the follow-ups (tip `d58add27`)

**Commits reviewed:**

| Commit | Addresses |
|---|---|
| `6ade03b3` | M2, L3, L4, N2, N3 |
| `cb9d5f15` | M1, L2 |
| `151606a2` | M3, L1 |
| `ddc33101` | L5 |
| `08330bce` | N1 |
| `d58add27` | M2 test |

This review branch is rebased onto `d58add27`.

### Final verdict: **APPROVE-WITH-NITS**

All three Medium and all five Low findings are resolved, and so are N1-N3. I checked each fix by reverting it on its own and confirming that a test fails (table below). The one exception is a check that is redundant by construction, as the author notes.

The new `MARMOT_ERR_EVENT_RATE` refusal is safe: nothing is created, and in practice it lasts about a second. There is one new Low:

- **L6:** a floor left far ahead by our own clock that was later corrected blocks Commits until real time catches up.

L6 is a narrow trigger with a three-line fix. It should be fixed, or filed as a bead, before `GH_FEATURE_ENCRYPTED_GROUPS` is turned on, but it does not block this merge. There are also three Nits (N4-N6).

### Gates at `d58add27` (macOS 27, `source /tmp/nostrc-macos27-env.sh`)

| Gate | Result |
|---|---|
| `ninja` (incremental, review worktree) | PASS |
| `python3 scripts/check-unsequenced-args.py` | PASS |
| `ctest` (full) | 443/444. `gnostr-test-event-item-txn-budget` aborted once while the Docker sanitizer gate ran in parallel; it passed 3/3 when re-run alone. It is a gnostr transaction-budget timing test, and this branch makes no gnostr change. All marmot and groundhog tests passed, including `groundhog-mls-service` with the new `/groundhog/mls-service/burst-then-commit-accepted`: 90 messages and a Commit through a relay that refuses events more than 60 s ahead, none refused |
| Host ASAN+UBSAN, out-of-tree, from a clean worktree at `d58add27`: `test_commits` (all), `test_protocol`, `test_marmot_interop`, `test_storage_contract` | PASS, no reports. The back-to-back-Commit loop reaches `EVENT_RATE` even under ASAN's slowdown |
| `scripts/linux-gate.sh --sanitizers` (clean detached worktree at `d58add27`) | PASS: "sanitizer tests passed, 49 run". It reused the existing `nostrc-linux-gate-asan-arm64` volume, after waiting for another agent's gate on it, and created none |

### Mutation spot-checks at `d58add27` (each applied alone, test binaries rebuilt, file restored)

| # | Mutation | Result |
|---|---|---|
| D1 | No signature check for PrivateMessage Commits | Caught: forgeries, epoch_and_generation, and checks_alone ("signed by Charlie, processed as Alice's") |
| D2 | Header epoch check alone (`private_message_sender`) | **Caught**: checks_alone "a PrivateMessage header of another epoch names no sender" (survived at `81efdc50`) |
| D3 | FramedContent epoch check alone (`commit_authenticate`) | **Caught**: checks_alone, PublicMessage case (survived at `81efdc50`) |
| D5 | Blank-leaf check alone | **Caught**: checks_alone "sender data naming a blank leaf names no sender" (survived at `81efdc50`) |
| D6 | Ratchet restore removed | Caught |
| D7 | Padding check removed | Caught |
| D8 | `from != sender_leaf` alone | Survives, as the author notes (see L5 below) |
| D8b | `from != sender_leaf` **and** the FramedContent sender check | **Caught**: "sender data naming Alice, processed as Charlie's" |
| D9 | Leaf binding disabled | Caught |
| C1 | Stored decoder for every GroupData | Caught |
| C4 | Welcome decodes with the stored decoder | **Caught**: `test_welcome_with_legacy_group_data_refused` (survived at `81efdc50`; L2) |
| M1 | Add does not re-encode 0.10.0 GroupData | **Caught**: `test_add_reencodes_legacy_group_data`, where Dave's accept returns -84 |
| M3a | `kp_priv` storage error read as "not ours" | **Caught**: test_protocol "a KeyPackage storage error keeps the invitation pending: kp_priv" |
| M3b | `kp_full` storage error takes the minimal-KP fallback | **Caught** (kp_full) |
| L1a | sqlite `step_lookup` reads errors as absence | **Caught**: `test_sqlite_read_errors_are_not_absence` |
| T1 | Observing a peer's Commit time removed | Caught: "Bob's Commit after Alice's" and "Bob's message after the Add" |
| T2 | No reservation | Caught |
| M2a | Message soft lead removed | **Caught**: "message 29 dated 31 s ahead" |
| M2b | Commit dated past the bound instead of `EVENT_RATE` | **Caught**: lead_is_bounded and peer_lead_capped |
| M2c | Observe cap removed | **Caught**: "Bob's floor 0 is 100000 s ahead" (`d58add27`) |
| L3a | Joiner floor not seeded from the Welcome | **Caught**: "Dave's first message after his Add" |
| L3b | Welcome rumor dated `now` | **Caught**: "the Welcome is dated as its Commit" |
| N2 | Malformed floor row is an error | **Caught**: `test_group_event_floor_repairs` |

### Status of each finding

#### M1: resolved, interop checked (`cb9d5f15`)
**The fix.** `marmot_add_members()` checks the stored GroupData. When it does not read as MIP-01, the Add re-encodes it as MIP-01 in the same Commit, through a GroupContextExtensions proposal built by `legacy_group_data_reencoded()` (`groups.c`) and `mls_group_add_members_with_extensions()`. The re-encode is `updated_group_data()` with no changes, which also adds `required_capabilities [0xF2EE]` when it is missing.

**The test** reproduces my original scenario. A trio's stored GroupData is rewritten to the 0.10.0 layout, Alice adds Dave, and all four members converge, read MIP-01, keep name, description and admins, and exchange messages.

**Interop risk:**
- **RFC 9420.** A GroupContextExtensions proposal is "Path Required" (§17.4). Add Commits are built by `path_commit_with_proposals()`, so they carry an UpdatePath. ✔
- **libmarmot 0.11 members.** The MLS receive path (`mls_group_process_commit`) is unchanged on this branch (only `add_members_staged` changed in `mls_group.c`), so a 0.11 member processes this Commit exactly as the converging 0.12 test shows. 0.11's `marmot_commit_authorize()` decodes the legacy `pre` with its old fallback and the MIP-01 `post` normally. It treats the Commit as privileged and requires the committer to be an admin, which an Add already requires. ✔
- **libmarmot 0.10 members.** No new risk. Since 0.11 they cannot decrypt any kind:445 (MIP-03 ChaCha20 replaced NIP-44) or read MIP-01 GroupData, so a group mixing 0.10 and 0.11+ members was already broken for them.
- **MDK.** A group whose GroupData is still in the 0.10.0 layout cannot contain MDK members (MDK never read that layout). An MDK joiner added through this path receives a MIP-01 v2 GroupContext with `required_capabilities`. ✔
- **Proposal order.** The sender emits `[Add…, GroupContextExtensions]`. libmarmot receivers apply them in vector order, then validate the new extensions against every leaf, joiners included (`mls_group.c:3419-3436`). RFC 9420 §12.3 applies the GroupContextExtensions proposal first, and "the new extensions MUST be used when evaluating other proposals" (Adds included). This makes no difference here, for three reasons:
  - The resulting tree and GroupContext do not depend on the order.
  - The new extension list is a superset of the old one.
  - The joiners' leaves are checked against the new list either way.

  An RFC-order receiver such as OpenMLS evaluates the Adds against `required_capabilities [0xF2EE]`, which Marmot KeyPackages advertise. ✔ The comment citing the RFC is wrong (N4).
- **Residual.** The added `required_capabilities` needs every leaf to advertise 0xF2EE. libmarmot leaves have done so since `83f0e036`, which includes 0.10.0 (`credentials.c:324` there). A leaf from the very first libmarmot commit would make the Add fail cleanly with an MLS error and create nothing, which is no phantom member. 0.11's metadata Commit already behaved that way.

#### M2: resolved (`6ade03b3`, `d58add27`)
**The new rule** (`marmot.c:82-200`). The floor row keeps two values, the newest event `last` and the newest Commit `commit`:
- An application message is dated `max(now, last + 1)`. Beyond the 30 s soft lead it is dated `now + 30`, or `commit + 1` if that is later. It is never dated past `now + 60`.
- A Commit is dated `max(now, last + 1)`. If that would pass `now + 60`, the Commit is refused with `MARMOT_ERR_EVENT_RATE`.
- An observed peer Commit (or our Welcome) still moves us at most to our `now + 60`, and the observation never lowers the floor.

**Verified:**
- My original reproduction (300 messages leaving the next Commit 302 s ahead) is covered by `test_group_event_lead_is_bounded`: 120 messages stay within 30 s, the Commit after them fits, and Commits made back to back end in `EVENT_RATE` with nothing pending.
- The 60 s bound is also well inside MDK 0.8's default `max_future_skew_secs` of 300 s (`mdk-core/src/lib.rs:172`). `validate_created_at_with_now` rejects kind:445 events dated further ahead as `InvalidTimestamp`. So at `81efdc50` an unbounded lead past 300 s would also have made MDK peers refuse our events, not just relays.

**`MARMOT_ERR_EVENT_RATE`: is refusing our own Commit for about a second safe, and does Groundhog retry?**
- **Nothing is created.** The refusal comes from `marmot_next_group_event_time()`, before the floor is written and before any Commit is staged, inside the operation's storage transaction. Every Commit path reserves its time this way: `finish_local_commit_now()` for remove, metadata and self-update; `add_members_impl()` and `create_group_impl()` directly. The test asserts that nothing is pending afterwards. Messages are never refused. `marmot_leave_group()` publishes nothing. ✔
- **When it fires.** Groundhog sends one change at a time (`stage_change()` returns BUSY while a change is being sent, `gh-mls-service.c:1938`), so our own Commits cannot approach 60 per second. In practice it fires in two cases:
  - within about 1-2 s of applying a peer Commit dated about 59 s or more ahead of our clock (a fast clock or a hostile member), if we also sent a message in that second. By the next tick the Commit fits.
  - after a backward clock step (L6), where the refusal lasts far longer than "a second".

  A hostile member committing every second could keep our Commits refused. Such a member already makes our Commits lose to its own (stale epoch), so this is not a new denial of service.
- **No retry in Groundhog.** `stage_change()` → `marmot_fail()` returns `GH_MLS_COMMIT_ERROR` "The change could not be made: too many group events too fast; retry later" (`gh-mls-service.c:361-389`). The user retries by hand. That is acceptable for the 1-2 s case, and N5 suggests treating the error as transient. Create-group cannot hit it: a new group's floor is empty, and its Add follows the creation Commit by one second at most.

#### M3: resolved (`151606a2`)
- `welcome.c:519-578`: only `MARMOT_ERR_STORAGE_NOT_FOUND` from `kp_priv` means "not this KeyPackageRef". For `kp_full`, only NOT_FOUND (or an undecodable KeyPackage) takes the minimal-KP fallback. Any other error returns with the Welcome pending, and `matched_priv` is wiped.
- `test_accept_welcome_keypackage_storage_failures` covers both labels: the error is returned, the Welcome stays pending with no failure record, and it is accepted afterwards. Mutations M3a and M3b are caught. ✔

#### L1: resolved for sqlite; nostrdb accepted as inspected, tracked by nostrc-d1oq (`151606a2`)
**sqlite.** `step_lookup()` treats only `SQLITE_ROW` and `SQLITE_DONE` as success, in every single-row lookup, `mls_load` and `get_exporter_secret`. `mls_delete` now checks its step. A damaged exporter row is `MARMOT_ERR_STORAGE`, not "missing". `test_sqlite_read_errors_are_not_absence` overwrites the lookup tables' pages and asserts every lookup returns `MARMOT_ERR_STORAGE`, and mutation L1a is caught. ✔

**nostrdb.** I read the diff line by line, and it is correct:
- `ndb_lookup()` maps `MDB_NOTFOUND` to absent and any other `mdb_get()` code to `MARMOT_ERR_STORAGE`.
- Failed allocations are `MARMOT_ERR_MEMORY`.
- Damaged records (an empty processed-Welcome value, an exporter value under 32 bytes, an undecodable group, message or KeyPackage info) are `MARMOT_ERR_STORAGE`.
- `mls_delete` aborts the transaction on failure, instead of committing a failed one.

The author's note is accurate. nostrc-d1oq exists and records why no test runs this backend: the contract test passes a directory to an `MDB_NOSUBDIR` environment, so it is SKIPPED, and the suite crashes with a file path, before this branch too. `storage_nostrdb.c` is compiled in the default build (`libmarmot/CMakeFiles/marmot.dir/src/storage_nostrdb.c.o`), and no in-tree product constructs it (marmot-gobject uses sqlite). Accepted, with d1oq tracking a test.

#### L2: resolved (`cb9d5f15`)
`test_welcome_with_legacy_group_data_refused` pins the Welcome to the strict decoder: mutation C4 is now caught. ✔

#### L3: resolved (`6ade03b3`)
- The Welcome rumors of an Add (and of `create_group`) carry the Add Commit's reserved created_at.
- Accepting a Welcome seeds the joiner's floor through `marmot_observe_group_event_time(…, welcome_created_at())`, capped at `now + 60`.
- Mutations L3a and L3b are caught.
- A rumor up to 60 s ahead is accepted by both sides. libmarmot's own rumor check rejects only Welcomes that are too old (`welcome.c`, `max_event_age_secs`), and MDK 0.8's Welcome path does not validate the rumor's created_at. ✔

#### L4: resolved (`6ade03b3`)
The README section "Timing of kind:445 events" states the dating rules and both public signals (sender chains, epoch markers), and the bounded lead caps both. One sentence overstates (N6).

#### L5: resolved (`ddc33101`)
- `test_private_message_commit_checks_alone` pins each check at the layer where it alone decides:
  - the header epoch and the blank leaf via `mls_group_handshake_sender()`;
  - the FramedContent epoch via a PublicMessage Commit, where it is the only epoch check;
  - signature verification against the sender data.
- `test_private_message_commit_of_past_epoch` covers:
  - a header naming the previous epoch (`WRONG_EPOCH`);
  - a relay replay of the applied Commit, a duplicate that changes nothing;
  - a tampered parent-epoch Commit judged on the retained parent;
  - the new epoch refusing the old Commit at the MLS layer.

  This was the case my review asked for.
- **The `from != sender_leaf` note is right.** `mls_handshake_content_decode()` sets the FramedContent sender from the same sender data `private_commit_open()` returns as `from`. So `from != sender_leaf` (`mls_group.c:3087`) and `pm->content.sender.leaf_index != sender_leaf` compare one value, and neither alone can make a test fail. Removing both (D8b) is caught. Keeping both as defence in depth is fine.
- Generation reuse stays an MLS-layer contract test, because handshake keys are never consumed. The README documents this. Accepted.

#### N1, N2, N3: resolved
- **N1:** the three rows now sit inside the decision table (`VERSION_MANIFEST.md:144-146`, before "## Maintenance" at line 148).
- **N2:** a row of any other shape is read as absent and rewritten (`test_group_event_floor_repairs`; mutation caught).
- **N3:** the floor-bypassing wrapper is gone. `marmot_commit_build_event()` takes created_at, and every production caller reserves it.

### New findings

#### L6 (Low): A floor left ahead by our own clock blocks every Commit until real time catches up
**Where:** `libmarmot/src/marmot.c:161-183` (`marmot_next_group_event_time`). The loaded floor is used as is: `next = t.last + 1`, and a Commit past `now + 60` is refused. Nothing ever lowers the floor.

**Scenario.** Our clock was more than 60 s fast while we published to the group, then it was corrected backwards. Typical causes:
- a dual-boot machine whose RTC holds local time, in a UTC+ zone, before NTP syncs;
- someone fixing the time by hand;
- restoring a store backed up on a device with a fast clock.

From then on, every Commit to that group (add, remove, rename, self-update) returns `MARMOT_ERR_EVENT_RATE` for (step − 60) s, while messages are dated a constant 60 s ahead.

I reproduced it with a throwaway test on `d58add27`. Alice's floor row was set 2 h ahead. Her message was dated +60 s, and three renames over 4 s each returned `-36 too many group events too fast; retry later`.

The error says "retry in a second" but the refusal lasts hours, and Groundhog does not retry. A peer cannot cause this, because observations are capped at our current clock. Compared with `81efdc50` this is not a regression: there, the same Commits were dated hours ahead and refused by relays and by MDK (more than 300 s ahead). Compared with 0.11, which had no floor, it is new.

The same unchecked load also lets a corrupted row with `last = INT64_MAX` overflow `t.last + 1`, which is undefined behaviour. The `INT64_MAX` guard of `81efdc50` was removed.

**Fix:** after loading, clamp both values to `now + GROUP_EVENT_MAX_LEAD − 1`. Anything we dated past the bound was outside what relays and MDK accept anyway, so order against it cannot be kept. Add a test with a far-ahead row.

#### N4 (Nit): The proposal-order comment misstates RFC 9420
`libmarmot/src/mls/mls_group.c:1630-1632` and `mls_group.h` say the extensions are "applied after the Adds as receivers apply it (RFC 9420 §12.4.2)". §12.3 applies a GroupContextExtensions proposal **first**, and the new extensions are used to evaluate the Adds. §12.4.2 only refers back to §12.3. The outcome is the same here (see M1), but correct the comment. libmarmot's own receive order (vector order, then validation of all leaves) predates this branch.

#### N5 (Nit): Groundhog shows `EVENT_RATE` as a generic failure
`gnome/groundhog/src/mls/gh-mls-service.c:369-383`: `marmot_fail()` maps `MARMOT_ERR_EVENT_RATE` to the generic `GH_MLS_COMMIT_ERROR`. Map it to `GH_MLS_SERVICE_ERROR_BUSY`, or to another transient code, so the UI treats it like "Another change of this group is still being sent". The alternative is an automatic retry after a second.

#### N6 (Nit): The README overstates the ordering guarantee
The README says "An application message is never dated at or before a Commit". `marmot.c`'s own comment admits the edge: past the bound a message is dated `now + 60`, the same second as a Commit dated there, whether a peer's or a run of ours. Say "at most in the same second, at the 60 s bound".
