# Groundhog W18 review: messaging polish, attachment UI (G22), merge fix (2026-09-29)

## Context / scope

This is the independent peer review that AGENTS.md requires. It covers three commits on top of `63f0317d` (`origin/master`), reviewed on branch `groundhog/w18-review` at `e6c859d1`. File:line references are to `e6c859d1` unless a commit is named.

| Commit | Beads | What |
|---|---|---|
| `403ec197` | `nostrc-qp24.75`, `.83`, `.84`, `.86`, `yp69`, `9cho`, `lff5` | W18 messaging polish: read state by arrival (schema v4), N1 backfill notifications, local timer-change rows, pins, Mark Read/Unread, header menu, S4 on every round, persisted no-inbox, room copy and banner, charter amendments |
| `88c6d278` | `nostrc-qp24.40` (+ `nostrc-dnsc`) | G22 attachment UI: attach flow, send sheet, first-use server, per-account upload consent, received-file card, Preferences › Attachments, `GH_FEATURE_ATTACHMENTS`, metainfo |
| `e6c859d1` | — | Merge fix: closes `item_row()` in `test_conversation_view.c` |

- **Ignored.** `chore(beads)` commits and `.beads/` hunks.
- **Normative references.**
  - `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`, especially §3.3, §3.7, §4.2, §4.5 S4, §5.2 N1/N2, §6, D6, §7.x and AT-1…AT-9;
  - NIP-17, NIP-59, Blossom BUD-01/02;
  - `docs/reviews/groundhog-w17-review-2026-09-29.md` and its addendum.
- **Changes.** I changed no code or beads. Scratch programs are in `/tmp/w18scratch` only. Every mutation was reverted, and the tree was rebuilt clean afterwards.

## Summary

- **The read marker by arrival (`403ec197`, schema v4) is correct and well tested.**
  - `messages.seq` comes from a per-room counter that never goes back, so a sequence is never reused.
  - The marker carries `read_seq`, and relay-delivered own messages set a separate reply boundary.
  - Memory (`is_read`) and store (`READ_BY`) use the same predicate, and the durable `seq` is read back after every admit and on restore.
  - The migration is one transaction per version, so a crash mid-v4 leaves v3 intact.
  - The delete trigger keeps the marker from dangling, including when a purge removes it and the rows before it one by one.
  - Mutating either side's `seq` condition fails the tests.
