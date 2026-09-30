# Peer Review — W21 Testing/ on Linux, relay callback lifetimes outside nostr-gobject, nip55l contract pacing

**Branch:** `infra/w21-lifetimes-tests`, 3 commits on `b5ca25bf`, reviewed on `review/w21-lifetimes-tests` at `b41322a2`:

| Commit | Bead | Subject |
|---|---|---|
| `4ffd368b` | nostrc-ig0z | build: add the mock relay framework from Testing/, so Linux builds it |
| `33929703` | nostrc-tw7f | fix: relay callback data outlives calls in flight in signet, nd-fetch, nip46 and neg-client |
| `b41322a2` | nostrc-oauv | test(nip55l): contract test paces prompts by the daemon, tears down on failure |

**Reviewer:** independent peer reviewer (AGENTS.md, "Peer Review (REQUIRED)")
**Date:** 2026-09-30
**Verdict:** **APPROVED.** There are no blocking findings. Three Low findings and one Nit follow; none needs to land before merge.

This review changed no source files and no beads. Mutations, probes and build trees live under `/tmp` and are not committed.

---

## Verification performed

| # | What | Result |
|---|---|---|
| 1 | macOS: `cmake -S . -B /tmp/w21rv -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w21rv && ctest --test-dir /tmp/w21rv -j6 --timeout 300` | Build clean (2433 steps). **439 registered, 434 passed, 5 skipped** (`test_nip5f_tcp`, `groundhog-launch`, `groundhog-store-key-keyring`, `groundhog-background-gui`, `groundhog-notifier-gui`, all environment-gated). The build rewrote `apps/gnostr/data/ui/dialogs/gnostr-profile-edit.ui`; restored with `git checkout` |
| 2 | `scripts/linux-gate.sh` (arm64, Ubuntu 24.04, GCC, strict CFLAGS) | **435 smoke tests passed**, no rerun. The per-test log in the gate volume shows the newly built tests ran on Linux: `TestHarnessExample`, `MockRelay{ServerBasic,Integration,Unit}`, `SignerIntegration`, `Nip46MockRelay`, `Nip46QRSocket`, `test_nostr_gobject_relay_{connect,teardown}`, `gnostr-test-plugin-{publish-ack,raw-relay-api}`, plus `relay_pool_ok_lifetime` and `nip55l_dbus_contract_fail_teardown`. The volume's `ctest-rerun.log` (publish-ack failing) is from the author's earlier run, 01:58 UTC, an hour before this one |
| 3 | macOS ASan+UBSan (`-DCMAKE_BUILD_TYPE=Debug -DGNOSTR_ENABLE_ASAN=ON -DGNOSTR_ENABLE_UBSAN=ON -DENABLE_NOSTR_DISPATCHER=ON`), the touched tests | **16/16 passed**: `relay_pool_ok_lifetime`, `nip55l_dbus_contract`, `nip55l_dbus_contract_fail_teardown`, both plugin API tests, the 7 `Testing/` tests, `test_nostr_gobject_relay_{connect,teardown}`, `test_nip46_pending_lifetime`, `test_nip46_glib_lifetime`. Also the 4 nostr-dispatcher tests (with `nd_portable`, which holds `nd-fetch.c`, built under ASan): **4/4** |
| 4 | `relay_pool_ok_lifetime` under ASan, 30 consecutive runs | **30/30** pass |
| 5 | **Pre-fix, tw7f (publish + AUTH):** `33929703^:signet/src/relay_pool.c` compiled alone, with two shims defining `signet_relay_pool_watch_{publish,auth}_ok()` as the old registration (plain `nostr_relay_set_ok_callback()` on a `g_new0` `PublishOkCtx` / `PostAuthResubData`, handlers unchanged from the old code). Swapped into a copy of `libsignet_core.a` and linked with the unmodified test. `MallocScribble=1 MALLOC_PERTURB_=165` | **3/3 abort** (exit 134, malloc double-free detection, publish path first) |
| 6 | **Pre-fix, tw7f (AUTH only):** the same, with `main()` running only `test_auth_ok_parked_while_removed` | **3/3 SEGV** (exit 139). Fixed code: 3/3 pass. This confirms the commit's "double free on the publish path, SEGV on the AUTH path, each checked separately" |
| 7 | `python3 scripts/check-unsequenced-args.py` | `No call modifies and uses a variable in different arguments` |
| 8 | Leftover processes after the nip55l runs (`pgrep dbus-daemon / nostr-signer-daemon / test_nip55l_dbus_contract`) | None from these tests. The only `dbus-daemon` alive belonged to a concurrent `test-groundhog-mls-service` from another session (its own `nostrc-test-bus-lifeline` wrapper) |

