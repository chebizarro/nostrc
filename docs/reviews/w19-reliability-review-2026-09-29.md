# W19 review: Groundhog test reliability and the Linux gate's smoke run

- **Reviewer:** independent peer review (AGENTS.md), 2026-09-29
- **Branch:** `review/w19-reliability` (tip of `tests/w19-reliability`), 10 commits on `f5147aaa`
- **Beads covered:** nostrc-yzlp, nostrc-vlb4, nostrc-16yi, nostrc-rjz2, nostrc-qp24.85/.92, nostrc-gem9/f56o, nostrc-9g6e

| Commit | Subject |
|---|---|
| `2c78e8ac` | test: NO-12 timer check waits for D-Bus replies, not a 2 s window (yzlp) |
| `1f0a00f5` | test: PT-4 waits until every wrap copy was taken (vlb4) |
| `0834ca25` | fix(pre-push): Linux gate keeps and prints a rerun test's first failure, counts reruns (16yi) |
| `df3031db` | test: never reserve a port by binding and releasing it (gem9, f56o) |
| `4b4429c0` | fix: attachment-ui pastes from a private clipboard (rjz2) |
| `2386fa2e` | test: onboarding waits for the signer probe (qp24.85, qp24.92) |
| `ef79ee1d` | test: judge activation-dependent UI state by the activation it saw (9g6e) |
| `1d3813d5` | test: PT-4 room and AT-5 wait for settled state; name unexpected signer calls (vlb4) |
| `f9b5b0ad` | test: relay-wire checks a held port cannot be bound by anyone else (gem9, f56o) |
| `9c093e32` | fix: background status says nothing while the account store opens (yzlp) |

**Verdict: REQUEST CHANGES.** One Medium finding blocks: **R1**. Because of the new clipboard seam, the attachment-ui "text pastes as text" check runs a code path that only exists in tests. A regression that breaks text paste for users now passes the suite; I planted one and it passed. The fix is small, and everything else is sound.

---

## 1. What I ran

