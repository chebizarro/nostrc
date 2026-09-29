# Groundhog W13 integrated peer review — 2026-09-28

## Context / scope

This is an independent review of exactly `d12fcc89bab3d6027f0d89064fecd837ba96dfe6..5ec0ade31ec7f64bb3ff6c97517edca38fde9b15`. It ignores beads-only commits (`264c5b8d`, `a4c4c23f`, `91efc507`, `74db4375`, `24587ad4`, `22112acb`, `4d9fb3bd`, `9d9089e0`, `28f3cfd3`, `52fa2c92`, `5ea363b2`, `ebff73a8`, `6ce05674`, `2fea7507`) and the W12 review document itself (`d5ee93aa`).

- `4fa998cb` **G01**: privacy defaults in the gschema, plus `tests/check_privacy.py` (`groundhog-privacy-static`).
- `6f047581` and `c9b34380` **G03**: `GhStoreKey`, Secret Service custody of the store key, plus deterministic keyring tests.
- `16ef4587` **G02**: `GhStore`, the SQLCipher store core, the ST-4 single-SQLite guard, `GhClock` and the crash harness.
- `b15de4bc` **Inbox hardening** (`qp24.10.10/.11/.12/.14`):
  - backfill paging;
  - rejected-wrap namespace;
  - own-inbox ACCOUNT AUTH through `GhAccountAuth`;
  - lookup sources limited to `discovery-relays`;
  - charter §3.3 `seen` ns 5 and the R6 amendment.
- `a7c9f581` **G23**: `GhStoreMarmot` (a `MarmotStorage` vtable) and schema v2.
- `d208f264` and `1f2e05c0`: the shared private D-Bus test bus (`tests/common/nostrc-test-bus.*`, `cmake/NostrcTestBus.cmake`). It replaces GTestDBus in the Groundhog and gnostr tests.
- `e8553b9a` **G05**: `GhStoreConversations`, T-admit, and the legacy `.seen` import.
- `d9ed4835` **G06**: the durable outbox, the retry scheduler and `GhMessageStatus`.
- `8194746f` and `3da68e39` **G11**: shell IA, the conversation list, status banners, shortcuts, and the NIP-17 inbox wired into the executable.
- `5ec0ade3`: Groundhog 0.6.0.

Normative reference: `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`, in particular §1 (threat model), §3 (encrypted store), §4 (network and AUTH) and §7 (UX).

**What ships.** In the shipped executable, `main.c:118-121` now creates one `GhConversationStore` and one `GhDmInbox`:

- The app subscribes to the account's own kind-10050 relays and unwraps through the signer.
- It shows the conversations in the new list.

The rest of the wave is compiled into or linked beside the app but has no production caller yet: `GhStore` (linked into the executable for ST-4), `GhStoreKey`, `GhStoreConversations`, `GhStoreMarmot` and `GhOutbox`. G04, the account↔store lifecycle, is still in progress (`nostrc-qp24.21`).

I changed no code or beads.

## Findings

### Blocking