---

## Findings summary

| ID | Severity | Blocking | Location | Summary |
|---|---|---|---|---|
| L1 | Low | No | `signet/src/relay_pool.c:764` (`signet_publish_ok_handler`), `:211` (`signet_ok_response_callback`) | No fire-once guard: a duplicate OK handled concurrently runs the user callback twice (publish) or schedules two re-subscribes (AUTH) |
| L2 | Low | No | `CMakeLists.txt:358` | `BUILD_TESTING_FRAMEWORK` defaults ON independently of `BUILD_TESTING`, so default-option Linux packages now compile the framework's tests and `mock-relay` |
| L3 | Low (pre-existing, out of scope) | No | `signet/src/relay_pool.c:227, 399` | The AUTH idle payloads carry raw `NostrRelay *` / `SignetRelayPool *` across `g_idle_add()`. A reconfigure or `signet_relay_pool_free()` before dispatch leaves them dangling. Untracked |
| N1 | Nit | No | `libgo/Testing/Temporary/`, `tests/Testing/Temporary/` | Two more committed `CTestCostData.txt` files, now matched by the new ignore rule |

---

## nostrc-ig0z (`4ffd368b`): Testing/ on Linux

**Root CMake.** `add_subdirectory(Testing)` with no `EXISTS` guard is right. The directory is tracked, and a missing one should fail configure rather than silently drop every `TARGET nostr_testing` consumer. That silent drop was the bug. `Testing/src/test_harness.c`'s fallback fixture path now matches the directory's case.

**Packagers.**
- *Install rules:* all three (`nostr_testing`, the headers under `include/nostr/testing`, `fixtures/`) are behind `NOSTR_TESTING_INSTALL`, default OFF. A default-option `cmake --install` on Linux ships nothing new. Checked against the recipes that install the whole tree with defaults: `packaging/archlinux/PKGBUILD`, `snap/snapcraft.yaml`, the gnostr flatpak, `flake.nix`, Homebrew. Debian (`debian/rules:73`) and RPM (`nostr-login.spec:772`) already pass `-DBUILD_TESTING_FRAMEWORK=OFF`.
- *Build dependencies:* none new. `Testing/` REQUIREs OpenSSL, secp256k1, libwebsockets and jansson, and the default tree already REQUIREs all four (`libjson/CMakeLists.txt:24,27` for jansson and libwebsockets, `libnostr` and `nostr-gobject` for libwebsockets).
- *Build work:* see **L2**.

**`apps/gnostr/tests/plugin_api_app_stubs.c`.** Correct, and it hides no real link error. It *removes* the mechanism that did.
- It defines exactly the 19 symbols the commit names: two `GType`s, 4 main-window, 4 repo-browser, the shared query pool and 8 signer-service functions. Before, `-undefined dynamic_lookup` / `--unresolved-symbols=ignore-in-object-files` let *any* unresolved symbol through. Now both tests link completely, so a new app-shell reference from `gnostr-plugin-api.c` fails at link time, and a symbol that later moves into a linked library fails as a duplicate. Both are loud.
- The behaviour is sound for what the tests reach. `GnostrMainWindow` / `GnostrRepoBrowser` are real empty final types, so the `GNOSTR_IS_MAIN_WINDOW()` / `GNOSTR_IS_REPO_BROWSER()` guards at `gnostr-plugin-api.c:493–573, 1952–1995` are well defined. The signer getters return NULL/FALSE, matching every call site's `is_available` check (`:1938, 2038, 2098, 2287`). Everything else calls `g_error()`.
- `gnostr_get_shared_query_pool()` aborting is right. The only path is `request_relay_events_async` with no context pool (`:1096`), which neither test calls; their `query_relays_async` uses the context's pool.
- The main-window type comes from `gnostr-main-window-private.h`, so a layout change there recompiles the stub.

**`.gitignore`.** `**/Testing/Temporary/` replaces two bare `Testing` lines. `git check-ignore` confirms `Testing/src/*` and `libgo/Testing/*` outside `Temporary/` are no longer ignored, and `Testing/Temporary/x` is. `git status --ignored` shows nothing new exposed in this tree. The only `Testing` directories are the framework and the two CTest scratch directories under `libgo/` and `tests/` (N1). Every file under `Testing/` was already tracked (force-added past the old rule), so nothing untracked becomes visible.