| Check | Result |
|---|---|
| `cmake -S . -B /tmp/w19rr -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w19rr` (macOS arm64) | builds (2383 steps). `gnostr-profile-edit.ui` restored afterwards |
| `ctest --test-dir /tmp/w19rr -R 'groundhog-' -j6` | **67/67 pass**, 4 skipped on macOS (launch, store-key-keyring, background-gui, notifier-gui) |
| Loaded, macOS: 10 × `ctest -R groundhog- -j12` with 14 `yes` CPU hogs on 14 cores | **9/10 clean**. One failure: `groundhog-blossom` timeout in `download-rebinding`, a test this branch does not touch (O1) |
| Loaded, Linux (gate image, gate volume, under `flock /work/.lock`): 6 × `ctest -R groundhog- -j12` under `dbus-run-session xvfb-run` with `nproc` `yes` hogs | **4/6 clean**. One `groundhog-nip29-service` `denied-and-closed` 10 s wait, one `groundhog-preferences` timeout. Neither is in code this branch changes (O1). The 9g6e GUI tests (`notifier-gui`, `background-gui`, `composer`, `conversation-view`) passed in all 6 |
| `scripts/linux-gate.sh .` (arm64, warm volume) | **passes** in 53 s: all targets built, 416 smoke tests, no rerun needed |
| `bash scripts/test-linux-gate-smoke.sh` | passes, including under macOS `/bin/bash` 3.2 |
| `python3 scripts/check-unsequenced-args.py` | clean: "No call modifies and uses a variable in different arguments" |
| **M1** Revert the product fix: drop `return self->status;` for `GH_ACCOUNT_STORE_OPENING` in `compute_status` | `no11-status-waits-for-store` **fails**: `'gh_background_get_status(f->background)' should be NULL` (test_background.c:832) |
| **M2** Add a stray 30 s repeating `g_timeout_add_seconds` in `gh_background` setup | background tests **fail** by name: `source 9 (unnamed) fires in 29746 ms while idle` |
| **M3** Drop the `gh_conversation_store_has_wrap` skip in `handle_wrap` (duplicate copies cost signer calls) | `groundhog-dm-inbox` and `groundhog-account-store` **fail**. `privacy-e2e` still passes: in its scenarios duplicates are caught by `pending_ids`. That is coverage from before this branch, which did not weaken it |
| **M4** In `on_paste_clipboard`, stop the `paste-clipboard` emission unconditionally (users' text paste is broken) | branch: `attachment-ui` and `composer` **pass** (R1). Base `f5147aaa` test file against the same mutation: **fails** at `caption_pasted` (line 1152) when the macOS pasteboard `poll(2)` flake doesn't abort it first (3 of 4 runs hit that flake, 1 of 4 reached the paste wait and failed there) |

Every mutation was reverted and the tree rebuilt before the loaded runs. `git status` was clean at the end.

---

## 2. Product changes

### 2.1 Background status while the account store opens (`9c093e32`, nostrc-yzlp)

`compute_status()` now returns the status it already had for `GH_ACCOUNT_STORE_OPENING`. It used to return "Receiving messages" (gh-background.c:111-116). At start `self->status` is NULL. `send_status()` sends nothing for NULL, so the desktop shows no status until the key lookup answers. Then it shows "Receiving messages" or "Keyring locked" (the NO-11 case). This is correct and more honest than before. A locked keyring can no longer show "Receiving" first, whether or not the portal answers before the keyring does.

- **Implementation:** `update_status()` compares the returned pointer to `self->status` with `g_strcmp0`, finds them equal and does nothing. There is no use-after-free and no self-assignment.
- **Enable toggle:** turning the setting off and on while the store is opening gives NULL (it was NULL while off), so nothing is sent. Correct.
- **Test:** `no11-status-waits-for-store` holds the key lookup until the portal's version was read and every D-Bus reply has landed. It then checks the status is NULL with no `SetStatus` sent, and after the lookup completes, exactly one `SetStatus`, "Keyring locked", and never "Receiving". M1 shows it catches the revert.

See **B1** for the re-entry cases (`OPENING` after a state other than the initial one).

### 2.2 `gh_composer_set_clipboard()` (`4b4429c0`, nostrc-rjz2)

- **Scope and privacy.** It is not a product hook. It is a plain C setter in an internal header. It is not a GObject property, action, D-Bus method, GIR symbol or setting, and nothing in `src/` calls it; the only caller is `test_attachment_ui.c:1143`. Only code already inside the process can reach it, and that code can read the clipboard anyway. The test hands in a base-class `GdkClipboard`, which is process-local: it has no pasteboard or Wayland/X11 backing. That is the point of the fix. The dispose path clears the reference. **No privacy risk.**
- **Behaviour with the default NULL.** The users' path is unchanged. The early `attach_possible()` return became a condition on both attach branches, and with `self->clipboard == NULL` all branches fall through as before. Text still goes to `GtkTextView`'s default handler.
- **Test fidelity: see R1.** The new `else if (self->clipboard)` branch runs only in tests. It is also what the text-paste check now exercises.

---

## 3. Test changes

- **NO-12 idle timers (`2c78e8ac`).**
  - **Stronger than before.** The old loop waited up to 2 s for short timers to *disappear*, so a one-shot timer that fired inside the window was hidden. The new loop fails at once on any short timer that is not a GDBus reply timeout. It excludes its own two sources by id and re-checks after every main-loop turn, and a source armed during dispatch can only fire on a later turn, after the check. M2 is caught by name.
  - **GLib dependency (N1).** It depends on GDBus's internal source name `"[gio] send_message_with_reply_unlocked"` (test_background.c:536), present in GLib 2.90 here. If GLib renamed it, each in-flight call's default 25 s reply timeout would be under the 59 s threshold and flagged as an idle timer. That fails loudly, not vacuously.
- **PT-4, pt4-room and AT-5 (`1f0a00f5`, `1d3813d5`).**
  - `wait_copies_taken()` waits for the inbox's `received` counter, which counts each relay's copy (gh-dm-inbox.h:78, incremented at gh-dm-inbox.c:466). It waits for the number of accepted targets per role, where it used to wait only for "inbox idle".
  - The signer-call assertion keeps its old strength: `calls == since` equality, and it now also names the unexpected calls. See **P1** and **P2** for the helper's limits.
- **Held and refused ports (`df3031db`, `f9b5b0ad`).**
  - Reserving a port by binding and then closing it is gone. `test_held_port` checks that a stranger's `SO_REUSEADDR` bind to the held port fails with `ADDRESS_IN_USE`.
  - While down, the held listener accepts and then closes. A dial therefore fails at the WebSocket handshake instead of with `ECONNREFUSED`. Groundhog's relay code doesn't classify those errors differently; only `gh-attachments.c:92-193` does. The attachment tests use `gh_test_refused_port()`, so they keep the refused path.
  - `test_wire_relay_down` and `partial_success` still cover a refused relay.
- **Onboarding (`2386fa2e`).** The test now waits for `signer_availability != UNKNOWN`, then asserts the row text. If the probe never answers, it fails with a named timeout. If the row doesn't follow the availability, it fails on the assertion. Not weakened.
- **Activation spans (`ef79ee1d`).**
  - When `is-active` did not change during the span, the assertions are exact: 1 when active, 0 when inactive, the same as before. They loosen to `made <= 1` (or `visible ∈ {accepted, NULL}`) only when activation actually changed mid-span. That is correct: either answer is right then.
  - Before this branch, a display where the window is never active also only ever checked the "0" case, so that is not a new gap.
  - The `FOCUS_WITHIN` state-flag check became a `gtk_root_get_focus()` ancestry check. It is logically equivalent and doesn't depend on activation.

---

## 4. Port 1 as "never answers"

- **Nothing listens by default.** Port 1 is TCPMUX (RFC 1078), served only by an inetd or xinetd entry that no current Ubuntu, Debian or Fedora image, macOS, or GitHub-hosted runner enables. The tests passed on macOS and in the Linux gate container, both with loopback `127.0.0.1:1` refused.
- **Port 1 is not reachable by accident.** It sits below every ephemeral range, so a bind to port 0 or an outgoing connection's local port can never land on it. That is the property the fix needs.
- **"Privileged" is not a guarantee inside containers.** Docker 20.10+ sets `net.ipv4.ip_unprivileged_port_start=0` in containers, so any process in the container's network namespace *could* bind it. Nothing in the suite does, and a stranger that did would fail the test by name (`gh_test_refused_port()`'s probe). That is acceptable.
- **Only IPv4 is probed, which is sufficient.** Every URL uses the `127.0.0.1` literal, never `localhost`.
- **Gap:** see **T1**, a filtered (DROP) port 1.

