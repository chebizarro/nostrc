# Path test coverage

Oracle: `core/src/proto/path` at the commit in `../UPSTREAM.md`.
The C tests consolidate upstream scenarios rather than preserving one Rust
function per C function. They use the real codecs and Bundle entry points, a
synthetic clock and explicit deterministic entropy; no sleeps or host IO.

- `test_path.c`: fixed wire layouts, length/padding and flags, TLV handling and
  malformed reasons, truncation/capacity rollback; set identity, admission,
  limits/BLE, eviction and protected last-acked Dead, driver handback,
  classification, rebind/roam; parameter rejection; candidate recency/source
  holds, replacement, release/prune/forget; numeric IP scope.
- `test_path_live.c`: ring saturation/order, timely/late/wrapped answers,
  pre-thaw misses, sample window/horizon, ETX, receive-count wrap, dead stop
  floors/recheck/rebase, verdict data deferral, MTU ladder/search, carried MTU,
  score/tier/fit/preferred/medium/pin ranking, load holds, cost holds and
  capability schedule/expiry/matching.
- `test_path_machine.c`: original initialization, capability gating and cross-pair
  ACKs, probe/ACK exchange, exact-deadline vs late answers, frame attribution,
  disabled operation, policy refusal/reoffer, discretionary dwell, mandatory
  departure/recovery return, load damping, pinning/view, authentication trips,
  renewal/close ownership, transactional output capacity, connection opening,
  interface departure/return and peer-active connection retention.
- `test_path_net.c`: a public-interface scripted-answer shell (not the two-peer
  simulator), checking inert early polls, strictly future wakes, action capacity,
  encoded sends and cost continuity. It exercises two-hour capability schedules,
  candidate policy/presence/unlisting gates, active/standby/peer-active cadence,
  exact 1.8s failure detection, the 4.4s data-held verdict versus control frames,
  eight-probe discovery exhaustion, Dead grace and fresh-source revival, active
  recheck recovery, MTU search across a narrow hop and ten-minute restart,
  connection success/failure/exhaustion, score/role/preferred selection, recovery
  return and horizon damping. Dwell ends are asserted against the first score
  lead, using real probe-derived measurements rather than injected liveness.
- `test_path_vectors.c` calls all three checkers directly and rejects a modified
  input in each row. This is not a claim that `cases.c` dispatch is wired.

Not ported: `tests/sim.rs`, `tests/stub.rs` day runs, the Rust allocator-counter
instrumentation and Rust `Display` formatting tests. The source-level C boundary
uses no allocation; that is not a substitute for allocator instrumentation.
The complete set of individual upstream machine scenarios (including the large
`tests/shell.rs` network harness) is **not yet a one-for-one test port**. These
consolidated cases must not be reported as all upstream tests passing. Simulator
trace export/replay is a future task, not covered by the three wire vectors.

`FipsProtoSweep` expects the integrated 28/0 line but is excluded locally while
`cases.c` remains at 25/3. All other Fips tests remain selected.