**Gate.** Removing `gnostr-test-plugin-raw-relay-api` from `BROKEN_ON_LINUX` is justified: it passed in the Linux gate. The smoke set is "all minus `^(SLOW|BROKEN)$`", anchored. So `nip55l_dbus_contract_fail_teardown` runs in the smoke set while `nip55l_dbus_contract` stays in `SLOW_TESTS`. That is reasonable: the fail-teardown case takes about 0.4 s.

### L2 — the framework builds whenever `BUILD_TESTING_FRAMEWORK` is ON, regardless of `BUILD_TESTING`

`option(BUILD_TESTING_FRAMEWORK ... ON)` does not follow `BUILD_TESTING`, and `Testing/CMakeLists.txt` adds its 7 test executables and the `mock-relay` tool unconditionally. Arch, the snaps and `flake.nix` configure with the defaults. Their Linux builds now compile and link `nostr_testing`, 7 tests and `mock-relay` that they never install or run (`snap/snapcraft.yaml:47` builds `all`). It is harmless, since nothing is installed and there are no new dependencies, but it adds build time and a failure surface to release builds. It is also why Debian and RPM carry an explicit `-DBUILD_TESTING_FRAMEWORK=OFF`.

*Suggestion:* `option(BUILD_TESTING_FRAMEWORK "..." ${BUILD_TESTING})`, or `cmake_dependent_option`. The macOS default is then unchanged (BUILD_TESTING is ON by default via CTest), and packagers that set `-DBUILD_TESTING=OFF` get neither.

### N1 — two more committed CTest cost files

The commit drops `Testing/Temporary/CTestCostData.txt`, but `libgo/Testing/Temporary/CTestCostData.txt` and `tests/Testing/Temporary/CTestCostData.txt` are still tracked. The new `**/Testing/Temporary/` rule matches both. Tracked files are unaffected by ignore rules, so they will still show as modified after an in-source ctest run there. Suggest `git rm --cached` both in a later cleanup.

---

## nostrc-tw7f (`33929703`): callback data outlives calls in flight

**libnostr contract relied on** (`libnostr/src/relay.c:191–238, 2161–2352, 690–705`):
- Each registration is a slot with an atomic refcount. Dispatch takes an invocation reference under `priv->mutex` and drops it after the call.
- Replace and remove swap the field under the mutex and drop the registration reference outside it. Destroy runs on the last unref, never with the mutex held.
- `nostr_relay_free()` drops all three slots.

So "remove myself from inside my own callback" is safe: the running call holds its own reference, and `destroy` runs when that call returns. Every site below relies on exactly this.

### signet `relay_pool.c`

- **Publish OK (`PublishOkCtx`).** `signet_relay_pool_watch_publish_ok()` hands the relay a `g_new0` context with `g_free` as destroy. The handler never frees and ends with `nostr_relay_set_ok_callback(relay, NULL, NULL)`. There is no double free (only the slot frees) and no leak: a replaced registration is now freed by the relay, where the old code leaked the replaced `PublishOkCtx`. `test_publish_ok_replaced_and_freed` covers both replaced and never-answered registrations under ASan. The removed `saved_ok_cb` / `saved_ok_data` were always NULL (set so at the sole install site), so dropping the "forward to saved callback" branch changes no behaviour.
- **AUTH OK (`AuthOkWatch`).** The old `PostAuthResubData` did double duty. It was the idle payload *and* the OK callback data, and it was freed by whichever path ran first. Now the idle payload (`PostAuthResubData`) is always wiped and freed by `signet_send_auth_idle()` via `post_auth_data_free()`, including the sign-failure and `write`-NULL paths that used to leak or double-handle it. The OK callback data (`AuthOkWatch`) is relay-owned. The re-subscribe idle gets a by-value copy (`*resub = *rd`) and frees it. `AuthOkWatch` has no `sk_hex`, so no key material reaches the relay-owned copy. I verified that `signet_relay_auth_callback` copies everything it needs from `SignetAuthCallbackData` into the idle payload and keeps no pointer to it, so the relay may free `d` at any time after the call.
- **Per-relay AUTH callback data (`auth_cb_data` / `old_auth_cb_data` removed).** The arrays had exactly one job: free and wipe the `SignetAuthCallbackData` handed to each relay, at `signet_relay_pool_free()` and after a reconfigure's old pool is freed. `signet_auth_callback_data_free()` now does the same wipe, `g_mutex_clear` and `free` as destroy, and both teardown points still reach it: `nostr_simple_pool_free()` → `nostr_relay_free()` (`simplepool.c`) → slot unref. The ordering is at least as safe as before, since destroy waits for an in-flight AUTH callback. It is also strictly better on re-registration. `signet_relay_pool_register_auth()` runs at start (`:498`) and after reconfigure (`:982`), and a second registration on the same relay now frees the first `d` instead of accumulating it in the array. The arrays had no other readers; I grepped all uses before and after.
- **Thread safety.** Slot swaps are under the relay mutex, and the handlers touch only their own immutable context plus `g_idle_add` (thread-safe). `SignetAuthCallbackData` keeps its own `challenge_mu` / atomic `auth_sent`.