---

## 5. Gate script (`scripts/linux-gate-smoke.sh`, nostrc-16yi)

- **`set -euo pipefail` pitfalls: none found.**
  - Every `grep` that may match nothing is guarded (`|| true`) or sits in an `if` condition or inside `echo` arguments, where `-e` doesn't apply.
  - `grep -c … || true` yields `0`, not an empty string.
  - `failed_in`'s `sed | paste` exits 0 on no match.
  - `JOBS` and `SMOKE_EXCLUDE` are required under `-u`. `linux-gate.sh` passes both with `-e`, and variables inherited from the environment stay exported through `exec`.
  - The whole script sits in `{ … }`, so an edit to the file mid-run cannot splice two versions together.
- **Failure handling.**
  - A first run that fails with no named test prints the log tail and exits 1.
  - A rerun that fails prints both outputs and exits 1.
  - A rerun that ran fewer tests than it was asked to fails. The unit test covers all three.
- **Disk growth.**
  - First-run logs are pruned to `HISTORY_KEEP` (20). The names start with a UTC timestamp, so `sort` orders them by time.
  - `gates` gets one line per gate and `reruns` one per rerun test, about 40 bytes each, and neither is ever trimmed: about 1 MB after roughly 25,000 gates. Negligible (**G2**).
- **Concurrency.** The history is written only while fd 9's `flock` on `/work/.lock` is held. It is inherited through `exec bash /gate/smoke.sh`, so two gates on the same volume cannot interleave their appends or pruning. Each architecture has its own volume, so arm64 and amd64 gates keep separate histories. See **G1** for `RUN_ID`.
- **Provenance.** The smoke script is mounted read-only from beside `linux-gate.sh`. It comes from the invoking checkout, as the old inline `CMD` did, so nothing changes there.

---

## 6. Findings

### R1: Medium (blocking). The text-paste check tests a code path users never run