- **N1 backfill is right in semantics and ordering.**
  - The cutoff is the inbox checkpoint less one hour. Without a checkpoint it is the session start, so a first backfill is never replayed.
  - Requests stay in the hidden count-only notification, and a burst coalesces (N2).
  - `bind_store` sets the store before the model binds the account, so the notifier reads the checkpoint before this session's inbox moves it.
  - One gap: the app-level glue (`notifier_last_seen`) is untested (non-blocking #3).
- **The other W18 items are sound:** timer rows, requests not inheriting the default timer, local-only pins, Mark Read/Unread, the header menu, S4 on every round, `no_inbox` and the room copy. Each has a test that fails under mutation (table below).
- **G22's network and crypto paths are right.**
  - Nothing is fetched until Download (AT-7). A mutant that downloads on lookup fails two suites.
  - The download goes through `gh_net_http_get_public_async()` (98a5328a) in production. The private-hosts seam has no `src/` caller, and a mutant that always takes the plain GET fails `groundhog-blossom`.
  - Consent is per account, kept in SQLCipher's `meta` table, loaded only into that account's client, and revocable.
  - Save As writes only to the file the Save dialog returned. SQLite's temp store is in memory, so no plaintext temp files are made.
  - No server is ever chosen for the user, and adding one is pure validation, with no probe.
- **But a dropped or pasted file is read through GVfs, outside GhNetHttp.**
  - A web address dropped on the composer (a browser drag of an image or link) becomes an `https://` `GFile`.
  - The offer path then runs `g_file_query_info_async` and `g_file_load_bytes_async` on it. On a desktop with GVfs, that is a direct HEAD and GET from the user's IP in Tor mode, before the sheet even opens.
  - Reproduced. This is **B1 (blocking)**.
- **Blueprint.** The `.blp` and `.ui` files are in sync under blueprint-compiler 0.12.0 (the CI container) and 0.20.4 (here), and the new files are on the drift list.
- **Tests.** All pass on macOS and in the Linux/Xvfb container. Two parallel-run flakes and one copy bug turned up (non-blocking #1, #2).

## Findings

### Blocking

#### B1. Medium: a dropped or pasted non-local file is fetched through GVfs: a direct HEAD and GET in Tor mode, outside GhNetHttp, before the sheet opens

- **Where.**
  - `gnome/groundhog/src/ui/gh-composer.c:621` accepts `GDK_TYPE_FILE_LIST` and `G_TYPE_FILE` drops.
  - `:347-371` (`on_drop`) and `:396-411` (`on_files_pasted`) emit `attach-file` with whatever `GFile` GTK made.
  - `gnome/groundhog/src/app/gh-attachment-ui.c:557-561` → `gh_attachment_ui_offer_file` (`:510-530`) → `g_file_query_info_async` (`:524`) → `on_file_info` → `g_file_load_bytes_async` (`:506`).
  - Nothing checks `g_file_is_native()` or the URI scheme.
- **Why it matters.**
  - Charter §4.2 (Tor): "**Never fall back to a direct connection**", and "Media (§2.1, §6) … use the same mode".
  - The G22 header (`gh-attachment-ui.h:32-33`) promises "Nothing leaves before Send".
  - GVfs's HTTP backend uses its own libsoup session. It knows nothing of `network-mode`, the Tor SOCKS isolation, GhNetHttp's no-redirect policy, or the public-address check, and it announces a `gvfs/…` User-Agent.
- **Reproduced** (Ubuntu 24.04, the CI image plus `gvfs gvfs-backends`, under `dbus-run-session`).
  1. `/tmp/w18scratch/deser_probe.c` runs GTK's own `text/uri-list` → `GDK_TYPE_FILE_LIST` deserializer, which is what a drop runs:
     ```
     GdkFileList entry: https://images.example.org/cat.jpg (GDaemonFile, native=0)
     ```
  2. `/tmp/w18scratch/gvfs_probe.c` makes the two calls the offer path makes, on such a `GFile`, against a local web server:
     ```
     GFile type GDaemonFile, uri scheme http
     query_info: type=1 size=5000 native=0
     load_bytes: 5000 bytes
     --- requests the local web server received:
     "HEAD /photo.png HTTP/1.1" 200 -
     "GET /photo.png HTTP/1.1" 200 -
     ```
  `G_FILE_TYPE_REGULAR` passes for such a file, so `on_file_info` goes on to load it.
- **Failure scenarios.**
  1. **Tor bypass.** A user in Tor mode drags a photo from a web page, or a link from a chat or feed reader, onto the composer. GVfs sends HEAD and GET for that URL straight from the user's IP. The image host, or anyone who planted the link, learns that this IP runs Groundhog at that moment. That is exactly what Tor mode exists to prevent, and it happens even if the user then presses "Don't Upload".
  2. **D6 bypass.** For a URL whose HEAD has no `Content-Length`, `g_file_info_get_size()` is 0, so the size check (`:495`) passes. `g_file_load_bytes_async` then reads the whole stream into memory, which is unbounded for a streaming endpoint. "Nothing larger than the limit is even read" is false for these files.
  3. **No-proxy and System modes too.** The request skips GhNetHttp's isolation, no-redirect and public-address policy (a link to `http://192.168.1.1/…` is fetched), and it is visible to the network as a GVfs client.
- **Why tests miss it.** Neither macOS nor the CI image has GVfs. There a non-native `GFile` is a `GDummyFile`, and `query_info` fails with "Operation not supported". `test_drop_and_paste` drops only a local file.
- **Suggested fix.** One check at the choke point. `gh_attachment_ui_offer_file` refuses `!g_file_is_native(file)` before any I/O, with a toast such as "Only files on this device can be sent. Save it first." Portal document paths (`/run/user/…/doc/…`) are native, so the file dialog and sandboxed drops keep working. The same check covers a non-portal file chooser's "Other Locations" (sftp://, smb://), which would otherwise go through GVfs too.
- **Suggested test.** In `groundhog-attachment-ui`, drop a `GdkFileList` holding `g_file_new_for_uri("https://example.invalid/x.png")`. Assert the refusal toast, no sheet, and that no `query_info` ran (for example, with a `GFile` subclass that counts calls, or by asserting the toast text rather than "The file couldn't be read").
- **Severity.** Medium. It needs a user gesture and a desktop with GVfs, which is every stock GNOME install. It blocks because it breaks the Tor-mode guarantee the rest of G22 carefully keeps (the public-address policy, `.onion` handling, the Tor copy), and the fix is a few lines.

### Non-blocking (follow-up suggested)

1. **Low (copy, and one cause of a flake): a server that resets the connection is described as "The network setting changed before the server answered."**
   - **Where.** `gnome/groundhog/src/media/gh-attachments.c:190-191` matches `G_IO_ERROR_CONNECTION_CLOSED` before `network_error()`.
   - **Why.** In GLib, `G_IO_ERROR_CONNECTION_CLOSED == G_IO_ERROR_BROKEN_PIPE` (44), and `g_io_error_from_errno()` maps both `ECONNRESET` and `EPIPE` to it. I checked this on macOS and in the Linux image with `/tmp/w18scratch/errenum.c`. So the `BROKEN_PIPE` case in `network_error()` (`:97`) is dead.
   - **Only one real source for the copy.** GhNetHttp's mode-change error (`gh-net-http.c:75`) is the only case the copy is meant for.
   - **Scenario.** A Blossom server drops an upload mid-body, or a Tor circuit is torn down mid-download. The card or sheet says the network setting changed, which is false, and it does not say the server couldn't be reached.
   - **Seen in the wild.** It surfaced in a stress run. `/groundhog/attachment-ui/card-cancel-and-errors` failed at `test_attachment_ui.c:1266` because the "unused" port had been taken by another process, which reset the connection ("Error receiving data: Connection reset by peer"). The card's summary then lacked "Can't reach the server".
   - **Suggestion.** Give the mode-change case its own marker, for example a `GH_NET_HTTP_ERROR` code or a dedicated quark, and treat a reset or broken pipe as a network error.
2. **Low (tests): "unused port" races make three tests flaky under parallel ctest.** Each takes a free port, releases it, and relies on it staying free.
   - **Where.**
     - `test_multi_send.c:1488-1515`: the new `wire/retry-spacing` releases three ports and re-binds them after a round;
     - `test_attachment_ui.c:1252-1266`;
     - `test_attachments.c:741-754`.
   - **Observed.**
     - The very first `ctest -R groundhog- -j6` run failed `groundhog-multi-send` at `wire-relay.h:561` (`soup_server_listen_local` could not re-bind).
     - Over 11 more full `-j6` runs, `groundhog-attachment-ui` failed once.
     - In 48 runs of `test-groundhog-attachment-ui` in 6 concurrent loops, there was one port-race failure (the reset in #1). There were also six macOS-only environment failures: pasteboard contention and GLib `poll(2)` EAGAIN.
     - Serially, `groundhog-multi-send` passes 8 of 8.
   - **CI risk.** The hosted job runs ctest serially, so the risk there is small. The ASan job runs `--parallel 2`.
   - **Suggestion.**
     - For "unreachable", hold a socket that is bound but not listening for the test's lifetime. It refuses connections and cannot be taken.
     - For `retry-spacing`, bind the three relays up front and keep them refusing (a `serve`/accept switch in `wire-relay.h`) until the retry is due.
3. **Low (test gap): the app's last-seen glue is untested.**
   - **Where.** `gh-app-services.c:709-723` (`notifier_last_seen`), and the ordering it relies on (`gh-account-store.c` `bind_store`: store set → attach binds the model → inbox `set_storage`).
   - **Mutation check.** `notifier_last_seen` returning 0 passes every test I could run (macOS skips `-notifier-gui` and `-background-gui`; neither references the checkpoint).
   - **Scenario.** A refactor moves `gh_dm_inbox_set_storage` before `gh_store_conversations_attach`, or makes the glue read the wrong cursor. Backfill then silently stops being notified (the checkpoint has already moved to now, so the cutoff becomes now − 1 h), or it replays up to the initial 30-day window. `groundhog-notifier` stays green because it injects `last_seen` directly.
   - **Suggestion.** An account-store-level test: a store with a saved `nip17-inbox` cursor, bind, and assert what the notifier was given before the inbox wrote.
4. **Low (copy honesty): the sheet names only the first server, and after consent it does not say the upload is signed as the account.**
   - **Where.** `gh-attachment-ui.c:167-195` (`update_notes`) says "uploaded to `<first server>`… can see … your IP address".
   - **Why that is incomplete.** The upload moves to the next server on any non-auth failure (G21), so after a 5xx a second server also receives the ciphertext and the user's IP.
   - **Consented servers.** For a server the account consented to, the note leaves out that the server learns the account. The consent page itself (`gh-attachment-sheet.c:151-156`) and the Preferences row say so correctly.
   - **Suggestion.** Preferences already says "the first server in this list that accepts them". Say the same on the sheet when there is more than one server, and add "and that it's from your account" for a consented first server.
5. **Low (memory and jank; `nostrc-nzek`): photos are decoded at full size on the main thread.**
   - **Where.** `gh-attachment-card.c:216-218` and the sheet thumbnail at `gh-attachment-ui.c:397-398`.
   - **Scenario.** A received 8192×8192 JPEG, the most the decode guard allows, decodes to about 256 MiB when Download finishes and stalls the UI while it does. The held bound (`gh-attachments.c:332-352`) always keeps the newest READY transfer, so the process holds about 64 MiB plus one such photo, and GTK's GL upload adds more.
   - **Why not blocking.** The pixel count is capped by the guard. Nothing decodes without the user's Download. Older photos are released under the 64 MiB bound. On a desktop this is a performance problem, not a correctness or privacy one.
   - **Suggestion.** It should land before a mobile form factor ships: decode off the main thread and at display size (a size-limited loader), and keep the full decode for Save As only.
6. **Nit: bisect gap.** `88c6d278` does not compile `test_conversation_view.c`: `item_row()` lacks its closing brace until `e6c859d1`. `e6c859d1` itself is correct. Next time, squash a merge fix into the commit it repairs.
7. **Nit: `drop-and-paste` uses the real system clipboard.** On macOS it overwrites the developer's pasteboard and races other GUI tests ("Failed to send clipboard to pasteboard" under concurrency). A private `GdkClipboard`, or a display of its own, would isolate it.
8. **Nit: repeated mnemonics.** Every card's `_Download`, `_Cancel`, `_Try Again` and `_Save As…` repeats the same mnemonic in each bubble of the list (`gh-attachment-card.blp:108-143`). Buttons inside list rows normally carry none.
9. **Nit: RAM residue after expiry.** A disappearing message's decrypted file stays in `GhAttachments`' entry after the message expires. The entry is dropped only on a store change, Clear, or the held bound. It is memory only, so P3 (at rest) holds, but the entry could be dropped when the message leaves the model.
10. **Nit: damaged v3 stores lose read-only viewing.** A store that is damaged *and* still at v3 can no longer be opened read-only after this upgrade ("too old to be migrated"). That is the existing policy for any schema bump, but the release notes could mention it.
11. **Outside this review.** `groundhog-net /groundhog/net/public-only` (`98a5328a`, already on master) timed out once in `spin_until` (`test_relay_soup.c:186`) during a `-j6` run. Worth a bead if it recurs.

## Per-commit notes

### `403ec197` messaging polish

- **Read state by arrival (`nostrc-qp24.75`).**
  - **Trigger.** `messages_admit_seq` numbers every insert whichever writer stores it. `INSERT … ON CONFLICT DO NOTHING` does not fire it, and no `REPLACE` or `UPDATE … conversation_id` exists (grep), so a `seq` never changes after admission.
  - **Migration defaults.** Existing rows keep `seq 0` and `read_seq 0`, so `READ_BY` reads exactly as before.
  - **`READ_BY` / `is_read`.** A message is read if it sorts at or before the reply boundary, or at or before the marker with `seq ≤ read_seq`. A same-second lower-id rumor, or a clock-skewed one arriving after the read, stays unread, is counted, and is pruned from notifications only when read (`room_prune` now checks each message).
  - **Mark read.** `mark_read` covers exactly the listed arrivals (`listed_seq`). With unread messages still unloaded it keeps the W13b seen-range. `absorb_seen` now also requires `read_seq ≥ seen_seq`.
  - **Mark unread.** In memory it moves the marker and the reply boundary back to just before the newest incoming message. The store sets `read_seq = admit_seq`. The two agree because `can_mark_unread` requires no unread at all.
  - **Delete trigger.** `messages_keep_read_marker` moves the marker to the newest remaining row before it. Under a multi-row purge the final position is the newest surviving row whatever the deletion order, and the purge's pre-computed unread count matches.
  - **Forget** clears `read_seq`, the reply boundary and `timer_changed_at`.
  - **Mutations.** Dropping the `seq` condition in `READ_BY` fails `groundhog-store-conversations`. Dropping it in `is_read` fails `-store-conversations`, `-conversations` and `-notifier`.
- **N1 (`nostrc-qp24.84`).**
  - `sync_since` computes `MIN(session_start, last_seen − 3600)`, or `session_start` without a marker. It runs at every account bind, including a relock and reopen, where it re-reads the moved checkpoint.
  - **Comparison.** Rumor time against the inbox checkpoint (a local wall-clock "everything held was received" time) is the right comparison. The hour of slack is honest in the charter, and duplicates are excluded by the seen set.
  - **Lock screen.** The default level is hidden, and `test_backfill` asserts the canary is absent from every payload and the request's identity is hidden.
  - **Mutations.** No backfill, replay-all and no-grace each fail `groundhog-notifier`. The glue gap is #3.
- **Timer rows and the requests decision (`nostrc-qp24.83`).**
  - `timer_changed_at` moves only on a real change (`CASE WHEN disappearing_s <> ?1`).
  - The row is local, and the blueprint says it is never sent.
  - A request starts at 0 (`conversation_insert` binds the default only for `GH_STORE_REQUEST_ACCEPTED`). This matches the charter amendment and the Preferences subtitle.
  - **Mutation.** Always applying the default fails `groundhog-expiry`.
- **Pins, Mark Read/Unread and the header menu (`nostrc-qp24.86`).** `pinned_rank` lives in the encrypted store, and no GSettings key exists (grep). Pinned rooms come first in pin order, and requests get neither item. Tests in `-conversation-menu-gui`, `-conversation-list` and `-conversation-info-gui` pass on both platforms.
- **S4 (`nostrc-yp69`).**
  - `round_schedule` draws a fresh order and spacing in memory for any round whose due wraps have no stored `not_before` ahead. The stored values stay untouched, and the test asserts that.
  - The commit also applies the spacing to `GhDmSender`'s direct room send. I did not mutate that path separately.
  - **Mutation.** Disabling `round_schedule` fails `groundhog-multi-send`.
  - The W17 nit remains: whole-second gaps from an inclusive range, so a quarter of gaps are 0 s.
- **`no_inbox` (`nostrc-9cho`) and room copy (`nostrc-lff5`).** The flag is written only for targetless recipient wraps and restored on load, and it is covered by `wire/no-inbox-restart`. The accessible descriptions use the recipient count.
- **Charter.** The amendments (§3.3, §3.6, §3.7, §4.5 S4, §5.2 N1, §7.x) match the code as shipped.

### `88c6d278` G22 attachment UI

- **Attach.**
  - **Entry points.** GtkFileDialog, drop, and paste. A pasted image is re-encoded as a new PNG of its pixels, so only the picture is sent.
  - **Limits and preparation.** Size is checked before the read, and folders and special files are refused. Preparation strips metadata in memory, and the sheet thumbnail is shown only after the decode guard.
  - **Where it goes.** Recipients are fixed at the offer. Relay groups and oversized rooms get no attach button.
  - **Account change.** A store change closes the sheet and cancels the upload (`on_attachments_reset` → `offer_free`).
  - **Exception.** B1.
- **Upload and consent.**
  - AUTH_REQUIRED shows the consent page naming the server. Consent is recorded per account (`gh-store-blossom.c`, the `meta` range scan checks each key's prefix and URL) and applied to that account's client only (`set_store`, after the signer, which clears consents).
  - Revoke deletes the row and updates the client.
  - The send is queued through the durable outbox, and `remember_sent` caches the sender's own plaintext only if `ox` matches.
- **Receive.**
  - `lookup` fetches nothing, and Download is the only way to start a transfer.
  - Cancel returns the transfer to IDLE at once and ignores a late completion (generation check and cancellable identity).
  - The decode guard runs before any texture is made, and an undecodable photo is marked and never retried.
  - Save As uses GtkFileDialog, or the tests' seam, and `g_file_replace_contents_async` to that file only. The name comes from magic bytes, never from the sender. The toast says saved files aren't protected.
  - Errors are in plain words, apart from #1.
- **Address policy.** `gh_blossom_client_download_async` calls `gh_net_http_get_public_async` unless the tests-only seam is set, and no `src/` code sets it. `groundhog-blossom` fails if the plain GET is used (mutation above), and `test_attachments.c:758-768` checks the host refusal at the service level.
- **Preferences › Attachments.** Servers in order, with Move Up/Down and Remove. Revoke shows only for consented servers. Clear sits behind an `AdwAlertDialog`. The copy matches the build (Tor or not, attachments or not).
- **Gating and metainfo.** `GH_FEATURE_ATTACHMENTS` follows `GROUNDHOG_HAVE_ATTACHMENTS` and is 0 without OpenSSL or libsoup. The metainfo and the `blossom-servers` description say what ships. No version bump, as asked.
- **CI lists.** `groundhog-attachments` and `-attachment-ui` are in the build and required lists, and `-attachments` is in the sanitizer list. `groundhog-attachment-ui` runs in the container (70/70).

### `e6c859d1` merge fix

It is correct: it adds the missing `}` of `item_row()`. See nit 6 for the bisect gap it closes.

## Mutation checks

Each mutant was built, tested and reverted, and the tree was rebuilt clean afterwards.

| Mutant | Result |
|---|---|
| Blossom download always takes the plain GET (no connect-time policy) | `groundhog-blossom` fails |
| Notifier: `notify_since` ignores the marker (no backfill) | `groundhog-notifier` fails |
| Notifier: `notify_since = 0` (replay all) | `groundhog-notifier` fails |
| Notifier: no one-hour grace | `groundhog-notifier` fails |
| App glue `notifier_last_seen` returns 0 | **all pass** (#3) |
| Store `READ_BY` ignores `seq` | `groundhog-store-conversations` fails |
| Memory `is_read` ignores `seq` | `-store-conversations`, `-conversations`, `-notifier` fail |
| Requests inherit the default timer | `groundhog-expiry` fails |
| `round_schedule` disabled (S4 first round only) | `groundhog-multi-send` fails |
| `gh_attachments_lookup` starts a download (AT-7) | `groundhog-attachments`, `-attachment-ui` fail |

## Verification

- **macOS 15 (Darwin 24.6, arm64).** Submodules initialised (`third_party/nostrdb`, `third_party/nsync`).
- **Build.** `cmake -S . -B /tmp/w18rev -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w18rev` succeeded with no Groundhog compiler warnings (only the known `ld` duplicate-library notices). `apps/gnostr/data/ui/dialogs/gnostr-profile-edit.ui` was rewritten by the build and restored.
- **Tests.**
  - `ctest --test-dir /tmp/w18rev -R 'groundhog-' -j6`: the first run was 66/67, with `groundhog-multi-send` failing on the port re-bind (#2). Serially it passed 8 of 8.
  - Eleven further full `-j6` runs: 9 clean, one `groundhog-attachment-ui` failure (#2), and one `groundhog-net` timeout (#11).
  - Four platform skips: `groundhog-launch`, `-store-key-keyring`, `-background-gui`, `-notifier-gui`.
- **Linux container.** `BUILD_VOLUME=w18rev-linux scripts/groundhog-linux-ci.sh` (Ubuntu 24.04, blueprint-compiler 0.12.0, `dbus-run-session -- xvfb-run`, the CI's ctest regex, serial): **70/70 passed**, including `groundhog-blueprint`, `-attachment-ui`, `-conversation-menu-gui`, `-notifier-gui` and `-background-gui`.
- **Blueprint.** `groundhog-blueprint` passes with 0.20.4 (macOS) and 0.12.0 (CI image). `gh-attachment-card` and `gh-attachment-sheet` are on `GROUNDHOG_BLUEPRINTS`.
- **Scratch (`/tmp/w18scratch`).**
  - `deser_probe.c` and `gvfs_probe.c`: B1.
  - `errenum.c`: #1.
  - `mut.sh`: the mutations above.
  - Stress: 6 concurrent loops × 8 runs of `test-groundhog-attachment-ui` (41 passed; 7 failed: 1 port race, 3 pasteboard, 3 `poll(2)` EAGAIN).
- **Checks.** `git diff --check 63f0317d e6c859d1` (excluding `.beads`) is clean.
- **Not run.** A real Tor daemon, a real Blossom server, a GNOME session with GVfs driving the actual window (B1 was shown at the GTK deserializer and GIO layers, which are exactly the calls the code makes), and the hosted CI.

**REQUEST CHANGES**

- **Blocking.**
  - **B1.** A dropped or pasted file that is not on this device (for example a web address dragged from a browser) is read through GVfs. That is a direct HEAD and GET from the user's IP in Tor mode, outside GhNetHttp and before the sheet opens, and the size limit can be skipped. Refuse non-native `GFile`s in `gh_attachment_ui_offer_file` before any I/O, and add the drop test.
- **Everything else is non-blocking.** Highest priority first:
  - the reset/broken-pipe copy (#1);
  - the port-race flakes (#2);
  - the untested last-seen glue (#3);
  - the sheet's server and consent copy (#4);
  - full-size main-thread decoding (#5, `nostrc-nzek`). It should not block: the decode guard bounds it, and nothing decodes without Download.
- **Approved as is.** `403ec197` (read marker and schema v4, N1, timer rows, pins, S4, `no_inbox`, copy), `e6c859d1`, and all of `88c6d278` apart from B1: consent storage, AT-7, the public-address download path, Save As, Preferences, gating and metainfo.