### nd-fetch, nip46 `client_start`, neg-client

- **nd-fetch `wait_established()`.** It creates the channel (ref 1) and registers with `go_channel_ref(ready)` plus `go_channel_unref` as destroy. It waits, removes the callback and drops its own ref, so a straggling `on_state()` `try_send`s on a live channel. The two copies are now one helper with identical semantics. `nd-fetch.c` is only in `nd_portable` behind `ENABLE_NOSTR_DISPATCHER` (default OFF); it compiled and the dispatcher tests passed under ASan (row 3).
- **nip46 `nostr_nip46_client_start()`.** Each relay's state slot holds its own channel ref. `go_channel_close()` followed by `go_channel_unref()` means a late `try_send` hits a closed-but-live channel and fails harmlessly. Covered by `Nip46MockRelay` and `Nip46QRSocket` (Linux gate and ASan) and the nip46 lifetime tests.
- **neg-client.** `NegSessionContext` moves from the stack to a `g_atomic_rc_box`, with one ref for the task and one for the AUTH slot via `g_atomic_rc_box_acquire` plus `neg_session_context_unref`. Every `goto cleanup` (lines 681, 707, 793, 798) comes after the context is created (669), so the unref never sees an uninitialised pointer. The old code's `neg_session_context_clear(&neg_ctx)` on a stack struct had the same jump constraint.
  - The sub-id registry borrows the context safely. `neg_handler()` uses it only under `g_neg_sessions_mu`, and the task unregisters (taking that mutex) before dropping its ref. Both exit paths clear the AUTH callback and free the relay before or around the unref, and the slot's ref keeps a running `neg_auth_callback()` valid either way.
  - The handshake channel follows the nd-fetch pattern.

### The regression test

`signet/tests/test_relay_pool_ok_lifetime.c` is a real regression test.
- It drives OK frames through `nostr_relay_dispatch_control_envelope()`, the same seam as `libnostr_relay_callback_lifetime`. It parks one call mid-callback, runs a duplicate to completion (which removes the callback), then resumes the parked call.
- Every wait is bounded, and hitting the bound is a `CHECK` failure.
- Rows 5–6 confirm it **fails on the old code** on both paths without ASan, thanks to `MallocScribble` / `MALLOC_PERTURB_` in the test environment. It is registered in both CMake and meson.
- The shim I used matches the old registration exactly (plain setter, old handlers verbatim), so this is the old code's behaviour, not an approximation.

### nostrc-izf9 (publish-ack replacing a pending AUTH OK watch)

This is correctly filed as pre-existing and not a regression. A relay has one OK slot. The old code had the same single slot, and its "chain to saved callback" was dead because the saved pointers were always NULL. tw7f makes the replaced watch's *data* safe (freed by the relay instead of leaked) but, as izf9 says, does not fix routing.

Exposure today is nil. The only production caller of `signet_relay_pool_publish_event_json_ack()` is `nip46_server.c:1140`, which passes `cb = NULL`. The `if (cb && eid)` guard then installs no watch, so a publish never evicts the AUTH watch in shipped signetd.

izf9's proposed fix, a signet-owned per-relay OK dispatcher with remove-by-id, is the right shape. It would also resolve **L1** and the "remove whatever is registered now" hazard of the unconditional `set_ok_callback(NULL)`.

### L1 — no fire-once guard on the OK handlers

The scenario the new test constructs (a duplicate OK while the first call is parked) makes both calls pass the event-id match. The test asserts it (`gate_calls == 2`).
- **Publish:** `user_cb` runs twice for one relay. `relay_pool.h` documents "fires once for each relay that responds". A future caller that frees `user_data` in the callback would double free.
- **AUTH:** two OK-true frames schedule two `signet_post_auth_resubscribe` idles, so two re-subscribes. An OK-true followed by an OK-false both log.

This is not a regression: the old code had the same double dispatch, plus the double free. Today there is no production publish caller with a callback.