- **Where:** `gnome/groundhog/src/ui/gh-composer.c:430-436`, `gnome/groundhog/tests/ui/test_attachment_ui.c:1158-1162`
- **What changed.** Text paste always went through the users' path: the composer's handler lets the `paste-clipboard` emission continue, and `GtkTextView`'s class handler pastes. With a clipboard injected, text instead takes the new `else if (self->clipboard)` branch. That branch stops the emission itself and calls `gtk_text_buffer_paste_clipboard()`. It exists only when a test injects a clipboard, and it is the only text-paste coverage in Groundhog (the attachment-ui test is the only caller of `paste-clipboard`).
- **Scenario (M4).** Make the composer stop the emission for every paste, or for text by mistake, for example by widening an attach condition so plain text matches. Users can then no longer paste text into the composer. `groundhog-attachment-ui` and `groundhog-composer` still pass, because the test branch pastes regardless. The base test caught it at `caption_pasted`.
- **Minor, test-only.** The seam branch also skips `GtkTextView`'s `scroll_after_paste`.
- **Fix, either way.** In both, the users' decision "text continues to the default handler" is again the thing under test:
  - **(a) Test-only, preferred.** Drop the `else if (self->clipboard)` branch. In the test, connect a normal (not `G_CONNECT_AFTER`) handler to `paste-clipboard` after the composer's own. It runs only if the composer let the emission continue. The handler records that it was reached, stops the emission, and pastes from the private clipboard. Assert it was reached for text and *not* reached for the texture paste.
  - **(b)** Make the composer always paste text itself from `clipboard`, in users' builds too, and scroll to the insert mark as `GtkTextView` does, so test and users share one path.

### B1: Low. On re-entering `OPENING`, the status keeps the previous state's message

- **Where:** `gnome/groundhog/src/app/gh-background.c:111-116`. The store re-enters `OPENING` from `gh-account-store.c:624` (sign-in or account switch in `reconcile`), `:736` (`reopen`, unlock) and `:566` (shred).
- **Scenario.**
  - Background mode is on with no account, so the status says "No account". The user signs in and the store goes `INACTIVE → OPENING`. The desktop keeps saying "No account" until the key lookup answers, which with a slow or prompting keyring can take a while.
  - Switching from account A to account B keeps "Receiving messages" while B's lookup runs and A's store is already unbound.
  - Before this branch, both cases said "Receiving messages", so neither is a regression in honesty. "No account" during sign-in is new, though, and the header comment only says the status "stays what it was".
  - No test covers re-entry.
- **Suggestion:** add a test for `INACTIVE → OPENING → OPEN`. Decide whether a transitional message (for example "Starting…") is better than a stale one once a status has been sent. The portal cannot clear a status once set, so NULL is not available after the first send.

### P1: Low. `wait_copies_taken()` counts every recipient copy against one inbox

- **Where:** `gnome/groundhog/tests/privacy/test_privacy_e2e.c:515-530`
- **What it does.** Every accepted non-self target is added to `mark->to`'s expected `received` count.
- **Where it is right.** It is correct for 1:1 sends. It is also correct for `pt4-room` as written, because Carol's copies are never accepted.
- **Scenario.** A future room test where two recipients' relays both accept. Bob's inbox never sees Carol's wrap, so the wait fails with a named 10 s timeout. It fails loudly, not vacuously, but the helper reads as general.
- **`>=` can end the wait early.** A late copy of an earlier message can satisfy it. That re-exposes the old race; it cannot hide a regression.
- **Suggestion:** count only targets whose recipient is `to` (or assert the room case away), and document the limit.

### T1: Low. The refused-port probe blocks without a timeout

- **Where:** `gnome/groundhog/tests/gh-test-port.h:43`
- **Scenario.** A host or CI sandbox whose firewall *drops*, rather than rejects, loopback traffic to port 1. The blocking `g_socket_connect` then waits for the kernel's SYN timeout: about 75 s on macOS, about 127 s on Linux. ctest's 120 s timeout kills the test first, without the helpful "a dial … was not refused" message.
- **Suggestion:** `g_socket_set_timeout(socket, 5)` before connecting, so the named `g_error` fires. The code already treats a timeout as "not refused".

### P2: Info. The `pt4-room` NEEDS_ATTENTION assertion is now tautological

