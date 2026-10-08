# Groundhog conversation-open iteration 3: persistent-list gate

Bead: `nostrc-yu968`. Parent: `24b9417c` on `groundhog/perf-conversation-open`. Host: Mac16,6, macOS 27.0.1, GTK 4.24.1, native GDK display. **Decision: reject the proxy; no shipping code change.** Component version: **no bump** (investigation/documentation only).

## A→B→A row-identity gate

A temporary mapped 650×700 GTK window held one persistent `GtkNoSelection` and a private proxy implementing `GListModel` plus `GtkSectionModel`. The proxy synchronously disconnected its old 1,000-item source, retained the new source, emitted one `items-changed(0, 1000, 1000)` splice, and released the old source. The list used a `GtkSignalListItemFactory` and scrolled to item 999; the factory was installed after setting the initial opening anchor, as in the production view. Every setup assigned a unique monotonically increasing ID to its child widget. Counts after 500 ms of main-loop settling:

| Phase | Factory setup | bind | unbind | teardown | Distinct row IDs | IDs shared with preceding phase |
|---|---:|---:|---:|---:|---:|---:|
| Initial A | 206 | 206 | 0 | 0 | 206 | — |
| B, full-range swap | 206 | 206 | 206 | 206 | 206 | 0 |
| Return to A, full-range swap | 206 | 206 | 206 | 206 | 206 | 0 |

The initial and returned A sets also had zero IDs in common. Destroying the window brought total teardown to 618/618 setups. A simpler direct `GListStore` full-range splice gave the same 206/206 teardown/setup and zero ID intersection. Thus GTK 4.24 recreates rather than rebinds these row widgets for a whole-range replacement. Keeping the selection/model objects alive does **not** save the observed row construction/disposal cost. No proxy was added to Groundhog.

## Alternative attribution: attach→first paint

Temporary, non-shipping test instrumentation wrapped GTK label and list-view `measure`/`size_allocate` vfuncs and `GtkBoxLayout` measure/allocate for `GhMessageRow` and `GhTimelineRow`. Collection started at the existing `attach_end_us` marker and stopped at the first `after-paint`. The opt-in mapped-window timing harness used one warm-up and one measured A→B→A pair at each size; figures below are the **single measured 1,000-message pair**, not a performance distribution. The existing endpoint settled at 239.4 ms first-open and 250.0 ms re-open.

| 1,000-message phase | Attach→first paint | List-view measure | Message-row measure | Timeline-row measure | Label measure | Label vertical HFW | Row allocate | List-view allocate |
|---|---:|---|---|---|---|---|---|---|
| First open | 97.8 ms | 4 calls / 61.4 ms | 615 / 60.7 ms | 615 / 60.8 ms | 1,705 / 62.5 ms | 409 / 8.2 ms | 9 / 6.0 ms | 2 / 7.3 ms |
| Re-open | 86.4 ms | 4 / 60.6 ms | 615 / 59.8 ms | 615 / 60.0 ms | 1,701 / 62.0 ms | 409 / 8.0 ms | 9 / 4.9 ms | 2 / 6.1 ms |

These are **nested wall spans, not additive costs**: list-view measure includes row measure, and row measure includes much of label measure. `GtkBox` rows use `GtkBoxLayout`, which is why the layout-manager vfunc—not the row widget's vfunc—was wrapped. Each of the 205 cached message rows was measured three times before first paint; only nine received allocation. The 409 label height-for-width calls account for about 8 ms of the roughly 62 ms label-measure span, so HFW is not the sole culprit. The first-paint interval also includes frame scheduling and other GTK work; the instrumentation does not establish exclusive CPU time or a new optimization's likely speedup.

Raw temporary gate C, output, timing CSV, and CTest log are in `/Users/bizarro/Documents/Projects/nostrc/prompt-exports/` under `conversation-open-opt3-gate-*`. The gate program and vfunc wrappers were **not** included in the shipping build. The opt-in timing CTest passed. After restoring the uninstrumented test source, `ctest --test-dir _build-conversation-open -R '^groundhog' --output-on-failure -j6` passed **102/102** (five expected environment skips). `git diff --check` passed.