*Suggestion:* a `gint fired` in `PublishOkCtx` / `AuthOkWatch` claimed with `g_atomic_int_compare_and_exchange(&ctx->fired, 0, 1)` before acting, or fold it into izf9's dispatcher. Add it to izf9's description so it isn't lost.

### L3 — raw pointers across `g_idle_add()` in the AUTH flow (pre-existing, untracked)

`signet_relay_auth_callback` schedules `signet_send_auth_idle` with `rd->relay` / `rd->pool` (`:399`), and the OK handler schedules `signet_post_auth_resubscribe` with a copy carrying the same pointers (`:227`). Neither holds a reference. Two sequences leave the idle dereferencing freed memory:
- a `signet_relay_pool_set_relays()` reconfigure whose worker frees the old pool between schedule and dispatch: `nostr_relay_write()` or `signet_relay_pool_watch_auth_ok()` on a freed relay;
- `signet_relay_pool_free()` at shutdown with an idle pending: `rp->mu` on a freed pool.

The window is narrow and this predates W21; tw7f neither widens nor narrows it. `bd search` finds no bead. **File one**: take a relay ref (the pool's generation or `nostr_relay_ref` if available) and check `rp->disposing` / generation in the idle.

---

## nostrc-oauv (`b41322a2`): nip55l contract test

**Is it still a contract test?** Yes, and a stricter one.
- The cap block paces by the daemon, not the wall clock. Iteration *i* sleeps 120 ms, sends, then (for *i* < 8) blocks in `watch_wait_request(&w, i+1)` until the daemon has *announced* prompt *i+1*. So each call leaves at least 120 ms after the daemon recorded the previous prompt, and the 100 ms per-sender prompt limit cannot trigger regardless of daemon latency.
- `watch_wait_request()` also fails loudly ("call answered without a prompt") if a call is answered instead of parked.
- The 9th call must now carry the cap-specific message `too many of this application's requests are awaiting approval`. Before, any `RateLimited` passed, including the per-sender limit, which was the false-pass mode.
- The assertions on 8 parked requests, one error reply and denying the eight by id are unchanged.
- The `g_usleep(120 ms)` before the no-agent probe is a spec'd floor on spacing *after* the last prompt was observed, not a synchronisation sleep, so it is not the timeout smell. The daemon's rate limit is itself time-based, and the file already uses the same pattern in `interactive()` and elsewhere (`:854, 1371, 1435, 1660`).

**Is killing the GTestDBus helper safe?** Yes.
- `watcher_kill()` (macOS only) SIGKILLs and reaps direct children whose `proc_pidpath` equals this executable's. The test never forks or re-executes itself: its only children are `g_subprocess_new` for the daemon and keyring (different binaries) and GTestDBus's `dbus-daemon`. So the only match is GLib's forked watcher.
- It runs from `teardown_at_exit()` after the live fixture (if any) is stopped with `g_test_dbus_stop()`, so the watcher has nothing left to clean up.
- On the success path every fixture is already down. The watcher would otherwise crash in `sscanf(NULL)` (the GLib macOS `G_IO_HUP` issue the commit describes), so killing it loses nothing.
- The atexit handler uses no `CHECK` (no `exit()` re-entry). `live_ctx` is cleared by `ctx_teardown()`, so the normal path does not double-stop.
- On Linux the handler is a no-op for the watcher, and `FAIL_REGULAR_EXPRESSION "cleaning up pid"` makes the test fail if the watcher, not the test, had to kill the bus. That is a good oracle.

`nip55l_dbus_contract_fail_teardown` passed on macOS (release and ASan) and in the Linux gate. After all runs, no `dbus-daemon`, `nostr-signer-daemon` or test process from these tests was left alive (row 8).

---

## Versions

- signet 0.1.0 → 0.1.1 (PATCH) is right for an internal use-after-free fix with no API, CLI or config change. `signet/meson.build`, `SIGNET_VERSION` and `SIGNETCTL_VERSION` agree, and it is newly listed in `VERSION_MANIFEST.md`.
- The nostr-dispatcher and NIP-46 "unversioned, no bump possible" and gnostr "unreleased 0.1.0" entries match the manifest's component table.
- ig0z and oauv are build- and test-only; no bump is correct.

---

## Verdict

**APPROVED.**

No blocking findings. Recommended follow-ups:
1. **L1**: extend nostrc-izf9 with the fire-once requirement.
2. **L3**: file a bead for the raw relay and pool pointers in signet's AUTH idles.
3. **L2**: tie `BUILD_TESTING_FRAMEWORK`'s default to `BUILD_TESTING`.
4. **N1**: untrack the two remaining `CTestCostData.txt` files.