- **Where:** `test_privacy_e2e.c:1449` (the wait) and `:1453` (the assertion)
- The new `spin_until(item_needs_attention)` means the later `g_assert_cmpint(... NEEDS_ATTENTION)` can only fail if the state regresses afterwards.
- A product regression that settles a partial room send now fails as the named timeout "the room message needing attention" instead of an immediate assertion. It is still caught.

### N1: Info. NO-12 depends on GDBus internals

- **Where:** `test_background.c:536`
- **Source name.** It depends on the source name, discussed in §3: loud if GLib renames it.
- **Delivery order (not verified).** It also assumes GDBus attaches the reply's completion to the main context before it destroys the reply-timeout source. If the order were reversed, a main-loop check between those two steps would see no call in flight and nothing ready. `assert_no_short_timers` would then stop one turn early, and a timer armed by that reply's callback would go unchecked. I did not verify the order against GLib's source here. Worth a comment, or a second settle turn after `in_flight` reaches 0.

### G1: Info. `RUN_ID` uniqueness

- **Where:** `scripts/linux-gate-smoke.sh:32`
- Inside the container under `--init`, `$$` is effectively the same on every run. `RUN_ID` is therefore unique only through its one-second timestamp.
- That is safe, because the volume lock serialises gates and a gate takes far longer than a second. Tests that call the script directly on the host (without the lock) get distinct `$$` values.

### G2: Info. Rerun counts are advisory

- **Where:** `scripts/linux-gate-smoke.sh:117-130`
- A test that needed a rerun in 20 of the last 20 gates still passes the gate with a `!!` line.
- That matches the bead ("a flake to fix, not to absorb" is left to the reader). Consider failing, or at least filing a bead automatically, above a threshold (for example ≥ 5/20).
- Unbounded growth of `gates` and `reruns` is negligible (§5).

### O1: Out of scope. Load flakes in tests this branch doesn't touch

- **`groundhog-blossom` `download-rebinding`** (macOS, 1 in 10 loaded runs). A 120 s ctest timeout. `test_blossom.c:41` `spin_until` and `:281` `download()` have no deadline, so a stuck download never produces a named failure.
- **`groundhog-nip29-service` `denied-and-closed`** (Linux, 1 in 6). `condition waited for at line 844 did not hold within 10s`.
- **`groundhog-preferences`** (Linux, 1 in 6). Timeout.

These match the branch's own claim of about 3 residual failures in 50. I recommend beads for them; I did not file them, because this review leaves beads alone.

---

## 7. Summary

| ID | Severity | Commit | Where | Finding |
|---|---|---|---|---|
| R1 | **Medium (blocking)** | 4b4429c0 | gh-composer.c:430-436; test_attachment_ui.c:1158-1162 | text-paste test runs a test-only branch; users' text-paste regression passes (M4) |
| B1 | Low | 9c093e32 | gh-background.c:111-116 | re-entering OPENING keeps a stale status ("No account" during sign-in) |
| P1 | Low | 1f0a00f5 | test_privacy_e2e.c:515-530 | copy wait counts all recipients' copies against one inbox |
| T1 | Low | df3031db | gh-test-port.h:43 | probe has no timeout; filtered port 1 → unnamed ctest timeout |
| P2 | Info | 1d3813d5 | test_privacy_e2e.c:1449-1453 | NEEDS_ATTENTION assertion now tautological (regression still caught by the wait) |
| N1 | Info | 2c78e8ac | test_background.c:536 | depends on GDBus source name and reply ordering |
| G1 | Info | 0834ca25 | linux-gate-smoke.sh:32 | `RUN_ID` uniqueness rests on timestamp + lock |
| G2 | Info | 0834ca25 | linux-gate-smoke.sh:117-130 | rerun counts never escalate |
| O1 | Out of scope | — | test_blossom.c:41,281; test_nip29_service.c:844; preferences | residual load flakes |

- **Product fix:** the background-status fix is correct, honest and guarded by a test that fails without it (M1).
- **NO-12:** the timer check is now stricter (M2).
- **Port helpers:** they remove a real race, and the port-1 assumption holds on macOS, Linux and CI runners.
- **Gate:** the smoke script is robust under `set -euo pipefail`, bounded on disk and serialised by the volume lock.
- **Unsequenced arguments:** the check is clean.
- **Blocking:** R1. The clipboard seam removed the only coverage of users' text paste, and M4 shows a real regression now passes.

**REQUEST CHANGES**
