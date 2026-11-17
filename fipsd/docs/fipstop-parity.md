# fipstop compatibility and verification

## Pin and screen comparison

Upstream **6aa80504baa0fa312b392009eea0e3e59fb6fe4c** matches
libfips-core/UPSTREAM.md (fipstop 0.6.0-dev). The C component is independently
versioned 0.1.0 Unreleased.

The requested upstream build succeeded in 33.90s, with one upstream dead-code
warning, using the read-only source checkout and an external Cargo target:

    CARGO_TARGET_DIR=/tmp/fips-rust-target-fipstop cargo build --locked --release --bin fipstop

Python pyte was unavailable. Installed **tmux** captured both programs in
isolated fixed 80×24 and 120×40 terminals. Both queried the **same live fipsd**
and a private absent gateway path. A second comparison served the committed
control_golden replies from a fixture socket. Control-output events drive
capture readiness: no sleeps or timed screen polling. Clients, private tmux
server and live daemon are stopped before return.

| Capture | Screens | Exact pane bodies | Remaining body difference | Footer differences |
| --- | ---: | ---: | --- | ---: |
| Live idle daemon, final comparison | 20 | 19 | Sessions 80×24: 6 cells | 0 |
| Fixture node, final comparison | 20 | 19 | Sessions 80×24: 6 cells | 0 |
| Fresh fixture-node process, rounding check | 20 | 20 | None | 0 |

The last measurement checks an observed upstream layout alternative, not a
retry used to erase a failure. Rust's Sessions header allocates the boundary
between Remote Addr and State one column differently in fresh processes at
80 columns. Both allocations were captured; C is deterministic. This is a
remaining one-column layout-rounding difference, not a claim of universal
byte-for-byte screen equality. All other pane bodies match at both sizes.

The version title intentionally differs (C 0.1.0 and C revision versus Rust
0.6.0-dev and upstream revision); navigation text matches. Initial C differences
were fixed: compact columns, State alignment, transport instance/bind/peer
columns and scrollbar, three-dot hex truncation, aligned key labels, compressed
Tree layout, empty-filter wording, and footer priorities.

### Evidence boundary

These are **resting overview text screens**, not exhaustive populated,
modal/detail or true-colour parity. Fixtures have no peers, sessions or running
gateway; the live node has one idle UDP transport. Populated selection,
multipath rows, grouping, numeric sorts/missing keys, logical cursor identity,
graph samples, collapse-parent identity, detail scrolling and confirmation have
source/unit coverage, not a populated Rust/C screenshot claim.

C detail views expose structured fields and joined link/transport data instead
of reproducing every ratatui detail layout. C uses portable ncurses colour pairs,
not identical RGB gradients. Wide curses is preferred; a symbol link check
prevents narrow-only curses from accidentally referencing wide symbols. The
narrow fallback is structurally provided, not separately qualified on Linux.

## Module map and reuse

| Module | Ownership |
| --- | --- |
| main.c | CLI/version, terminal/termios lifetime, monotonic refresh, resize, signal cleanup, hidden single-frame mode |
| app.c/app.h | Owned snapshots, adapter quiet state, active-tab/cross-reference queries, transport identity, confirmed disconnect |
| model.c/model.h | JSON fields, formatting/truncation, stable role/LQI order, history/null samples, linear resampling |
| event.c | Tab/mode keys, focus/scroll, graph selectors/sorts, confirmation and shared footer/help context registry |
| ui_common.c/ui.h | Unicode canvas, clipping, tables/borders, fields/details, footer/help/modal/scrollbars |
| ui_node/peers/transports/sessions/tree/bloom/mmp/routing/graphs/gateway.c | The ten panes |

The existing fipsctl_lib already supplies the shared boundary; no refactor or
fipsctl source/behavior/version change was needed. Reused: client.c socket and
adapter/symlink resolution, deadlines and status composition; render.c JSON,
tagged-u64, UTF-8 and sparkline routines; npub.c address conversion. Histories
are already daemon-computed rates: upstream's app does not differentiate byte
counters locally. No daemon/control schema, relay, sibling packaging/workflow,
mDNS, advert or tick-profiler files changed.

## Verification

Native host: macOS arm64. Debug and GO_WARNINGS_AS_ERRORS=ON throughout.
Apple Clang's builtin fortify definition requires
CMAKE_C_FLAGS=-U_FORTIFY_SOURCE and CMAKE_ASM_FLAGS=-U_FORTIFY_SOURCE before the
project's own definition; warnings-as-errors and instrumentation stay enabled.

Build closure: fipsd_tests, fips_core_host_test, fips_core_skeleton_test.
Full command in each sanitizer build directory:

    ctest -R '^Fips' -E 'Soak|Perf' --output-on-failure -j 6

| Instrumentation | Selected | Passed | Capability skips | Failed | Full run |
| --- | ---: | ---: | ---: | ---: | ---: |
| ASan + UBSan | 195 | 186 | 9 | 0 | 41.83s |
| TSan | 195 | 186 | 9 | 0 | 67.67s |

Skip list: ControlInterop, TunPrivilegedSmoke, InteropUdp, InteropUdpV6,
TcpInterop, Ipv6PrivilegedSmoke, InteropMulti, FipsctlInterop and TorInterop.
These need optional Rust/tor binaries or privileged platform capabilities.
They are not counted as passes. This is local native evidence, not hosted CI
or Linux deployment proof. Final focused FipsTop suites were rerun after the
CLI/curses/locale corrections.

FipsTopModelTest, FipsTopTest, FipsTopGoldenTest and FipsTopE2eTest cover
formatting/truncation/order/rates/nulls; every pane and size; navigation families;
stable confirmation/cursor targets; numeric sorting/missing keys; transport
collapse identity; adapter sibling/symlink/composition/fallback/exact ten-fetch
quiet; live/disconnected frames; real ncurses no-colour, resize, q, SIGTERM,
SIGHUP and terminal restoration; equals/attached flags and invalid dimensions.
Goldens also run under LC_ALL=C to verify UTF-8 fallback and locale-independent
numeric output. PTY waits drain through endwin before waiting for process exit.
Only kernel pending-input status PENDIN, not an actual terminal setting, is
excluded from termios equality.

Direct static checks: check-unsequenced-args.py, check-gui-test-gdk-frame.py,
check-linux-ci-packages.py and git diff --check on working/branch changes.
The one whitespace finding was removed. No sleeps, suite serialization,
sanitizer suppression or failure retries were introduced.

Read-only Jev discovery: 19 requests, about 142k input tokens, approximately
$0.006. Judgments were triage and checked against source. The single inline
review was scoped to supplied worktree evidence because this checkout is
outside RepoPrompt's loaded roots. Both concrete findings (combining-cell
width and collapse selection) were fixed and regression-tested.

## Packaging and handoff

The workflow owner needs libncurses-dev (Debian/Ubuntu) or ncurses (Alpine) in
package lists. This task changes documentation only, not workflows.
Changes and commits stay in the assigned feature/fipstop worktree, local-only:
no merge or push. Task-owned builds/captures/Cargo output are removed and
submodules deinitialized at handoff. See cli-fipstop.md for pane/key coverage.
