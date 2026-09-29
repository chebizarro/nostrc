# W19 review: own-relay AUTH through GhAuthPolicy, Linux pre-push gate, unsequenced-argument check

- **Reviewer:** independent peer review (AGENTS.md), 2026-09-29
- **Branch:** `review/w19-auth-gate`, based on `d77327e3` (origin/master)
- **Commits reviewed** (cherry-picked cleanly, no conflicts):

| Original | On this branch | Subject |
|---|---|---|
| `82dbe984` | `00f42ce9` | fix(groundhog): route all own-relay AUTH through GhAuthPolicy (qp24.65, .67, .56, .68) |
| `8b49743a` | `a301de63` | feat(pre-push): Linux (Docker) build of every target in the pre-push gate (nostrc-y9xg) |
| `3963ab46` | `9cca0aaa` | feat(checks): flag calls whose arguments modify and use the same variable (nostrc-td3b) |

- **Charter reference:** `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md` §3.6, §4.3, §4.4 R1–R7, NT-1..4.

**Verdict: APPROVED.** Nothing blocks the merge. The follow-ups below are Medium or lower.

---

## 1. What I ran

| Check | Result |
|---|---|
| `cmake -S . -B /tmp/w19rv -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w19rv` (macOS arm64) | builds, 2383 steps; `gnostr-profile-edit.ui` restored afterwards |
| `ctest --test-dir /tmp/w19rv -R 'groundhog-' -j6` | 1 failure, `groundhog-attachment-ui` (SIGTRAP). It passes 3/3 when run alone: a load flake, and not in code these commits touch |
| Mutation M1: drop `clear_deadline()` in `maybe_auth` (the qp24.68 deadline pause) | `groundhog-relay-publish` **and** `groundhog-outbox` fail |
| Mutation M2: drop the `approval_pending` branch in `gh_message_status_derive` | `groundhog-outbox` fails |
| Mutation M3: drop `gh_auth_policy_apply_scope(OWN_LIST_DISCOVERY)` in `GhAccountRelays` (qp24.67) | `groundhog-account-relays` (wire-auth-gated-source) **and** `groundhog-privacy-e2e` (PT-4) fail |
| Mutation M4: give `GhInboxSetup` a private `GhAccountAuth` again (qp24.65) | `groundhog-privacy-static` (account-auth-setter) **and** `groundhog-inbox-setup` fail |
| `scripts/linux-gate.sh "$PWD"`, end to end on this branch (arm64, warm volume) | **passes** in 1m03s: all targets built with `-Werror=` classes, 416 smoke tests. 2 tests (`groundhog-background`, `groundhog-composer`) aborted on the first run and passed on the rerun (see G1) |
| Planted steal-and-read (`g_task_get_cancellable(task)` beside `g_steal_pointer(&task)` in `gh-net-http.c:325`), `scripts/pre-push` fed a ref line | checker exit 1 with the correct message; **pre-push exit 1** before any build |
| Planted `nanosleep()` without `_POSIX_C_SOURCE` in `libgo/src/counter.c` (`-std=c11`), `NOSTRC_GATE_LINUX_TESTS=0 scripts/linux-gate.sh` | **BUILD FAILED**, exit 1: `implicit declaration of function 'nanosleep' [-Werror=implicit-function-declaration]`. The same file compiles cleanly with macOS clang, so the Linux stage is what catches this |
| Static checks on master (`d77327e3`) | `check-unsequenced-args.py --self-test --root master`: pass (15 bad, 29 good; tree clean). `check-linux-ci-packages.py`: n/a on master (the Dockerfile is new). `check_privacy.py` on the branch: pass (19 rules) |
| master built in Docker (`w19rv-master-arm64` volume) | **fails to link** on master: `multiple definition of nostr_metric_*` (libnostrgo.a vs libnostr.a). This is the break `8b49743a`'s weak stubs fix, which confirms that fix is needed |
| Groundhog tests under 2× oversubscription (`ctest -j28`) in the gate image, 6 runs, branch vs master (master with only the three Linux build-fix hunks applied) | similar flake sets on both (see G1); **no flake is new on the branch** |
| `gcc -Werror=implicit-function-declaration … -Wno-error` (nostr-homed's target options) | still an error: a bare `-Wno-error` does not undo the gate's `-Werror=` classes |

---

## 2. Own-relay AUTH through GhAuthPolicy (`82dbe984`)

### 2.1 Answers to the review questions

**Is each purpose's AUTH identity correct?** Yes. The table in `gh-auth-policy.c:22-31` matches charter §4.3: OWN_INBOX_READ, OWN_LIST_PUBLISH, SELF_WRAP and GROUP are ACCOUNT; OWN_LIST_DISCOVERY, CONTACT_DIRECTORY, RECIPIENT_WRAP and MLS_ROUTING are EPHEMERAL, and an unknown purpose falls back to EPHEMERAL. The new callers each use the right purpose:
- `GhInboxSetup` uses OWN_LIST_PUBLISH on its publication connections only (`gh-inbox-setup.c:829`); the probe stays unauthenticated.
- `GhAccountRelays` uses OWN_LIST_DISCOVERY, so a gated discovery relay gets a lazy throwaway key (`gh-account-relays.c`, `start_discovery`).

`check_privacy.py` now blocks the old bypass: the new `account-auth-setter` rule lets only the setters' definitions and `gh-auth-policy.c` name `set_url_auth`, `set_account_signer` or `gh_account_auth_new/_get_signer` (M4 proves the rule bites).

**Can an account key ever authenticate to a non-own relay?** Not through any path this commit adds. PT-4 now asserts that each account's own-list discovery on the gated relay G signs in exactly once, with a key that is no account's. Two paths that predate this commit remain; neither is new:
- OWN_LIST_PUBLISH also covers the discovery-relay targets of inbox setup (finding A4).
- SELF_WRAP treats stored targets as own until the own lists are settled (G08, documented in `leg_purpose`).

**Can the deadline pause hang a publish?** No; every path out is bounded.
- The pause (`gh-relay-publish.c:575`) removes the relay deadline only while a `GhRelayAuthAttempt` exists.
- An attempt at the head of `GhAccountAuth`'s per-relay queue is bounded by the D-Bus call timeout, `SIGNER_CALL_TIMEOUT_MS` = 330 s (`gh-signer.c:13`).
- Any non-cancel failure of that call (timeout, denial, signer gone) fails every waiting request on the relay at once (`gh-account-auth.c:165-171`), so a queue cannot wait N × 330 s on an unattended prompt. Only an approval advances it one request at a time, and each of those is a new user interaction.
- Cancellation is bounded too. `gh_relay_publish_cancel`/`unref` → `close_transport` → `gh_relay_auth_attempt_drop` cancels the attempt's cancellable. A queued request is then removed by `on_waiting_cancelled`; an active one ends as CANCELLED, which starts the next.
- An account switch fires `GhAuthPolicy.on_accounts_changed`. That leads to `drop_auth` and `gh_account_auth_revoke`, which revokes the signer, cancels every pending attempt and fails every waiting request. The publish then ends AUTH_REQUIRED (or was already cancelled by the outbox's generation change).
- A dropped connection still finishes the endpoint through `gh_relay_publish_failed`, which does not check `attempt`.
- Once the AUTH is sent, the full deadline is re-armed (`on_auth_signed`).

**Is the status copy honest?** Mostly yes:
- `approval_pending` only counts when the publish is really waiting on its own AUTH (`is_signing_in`), the leg is SELF_WRAP, and the policy says the relay is WAITING.
- It gives way to SENT and PARTIALLY_SENT, as the §3.6 amendment says.
- The per-relay description is specific ("Waiting for your approval in Nostr Signer.").

There are two gaps: one refresh gap (A1) and one wording nuance (A3).

**Do the tests fail without the fixes?** Yes, for all four fixes; see M1–M4 in §1. The commit message's claim holds.

### 2.2 Findings

#### A1 — Low — "Waiting for approval" is missed when the self-copy's AUTH queues behind another prompt for the same relay
- **Where:** `gnome/groundhog/src/app/gh-outbox.c:1783` (`on_account_state_changed` is the only non-terminal refresh trigger); `gnome/groundhog/src/app/gh-account-auth.c:266-273` (a queued request emits nothing) and `:150`/`:174` (`sign_done` compares WAITING before and WAITING after).
- **Scenario:**
  1. At startup `GhDmInbox`'s REQ on own inbox relay C gets a challenge; Nostr Signer shows that prompt and C's state becomes WAITING (a signal is emitted, but no message publishes to C yet).
  2. The user sends a DM. The self-copy to C is refused `auth-required:`, and its attempt queues behind the inbox request.
  3. No state change, so no `account-state-changed`, and `GhRelayPublish` reports no update for a non-terminal `auth_needed`. The item stays "Sending…".
  4. When the user approves the inbox prompt, `sign_done` immediately starts the self-copy's request. The state is WAITING before and after, so again no signal: the second prompt is on screen while the message still reads "Sending…". It corrects itself only on the next unrelated refresh (a recipient OK or failure).
- **Fix:** have `GhAccountAuth` emit `relay-changed` when a request is queued or handed the signer, even if the aggregate state is unchanged. Alternatively, let the outbox refresh when `maybe_auth` starts an attempt (an update hook for "signing in" in `GhRelayPublish`). A test would queue the self-copy behind an already-active inbox request.
- Found by reading the code; not reproduced.

#### A2 — Low — AUTH `created_at` is fixed at attempt start, and the deadline pause lets a queued attempt outlive the 600 s skew
- **Where:** `gnome/groundhog/src/relay/gh-relay-auth.c:392-393` builds the unsigned template with `now` when the attempt starts; `:279` rejects `|created_at − now| > GH_RELAY_AUTH_MAX_SKEW_SECONDS` (600) when it completes.
- **Scenario:**
  1. The self-copy's attempt queues behind the inbox's prompt for relay C (A1). The user answers that prompt after about 5 min, so the self-copy's template is already about 300 s old when the signer is asked.
  2. The user takes more than about 5 min on the second prompt. The signed event is rejected locally ("created_at too far from now") and the self-copy ends AUTH_REQUIRED ("requires sign-in") even though the user approved.
  3. Before this commit the 30 s publish deadline ended the attempt long before that, so the case could not arise. Relays often use a window tighter than 10 min, and many close idle sockets, so the effective limit is lower.
- **Fix:** build the kind-22242 template in `GhAccountAuth` when the request reaches the signer (`request_start`), not at attempt start; or rebuild it in `begin_sign`/on dequeue.

#### A3 — Info — WAITING_FOR_SIGNER can head a message whose delivery does not depend on the approval
- **Where:** `gnome/groundhog/src/app/gh-message-status.c:260-263` (charter §3.6 amendment) and `gh-outbox.c:541`.
- **Scenario:** a DM to Bob, whose inbox relay is slow. The self-copy waits for the user's sign-in approval on C, so the primary status reads "Waiting for approval" and Bob's delivery looks gated on the user. It isn't: Bob's wrap goes out either way, and declining changes only the self-copy.
- The detail line ("Approve signing in to your message relay in Nostr Signer.") and the charter amendment make this a deliberate, documented choice.
- **Suggestion:** "Sending…" with the approval named in the detail would be more literal. Not a defect.

#### A4 — Info (pre-existing, not introduced here) — account AUTH on discovery relays during inbox setup
- **Where:** `gnome/groundhog/src/app/gh-inbox-setup.c:679` adds the discovery-relays sources as publication targets; `:829` applies OWN_LIST_PUBLISH (ACCOUNT) to every target. Before this commit the code called `set_url_auth(…, ACCOUNT)` on every target, so the behaviour is unchanged.
- **Scenario:** a discovery relay (a public relay-list aggregator) demands AUTH for EVENTs. Onboarding then asks Nostr Signer to sign in to it as the account. R1 allows the purpose, and the published 10050 already names the account, so little extra is exposed. But the charter §4.3 row for own list publish lists "own write + inbox relays" as destinations, not discovery relays.
- **Suggestion:** either amend §4.3's destinations, or use EPHEMERAL for DISCOVERY-only targets (`roles == GH_INBOX_SETUP_ROLE_DISCOVERY`).

---

## 3. Linux stage in the pre-push gate (`8b49743a`)

### 3.1 Answers to the review questions

**Concurrent pushes.** They are serialised safely:
- `flock` on `/work/.lock` in the per-arch volume is taken *before* the rsync (`linux-gate.sh:89-93`), so a second gate waits instead of mixing trees. `flock` works across containers because they share the Docker VM's kernel.
- Each push gets its own `mktemp -d`, worktrees and log.
- `docker run --init` together with `kill` in pre-push's `cleanup` stops the container when the macOS stage fails; `test-pre-push.sh` checks this with a 60 s mocked Linux stage.

**Disk usage.** Measured on this host:
- `nostrc-linux-gate-arm64` is 0.54 GB (`/work/src` 89 MB, `/work/build` 439 MB), and the amd64 volume is 0.52 GB.
- Each image is about 1.9 GB.

Not a problem, but it is not pruned (G4).

**Docker absent or broken.**
- Docker missing or the daemon down: blocked with a clear message, and `NOSTRC_SKIP_LINUX_GATE=1` is announced on every push (tested).
- Docker failing mid-run (`docker build`, `docker info --format`, `docker run`): `set -euo pipefail` makes `linux-gate.sh` exit non-zero, and `finish_linux` blocks the push and prints the log.
- A daemon that *hangs* hangs the push (G5).

**Can the stage pass while the build is broken?** I found no path:
- The configure and build results are checked explicitly.
- A stale cached tree is refreshed. `rsync -c` without `-t` gives every changed file a new mtime, and every run reconfigures, so globs are re-evaluated.
- The strict flags survive per-target `-Wno-error` (verified with GCC).
- `--no-tests=error` makes an empty selection fail.
- When the rerun list is empty or mis-parsed, the gate fails: an empty `-R "^()$"` selects no tests, which is an error, and fewer reruns than failures is an explicit failure (`:154-158`).
- The one `set -e` pitfall fails closed, but without a message (G2).
- The planted nanosleep error was blocked (§1).

**Does the rerun-once policy hide real failures?** A deterministic failure fails both runs and blocks the push. A load-dependent real bug does get absorbed, and its evidence is thrown away (G1). Hosted CI still runs those tests without reruns, so nothing is lost for master's CI signal. The loss is local signal and diagnostics.

**Is the weak metrics stub safe?** Yes. Details:
- In static archives (the default, `BUILD_SHARED_LIBS` unset), GNU ld and ld64 pull an archive member only for undefined symbols. If `libnostr.a(metrics.c.o)` is pulled for any of its other symbols, its strong definitions override the weak stubs; this is exactly the `gnostr-live-log` case that fails to link on master. If it is never pulled, the stubs are used, as before.
- In shared builds (`debian/rules:48`, `packaging/rpm/nostr-login.spec:746` set `BUILD_SHARED_LIBS=ON`), glibc's dynamic linker ignores weak/strong and resolves by load order. That is unchanged from the old strong stubs, which never conflicted across `.so` files.
- MSVC gets the old strong definitions (`GO_METRICS_STUB` is empty).
- One hazard remains, and it predates this commit: a static binary that references only the four stubbed functions and lists `libnostrgo.a` before `libnostr.a` gets the no-op stubs even when libnostr metrics are enabled. Compiling `metrics_stub.c` into libgo only when no `nostr` target exists in the build would remove it (Info).
- The stub's `long delta` against the real `uint64_t delta` is pre-existing and ABI-identical on LP64.

**Other bundled Linux fixes:**
- `-lm` for `gnostr-test-image-viewer-remote-media`: correct.
- `MARMOT_GOBJECT_STORAGE(...)` cast in `gn-marmot-service.c:227`: correct; the checked cast passes NULL through, so the error path still works.
- relayd readiness now connects instead of `stat()`ing: correct, since bind() precedes listen(). The probe connection is harmless to the peercred assertions, which run afterwards on their own connection.

### 3.2 Findings

#### G1 — Medium (non-blocking) — the rerun absorbs load-dependent failures and discards their evidence
- **Where:** `scripts/linux-gate.sh:142-159`.
- **Mechanism:**
  1. The first smoke run has no `--output-on-failure`, so `/work/ctest.log` holds only names.
  2. The rerun overwrites `/work/build/Testing/Temporary/LastTest.log`.
  3. The absorbed names are echoed into `$tmp/linux.log`, which pre-push prints once and deletes.
  4. The rerun is serial and unloaded, so it systematically forgives the one class of failure only load reveals: real races.
- **Observed:**
  - In my end-to-end run, `groundhog-composer` (11 s) and `groundhog-background` (16 s) ended "Subprocess aborted" on the first run and passed on the rerun. Their assertion output was unrecoverable from the volume.
  - Reproducing under load (`ctest -j28`, 6 runs each, branch and master): `groundhog-background /groundhog/background/no11-locked-start` failed with **`GLib-FATAL-CRITICAL: Source ID … was not found when attempting to remove it`**, a real double `g_source_remove`, not a timing assertion. `groundhog-privacy-e2e` failed `test_pt4_relay_minimization` with `signer.calls == signer_calls (21 == 19)` 2/6 on the branch and 1/6 on master (same assertion, `:1343` vs `:1290`), plus `test_pt4_room` and `test_pt2_no_remote_fetch` on master. `groundhog-conversation-view` failed `compact-and-keyboard` and `scrolling`, and `groundhog-onboarding` `first-run-publishes`, on both.
  - None of these is in beads (`bd search`).
  - All are pre-existing. None is caused by these commits, and hosted CI still sees them. But the local gate now turns them into a one-line notice that nobody sees.
- **Fix:**
  - Run the first pass with `--output-on-failure`, or copy `LastTest.log` aside before the rerun.
  - Write the absorbed names and their output to a persistent file in the volume (for example `/work/flaky-<date>.log`).
  - Print a prominent `!! FLAKY:` line to pre-push's stderr.
  - Policy: a test absorbed twice gets a bead.
- **Follow-up beads to file:** the no11-locked-start GLib-CRITICAL and the PT-4 signer-count flake.

#### G2 — Low — a ctest run that dies before its summary makes the gate exit 1 with no message
- **Where:** `scripts/linux-gate.sh:143`, `first="$(grep -E "tests passed" /work/ctest.log)"` under `set -e`.
- **Scenario:** `xvfb-run` cannot start, `dbus-run-session` fails, or ctest crashes. `ctest.log` then has no "tests passed" line, `grep` exits 1, the assignment inherits that status, and the container exits 1 before any echo. Pre-push reports "Linux build failed" and the log's last line is "built all targets", which misleads.
- **Fix:** `first="$(grep … || true)"`, and when `failed` is empty, `tail -40 /work/ctest.log` plus a clear "SMOKE RUN FAILED TO COMPLETE".

#### G3 — Low — the Linux stage runs the caller's `linux-gate.sh`, not the candidate's
- **Where:** `scripts/pre-push:122` runs `"$repo_root/scripts/linux-gate.sh"`, while the static checks are the candidate's (`:106-110`).
- **Scenario:** a branch that changes `linux-gate.sh` (say, a new smoke exclusion) is validated by the version in the checkout the hook runs from. Pushing from a checkout that predates the script gives exit 127, reported as "Linux build failed for …".
- **Fix:** prefer `"$linux_src/scripts/linux-gate.sh"` when it exists, or document that the gate logic is the caller's, as the hook itself is.

#### G4 — Low — images and volumes are never pruned
- **Where:** `scripts/linux-gate.sh:56`, `scripts/groundhog-linux-ci.sh`.
- **Scenario:** every Dockerfile change leaves a dangling ~1.9 GB image per arch. Renaming the helper's image from `groundhog-linux-ci` to `nostrc-linux-ci` orphans the old `groundhog-linux-ci` and `groundhog-linux-ci-amd64` images (1.5 GB each on this host). The only cleanup mentioned is `docker volume rm`.
- **Fix:** a sentence in AGENTS.md, and `docker image prune -f --filter label=nostrc-linux-ci` after a rebuild (add a `LABEL`).

#### G5 — Low — no timeout on `docker info`
- **Where:** `scripts/pre-push:77`, `scripts/linux-gate.sh:43`.
- **Scenario:** a wedged Docker Desktop (a known state after sleep or resume) makes `docker info` block, so `git push` hangs forever instead of failing with the "Docker is not available" message.
- **Fix:** bound it with a background job and `kill` after about 20 s (macOS has no `timeout(1)` by default).

#### G6 — Info — scope of `check-linux-ci-packages.py`
- `MIRRORED` omits `nostr-homed-ci` (fuse3, libfuse3-dev, libgirepository1.0-dev, sqlite3), `libnostr-gi` and `bp-matrix`. The components those workflows build are off by default (`ENABLE_NOSTR_HOMED`, the FUSE options, GI), so the stage's "every default target" is not weakened today; if a default flips, the image silently configures the component out.
- `PACKAGE_RE` silently skips version-pinned tokens (`pkg=1.2`). Consider failing on a token it cannot classify.

#### G7 — Info — the in-container script has no automated test
- `scripts/test-pre-push.sh` mocks `docker` completely. So the flock, the failed-name parsing (`sed … s/^[[:space:]]*[0-9]* - \([^ ]*\) (.*/\1/p`) and the rerun count check are exercised only by real runs. A fixture `ctest.log` fed to the parsing, as a shell function, would pin it.

---

## 4. Unsequenced-argument check (`3963ab46`)

### 4.1 Answers to the review questions

**False negatives on the known shapes?** None. Both historical bugs are fixtures, and so are these variants: a member (`self->task`), a dereference (`task->x`), `g_clear_*`, casts on the out-param, `&(v)`, an index, a nested outer call, `++`/`--`/assignment, two writers, and a macro body. The planted `gh-net-http.c` regression is caught, with a message that says what to do.

Documented, acceptable misses:
- a struct out-param whose field a sibling reads (`use(fill(&range), range.start)`);
- aliasing (`&self->priv->x` beside `priv->x`);
- side effects without `&` (`g_object_unref(x)` beside `x`).

**Is the allowlist justified?** Yes, each exception checked at its site:
- `libmarmot/tests/test_commits.c` `CHECK`/`err` (5 sites): `CHECK(cond, ...)` evaluates `cond` in an `if` and passes the varargs to `fprintf` only after that, so `err` is read after `deliver(..., &err)`.
- `test_store_conversations.c:632`: `room(&f, ap)` in both arguments only reads `f->model`.
- `libgo/examples/arrays_demo.c` (`ia`, `sa`): the getters only read.

Stale or unjustified exceptions fail the check, which is good.

**CI wiring:** the check runs in `static-checks.yml` and `groundhog-ci.yml` (`--self-test`) and in pre-push from the candidate. On master it is clean.

### 4.2 Findings

#### U1 — Low — exceptions are keyed per (file, callee, variable), so each covers future calls too
- **Where:** `scripts/check-unsequenced-args.py:76-87` (key) and `:329-333` (match).
- **Scenario:** a new, genuinely unsequenced `CHECK(x, "%d", deliver(..., &err), err)`-shaped call, or a `printf("%d %d", int_array_pop(&ia), ia.size)`, added to those files later is exempt without review. The `CHECK`/`err` key already covers 5 sites.
- **Fix:** add the expected count, or a normalised-snippet hash, to each key, so a new matching site fails as "exception covers N+1 sites".

---

## 5. Summary

| # | Severity | Commit | Where | Short |
|---|---|---|---|---|
| G1 | Medium | 8b49743a | scripts/linux-gate.sh:142 | rerun absorbs load races (incl. a real GLib-CRITICAL) and discards their output |
| A1 | Low | 82dbe984 | gh-outbox.c:1783, gh-account-auth.c:266 | no "Waiting for approval" when the self-copy AUTH queues behind another prompt |
| A2 | Low | 82dbe984 | gh-relay-auth.c:392 | AUTH created_at fixed at attempt start; a paused, queued attempt can exceed the 600 s skew |
| G2 | Low | 8b49743a | scripts/linux-gate.sh:143 | `set -e` exits silently when ctest dies before its summary |
| G3 | Low | 8b49743a | scripts/pre-push:122 | caller's linux-gate.sh, candidate's static checks |
| G4 | Low | 8b49743a | scripts/linux-gate.sh:56 | no image/volume pruning; renamed image orphaned |
| G5 | Low | 8b49743a | scripts/pre-push:77 | `docker info` without timeout can hang a push |
| U1 | Low | 3963ab46 | scripts/check-unsequenced-args.py:76 | exceptions cover future calls in the same file |
| A3 | Info | 82dbe984 | gh-message-status.c:260 | WAITING_FOR_SIGNER can head a message whose delivery doesn't need the approval |
| A4 | Info (pre-existing) | — | gh-inbox-setup.c:679,829 | account AUTH on discovery relays in inbox setup vs §4.3 destinations |
| G6 | Info | 8b49743a | scripts/check-linux-ci-packages.py | unmirrored workflows; pinned packages skipped |
| G7 | Info | 8b49743a | scripts/test-pre-push.sh | in-container logic untested |

No blocking findings:
- **AUTH:** the identity mapping is correct, and no account key reaches a non-own relay through new code. The deadline pause is bounded by the signer's 330 s call and by cancellation or revoke, and the tests fail without each fix.
- **Gate:** it blocks the nanosleep and steal-and-read classes it was built for. I found no swallowed-error path that lets a broken build pass.
- **Weak stubs:** safe.
- **Unsequenced check:** it catches the known shapes, and its allowlist is justified.

**APPROVED**