1. **The shipped app shows each private message for one session, then hides it for good, and the UI never says so (charter P4, §3.4, §7.15 #15; the seen-set's own contract).**
   - **Wiring.** `main.c:118-121` builds an in-memory `GhConversationStore` and a `GhDmInbox` with the default state directory. `bind_account` installs the per-account `GhNip17Seen` file (`$XDG_STATE_HOME/groundhog/nip17/<pubkey>.seen`) as the store's persistence delegate (`gh-dm-inbox.c:955-972`).
   - **Defect.** `seen_admit` (`gh-dm-inbox.c:323-343`) durably records the wrap id and rumor id of a message that exists only in memory. Its own comment says a restart loses the message (`:336-338`). This contradicts the seen-set contract: "Record a message only after it is durably stored" (`gh-nip17-inbox.h:119`).
   - **Checkpoint.** Once everything has settled, the checkpoint advances to *now* and is written to `<pubkey>.checkpoint` (`gh-dm-inbox.c:227-265`).
   - **After a restart:**
     - The next session's `since` is the checkpoint minus 49 h.
     - The relays deliver the same wraps again. `handle_wrap` skips them as seen, before any signer call (`:458-463`).
     - The model deliberately never shows "a delivered rumor the delegate committed but the model does not list (a restart without restore …)" (`gh-conversation-store.c:206-208`).
     - Older wraps are no longer requested at all.
   - **Account switch.** The same loss happens inside one process when the user switches to another account and back. `gh_conversation_store_set_account` drops the model (`:123-135`), but the first account's seen file stays.
   - **It becomes permanent.** G05's legacy import takes the file's `r` lines "as seen without a message, so those messages are not shown again" (`gh-store-conversations.h:116-126`, ST-12). So even after G04 attaches the encrypted store, Groundhog will never show any DM that 0.6.0 displayed, although the relays still hold it.
   - **The UI is not honest about it.**
     - No in-app surface says that messages are not kept.
     - After a restart, the empty state promises "Private messages people send you will appear here" (`gh-sidebar-page.blp:170`).
     - `GhStatus` has no storage banner (`gh-status.c:80-111`).
     - The content page's reason line only says sending is not implemented.
     - Only the AppStream description mentions it: "messages are not kept after it closes" (`org.nostr.Groundhog.metainfo.xml:9`). That reads as "fetched again next time", not "never shown again".
     - The charter makes memory-only operation an explicit, labelled mode: "Continue Without Saving Messages", in-memory only, with zero files written (§3.4, §7.15 #15). P4 forbids claiming completeness the app cannot prove.
   - **Required.** Either of these fixes is acceptable (neither is large):
     - **(a) Make memory-only admissions truly memory-only.** In the executable's current configuration:
       - keep `w`/`r` keys in memory only;
       - keep persisting the `x` (rejected) namespace, which never hides a message;
       - do not persist the checkpoint.

       A restart then unwraps the backfill window again. That costs signer calls, but it is honest.
     - **(b) Keep the persisted keys to avoid re-prompting, and make both of these changes:**
       - Say so in the app wherever the list can be empty or partial. For example, adjust the empty-state description and add a non-problem banner or reason line: "Messages aren't saved in this version: each is shown once, while Groundhog is open."
       - Make the ST-12 import take only the `x` lines of a file written by the memory-only inbox, and ignore its `.checkpoint`. The first encrypted-store session then fetches again whatever the relays still hold.

     (Not wiring the inbox into the executable until G04 would also resolve it.)
   - **Test.** Add a restart test on the executable's wiring (same state directory, a new store and inbox). It should assert one of these:
     - with (a): the message is listed again;
     - with (b): the notice is shown and the import leaves the rumor unseen.

### Reviewed areas — no blocking finding

- **Keying and pragma order (G02, charter §3.5)**
  - **Key first.** `store_open_keyed` (`gh-store.c:1110-1143`) opens the file and keys it with `sqlite3_key_v2` (`:1135`) before any statement runs:
    - it uses SQLCipher's raw-key form `x'<64 hex>'`, so there is no KDF;
    - the key never appears in SQL text, so statement traces cannot capture it;
    - the hex spec lives in `sodium_malloc` memory and is freed with a wipe (`:1122-1136`);
    - a key buffer the store owns is wiped as soon as it has been used.
  - **Pragma sequence (`store_configure`, `:1023-1107`).** Every setting is read back, and a mismatch fails the open:
    1. `cipher_version` must be non-empty (`:1035-1041`).
    2. A `SELECT count(*) FROM sqlite_master` verifies the key before anything can write (`:1043-1057`). A wrong key maps to `GH_STORE_ERROR_KEY`.
    3. `journal_mode=WAL`, then `synchronous=FULL`.
    4. `foreign_keys`, `secure_delete`, `temp_store=MEMORY` and `trusted_schema=OFF`.
    5. `quick_check` in `store_initialize`. A newer `user_version` is refused.
  - **Hardening.** The connection sets DEFENSIVE, disables extension loading and allows no ATTACH (limit 0).
- **Files, WAL and temp files (§3.2)**
  - **Layout checks.** Every existing layout component is checked with `lstat`: it must not be a symlink, must be owned by the user and must have no group or other bits (`gh-store.c:381-441`). Unsafe nodes are refused, not repaired.
  - **Creation.** `store.db` is pre-created `O_CREAT|O_EXCL|O_NOFOLLOW` with mode 0600 (`:481-506`), so `-wal` and `-shm` inherit 0600. Stale sidecars are removed before a new database is created (`:464-479`). The `-wal`/`-shm` sidecars are checked on every open.
  - **Content.** WAL pages are SQLCipher-encrypted. Temp data stays in memory: the ST-4 guard also refuses a `TEMP_STORE=0` build (`:658-663`).
  - **Checkpoints.** A `TRUNCATE` checkpoint runs on close (`:1634`), after a purge at most once a minute (`:3096-3115`), and after forgetting a conversation (`:3186`). Forget destroys the key item before the unlink, and the unlink never follows links (`:1705-1760`).
- **ST-4 (one SQLite per process)**
  - **Link order.** The executable and the store tests link SQLCipher first with `--no-as-needed` (`CMakeLists.txt:586-608`).
  - **Runtime guard** (`gh-store.c:548-705`):
    - On ELF it resolves every entry point through `dlsym(RTLD_DEFAULT)` (`:599`), the same lookup that binds libsoup's and libmarmot's references. On Mach-O it requires a two-level namespace and refuses `DYLD_FORCE_FLAT_NAMESPACE` (`:625-628`).
    - It then checks `HAS_CODEC`, thread safety and `cipher_version`.
  - **Tests.** `groundhog-store-sqlite` runs in the executable's own link set and passed on macOS (SQLCipher 4.17.0) and on Linux (4.5.6).
- **Crash atomicity versus the real transaction boundaries**
  - **One transaction each.** T-admit (`:2209-2349`), T-enqueue (`:2383-2515`), T-seal (`:2551-2646`), T-outcome (`:2663-2704`), purge, forget and each migration each run in one `BEGIN IMMEDIATE`. A nested call becomes a savepoint.
  - **No fragments after an abort.** `store_txn_alive` refuses every statement once SQLite has rolled the transaction back on its own (`:757-765`). So no fragment of an atomic operation can autocommit, and a failed `COMMIT` is rolled back (`:958-967`).
  - **Order of writes.** T-admit writes `seen` for the wrap and the rumor before the message, in the same transaction. T-seal writes every wrap, its targets and `SEALED` together.
  - **G05.** `delegate_admit` wraps `gh_store_admit` and the read state in one outer transaction (`gh-store-conversations.c:412-461`).
  - **Crash harness.** It is real: a `fork` and a `SIGKILL` at named cut points (`tests/store/crash-harness.c:12-40`; the cut list is at `gh-store.c:67-83`), followed by a reopen and assertions. It proves atomicity under a process crash. Power loss rests on `synchronous=FULL`, which is read back but cannot be tested this way.
- **Key custody (G03, §3.4)**
  - **Guarded memory.** Secrets live in read-only `sodium_malloc` memory, wrapped in a `GBytes` (`gh-store-key.c:25-77`). Only a 32-byte secret is copied out of libsecret's secure memory (`:295-316`).
  - **Order on first open.** The item is stored and confirmed before any file exists (`gh-store.c:1510-1521`).
  - **No prompts in the background.** Background mode never prompts, and a background store needs an existing, unlocked default keyring (`gh-store-key.c:511-533`).
  - **Locked is not missing.** An empty search while the default keyring is locked reports LOCKED, not NOT_FOUND (`:414-440`). A locked key therefore cannot be offered for a "start fresh" wipe.
  - **Tests.** KC-6 ran against a real gnome-keyring in Docker (below).
- **Inbox hardening (`b15de4bc`); this closes W12 non-blocking #2, #3, #5 and the inbox half of #6**
  - **Paging.** A full page is followed by another request with an inclusive `until`. A tied second is stepped over and marks the relay incomplete. Paging is bounded by `max_pages` (`gh-dm-inbox.c:590-676`). The checkpoint moves only when every endpoint has reached EOSE and completed its backfill, with nothing pending or deferred (`:244-265`).
  - **Rejected wraps.** A rejected wrap is recorded only when its unwrap cost at least one signer call (`:426-436`). It is checked before any signer call (`:458-463`).
  - **ACCOUNT AUTH.** It is set only for the endpoint's own 10050 URL (`:514-538`). `GhAccountAuth` implements the amended R6 (`gh-account-auth.c:136-166,205-277`): one open signer request per relay, a denial that sticks for the generation, and waiting requests that fail on revoke.
  - **Lookups.** Recipient lookups use only `discovery-relays` (`gh-inbox-lookup.c:411-420`).
  - **Static guards.** `check_privacy.py` pins these rules with mutation self-tests: `lookup-sources`, the `account-auth-purpose` allowlist (`:162-166,418-425`), the URL-literal ban and the GSettings allowlist.
- **Outbox (G06, §3.6; not yet instantiated in production)**
  - **Order.** T-enqueue commits before any signer call (`gh-outbox.c:1459-1486`). The store refuses to re-seal (`gh-store.c:2569-2576`), and the outbox then republishes the stored wraps (`gh-outbox.c:783-787`).
  - **AUTH.** Every publish URL is EPHEMERAL (`:983`).
  - **Retries:**
    - The backoff runs 15 s, 1 min, 5 min, 30 min, 2 h, then 6 h, each ×U(0.8, 1.2), drawn from libsodium's `randombytes_uniform` (`:21,164-178`; `gh-clock.c:153-157`).
    - Retries stop 72 h after T-enqueue (`:157-161,1030-1033`).
    - The D8 delay applies only when normalized URLs overlap (`:181-199,744-775`).
    - A cancelled publish and an interrupted round do not count as attempts (`:1085-1098`).
    - An account switch stops every seal and publish before the generation changes (`:1281-1327`).
  - **Status.** No status claims more than a relay `OK`, and there is no DELIVERED or READ value (`gh-message-status.c:21-50`; `check_privacy.py` `message-status`).
- **`GhStoreMarmot` (G23, D5)**
  - **Snapshot scope.** It is the four group-keyed sets: the group info, its relays, its exporter secrets and `mls_kv` under label `"mls_group"`. `"mls_group"` is the only group-keyed label libmarmot writes (`libmarmot/src/groups.c:57-77`, `messages.c:190-210`, `welcome.c:544`). The other labels (`kp_priv`, `kp_full`, `kp_slot`, `welcome_data`) are keyed by KeyPackageRef or wrapper id.
  - **Rollback.** Clear, restore and consume run in one transaction or savepoint (`gh-store-marmot.c:1638-1719`).
  - **Other rules.** Ephemeral stores are refused, pruning uses unix seconds on both sides, and errors keep the underlying `GH_STORE_ERROR`.
- **UI (G11, §7): HIG and accessibility**
  - **Templates and layout.** All UI is Blueprint templates with checked-in `.ui` files, and `groundhog-blueprint` compares them. The minimum size is 360×294 and the split view collapses below 600 sp (`gh-window.blp:14-15,30-36`). No class above libadwaita 1.5 / GTK 4.14 is used, which the denylist enforces.
  - **Conversation rows:**
    - avatars show initials only and have the `presentation` role;
    - the composed accessible label reads "title. kind. N unread. time. preview" and is bound to `GtkListItem:accessible-label` (`gh-conversation-row.c:166-181`, `gh-conversation-list.c:135`);
    - the preview is dropped from the label when previews are hidden;
    - unread is shown as a count plus bold text, not by colour alone;
    - only named theme colours are used (`style.css`).
  - **Previews.** They follow `show-message-previews` live (`gh-conversation-list.c:334-338`).
  - **Selection and reading.** Autoselect is off (`gh-shell.c:298-300`), so nothing is marked read unless the user chooses it. Mark-read is local and happens only while the conversation is visible and the window is active (`gh-conversation-list.c:212-230`).
  - **Message text.** Bodies are plain text with `use-markup: false` (`gh-message-item.blp:31-32`).
  - **Honest disabled actions.** New Message is disabled, with a tooltip, an accessible description and a shortcut entry that say so (`gh-sidebar-page.blp:57-69`, `gh-shortcuts-window.blp:57`). Banners use honest copy and have no buttons without a destination (`gh-status.c:80-111`).
  - **Requests.** Requests are listed separately under a "Message request" subtitle, and nothing fetches a profile.
- **Shared test bus (`d208f264`)**
  - **What it fixes.** The lifeline supervisors kill daemons however the test ends. Daemon output goes to log files, not ctest's pipes. The session singleton is held for the life of the bus. Together these address `nostrc-doif` and `nostrc-ic36`.
  - **EBADF masking.** It is assessed under non-blocking #1.
- **Versioning.**
  - **Bump.** Groundhog **0.6.0 (MINOR)** is correct. Groundhog is 0.x and gains a shipped capability: receiving NIP-17 messages. It also adds GSettings keys and an optional build dependency. The bump supersedes W12 non-blocking #1.
  - **Consistency.** `CMakeLists.txt:2`, `VERSION_MANIFEST.md:21` and `groundhog --version` agree.
  - **Other components.** The gnostr changes are test-only (no bump), and `tests/common` and `cmake/NostrcTestBus.cmake` are test infrastructure. No libmarmot, nostr-gobject, libnostr or NIP source changed in this range.

### Non-blocking (follow-up suggested)

1. **The macOS EBADF tolerance is inert after the first test case (`tests/common/nostrc-test-bus.c:52-113,227`; header `:52-58`).**
   - **Scope.** The masking itself is acceptably narrow:
     - macOS only;
     - the exact GLib message only;
     - at most 64 occurrences;
     - test code only.

     Linux's `poll()` reports the same race silently as `POLLNVAL`, so the masking hides nothing that Linux CI would see.
   - **It does not work as documented.** I built probes against the helper on this host (GLib 2.90):
     - With the bus brought up **inside** a test case, 1 and 5 EBADFs pass and the 65th aborts, as designed.
     - With the bus brought up **before `g_test_run()`**, which the header recommends, the first EBADF aborts. lldb shows `fatal_unless_forgiven` is never called.
     - In a two-case binary, case 1 (which brings the bus up) passes and case 2 aborts on its first EBADF.

     GTest clears the handler installed by `g_test_log_set_fatal_handler()` between test cases, and `tolerate_select_ebadf()` installs it only once per process.
   - **Where the stability comes from.** The other changes. 60 reruns of the six D-Bus-backed Groundhog tests (`--repeat until-fail:10`) passed and logged zero EBADF warnings.
   - **Suggested fix.** Re-arm the handler per test, for example with a `nostrc_test_bus_tolerate_ebadf()` called from each test's setup or a `g_test_add_func` wrapper. Add a self-test with two cases that each produce one EBADF, plus one case that produces 65. Alternatively, drop the claim from the header.
2. **T-mls atomicity depends on the caller (`gh-store-marmot.h:15-30`).**
   - **Behaviour.** Each vtable write commits on its own unless the caller holds a `gh_store_begin()` transaction.
   - **libmarmot.** Its multi-write operations, such as `groups.c:600-649`, which compensates with `mls_delete` by hand, are atomic only if `qp24.7` / `qp24.13` wrap them as the header shows.
   - **Suggestion.** Make that wrapping an acceptance criterion of those beads.
3. **SQLCipher's own key copy and page buffers are not guarded.**
   - **Verified default.** `PRAGMA cipher_memory_security` defaults to 0 in both SQLCipher 4.17.0 (macOS) and 4.5.6 (Ubuntu 24.04). Groundhog's own buffers are sodium-guarded.
   - **Suggestion.** Either set it ON right after keying, at a performance cost, or record in §3.4 that this is accepted under A4/A5.
4. **A request's title is text the sender controls.**
   - **Cause.** `gh_conversation_get_title` returns the rumor's `subject` when there is one (`gh-conversation.c:497-501`).
   - **Where it shows.** The subject becomes the row title, the avatar initials and the header title of an unaccepted request (`gh-conversation-row.c:147-160`, `gh-conversation-list.c:201-209`).
   - **Charter.** §7.9 shows requests by npub.
   - **Suggestion.** Until a request is accepted (G18), title it with the abbreviated npub and show the subject as secondary text.
5. **Failure-path logs now carry wrap and rumor ids in the shipped app (`gh-dm-inbox.c:340,360,391,400,440`).**
   - **Origin.** These lines date from W12; W13 makes them run in the executable.
   - **Why it matters.** A rumor id is a hash that commits to the plaintext.
   - **Suggestion.** In the spirit of PD-10, drop the ids or demote the lines to `g_debug`.
6. **The legacy state files name the account in clear (`<pubkey>.seen` and `<pubkey>.checkpoint`, `gh-dm-inbox.c:959-978`).**
   - **Where.** They sit outside the encrypted store, but §3.2 names account data pseudonymously.
   - **Suggestion.** Fold this into the B1 fix, or into G04's delete-after-import (ST-12).
7. **Outbox details (not yet wired)**
   - **(a) Self-copy AUTH.**
     - **Current behaviour.** The self-copy also uses EPHEMERAL AUTH (`gh-outbox.c:983`). That is stricter than §4.3, which permits the account.
     - **Consequence.** An own inbox relay that accepts writes only from signed-in members will refuse it for good ("Not saved to your other devices").
     - **Suggestion.** Decide this explicitly in G08.
   - **(b) D8 jitter.** `not_before` is decided at seal time from the sealed target lists (`:766-778`). Relays added later by OB-8 (`:1146-1194`) can create an overlap with no delay.
   - **(c) Wrap retention.** §3.3's "`event_json` NULLed 7 d after settle" is not implemented, so stored signed wraps are kept indefinitely (inside SQLCipher).
8. **UI details**
   - **Message text.** It is not selectable, so it cannot be copied (`gh-message-item.blp:30-36`).
   - **Row times.** They go stale across midnight; this is already tracked in `nostrc-qp24.48`.
   - **Untranslated copy.** The account "limits" strings, now shown under the messages, are untranslated literals (`gh-account-controller.c:535-567`, via `gh-account-ui.c:124-129`).
   - **Localization.** `main.c` never binds a gettext domain (§7.14).
9. **The same race will log on macOS in production.** `GhSigner` closes its private connection after each call. On macOS that makes GLib log a non-fatal "poll(2) failed due to: Bad file descriptor" warning in users' logs. This is cosmetic.

## Verification

- **Checks and submodules.** `git diff --check d12fcc89..5ec0ade3` (excluding `.beads`) is clean. `git submodule update --init third_party/nsync third_party/nostrdb` was run.
- **macOS 15 (Darwin 24.6, arm64; GLib 2.90, SQLCipher 4.17.0 via Homebrew)**
  - **Build.** `cmake -S . -B /tmp/gh-w13-review -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_APPS=OFF -DBUILD_NOSTR_GTK=OFF && cmake --build /tmp/gh-w13-review -j4`: 1183/1183 targets. There are no Groundhog warnings, and Groundhog builds with `-Werror`.
  - **Tests.** `ctest --test-dir /tmp/gh-w13-review -R 'groundhog-|marmot|gobject' -j4 --output-on-failure`: **60/60 passed**:
    - Groundhog: 27 passed, plus 2 platform skips (`groundhog-launch` has no requested GUI session; `groundhog-store-key-keyring` has no gnome-keyring). These are the integrator's 29.
    - libmarmot: 21, including `marmot_test_storage_contract` and `marmot_gobject_test`.
    - nostr-gobject: 10.
  - **Stress.** `--repeat until-fail:10` over `groundhog-{dm-inbox,account,dm-send,account-ui,outbox,account-relays}`: all 60 runs passed, with 0 EBADF warnings.
  - **Leaks.** `leaks --atExit`: `test-groundhog-store-key` has 0 leaks. `test-groundhog-store-marmot` and `test-groundhog-store-conversations` report only GLib's own `g_set_user_dirs` allocations from GTest's isolated directories, with no Groundhog frame.
  - **Probes.** The EBADF probes for non-blocking #1 were built against `tests/common/nostrc-test-bus.c`.
- **Linux (Docker `local/groundhog-ci:24.04`, plus `libsqlcipher-dev` 4.5.6, `gnome-keyring`, `blueprint-compiler`, `adwaita-icon-theme`)**
  - **Build.** I used CI's configure flags (`-DSIGNET_ENABLE=OFF -DBUILD_RELAYD=OFF`, Debug) and CI's target list: 367/367.
  - **Tests.** `dbus-run-session -- xvfb-run -a ctest -R '^groundhog-' -j4`: **30/30 passed**. This includes `groundhog-launch` (the real executable's smoke run), `groundhog-desktop`, `groundhog-store-key-keyring` (KC-6 against gnome-keyring) and `groundhog-store-sqlite` (ST-4 on ELF).
  - **Minimal flags.** A full-tree build with only the minimal flags fails in `signet/src/bootstrap_server.c:32`: `microhttpd.h` is not installed in this image. That code is outside this range, and CI disables it.
- **Not run.** The gnostr tests migrated to the new bus (`apps/gnostr/tests/test_{nostr_target,nwc_wallet_agent,signer_nip55l_identity}.c`) need `BUILD_APPS=ON` and were not built here.

**REQUEST CHANGES**: scoped to integrated commit `5ec0ade31ec7f64bb3ff6c97517edca38fde9b15` versus `d12fcc89bab3d6027f0d89064fecd837ba96dfe6`.

- **Blocking.** There is one blocking finding, B1: the executable permanently hides every message it has shown once, without saying so. Either fix (a) or fix (b), plus the restart test, resolves it.
- **Everything else.** The store, key custody, ST-4, the transactions, inbox hardening, the outbox, `GhStoreMarmot` and the UI are sound as reviewed. The non-blocking items are suggested follow-ups.
