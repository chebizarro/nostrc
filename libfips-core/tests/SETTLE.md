# Settlement test mapping: D2 partial port

Source: `core/src/proto/settle/` at
`64f5b4d482cfe14abad108ffc2d92128f7658311` (2026-11-20).

## Scope

Implemented: params, link records/paths/derived index/attribution/close rule,
handshake legs/answers/resends/deadlines/lease ownership, restart gates,
input/output vocabulary, production table arbitration and the independent
model history/judge, the production join adapter and test-only parity
vocabulary/schedules/trace machinery. Fault integration and dependent world
infrastructure are **not implemented yet**.
No fipsd integration, hosted CI, live networking or full settlement convergence
is claimed. Remaining core work is `nostrc-f12v2.8`; leave `nostrc-f12v2.7` open.
Pinned upstream specs are in `src/proto/settle/upstream/`, verbatim and expressly
not a claim that every upstream rule is implemented in this subset.

The complete inventory below contains **369 actual Rust test functions in 36
files**, including feature-gated slow tests: **167 ported, 17 covered-by, 185 not ported**. Covered-by is a many-to-many behavioral mapping,
not a claim that every original assertion/helper was translated. Attribute-line
numbers identify the pinned sources. An earlier raw substring count of 370
included the string literal `"#[test]"` in `tests/mod.rs`'s source scanner;
only real standalone attributes are counted here.

## C coverage

`FipsCoreSettleTest` includes 18 same-named direct upstream gate tests in
`test_settle_gates.c`, plus transactional arena/parameter rejection checks.
`test_settle_model.c` checks the independent defaults against gate intervals,
model arena limits, transactional failure, clock overflow and batch runs. `test_settle.c` also adds capacity preflight/output sentinels,
full arenas, ID exhaustion, oversized transcripts, replacement/take lease moves,
failed sends, zero resend cap, short host clock and actual slot reuse followed
by ascending close/lease output. Four pthreads run **independent** caller-owned
states; shared-object mutation requires caller synchronization.

`FipsDiffSettle` (`diff/settle.c`, `vectors/settle.json`) checks 161 traces and
2580 trace rows (including construction/cleanup): 192 deterministic mixed link operations with every record, index,
placement and close snapshot, plus 11 leg traces varying multiplier, resend
interval, origin cap, answer and failed-send retry. A gate trace checks all six sources, both floors, stamps, replay/silent/redial
memory, partial forgetting, refusal counts, identity lapse and split clocks.
Ephemerals are opaque IDs mapped injectively on each side of the gate trace;
this tests gate semantics, not hashing. The new exporter module
`tools/fips-vectors/src/settle.rs` executes unmodified upstream `fips-core` at
the exact pin, not a C-derived oracle. It rejects mutation environment switches.
The replay rejects wrong pin/family/schema, trace count and mismatched outputs.
The 78 table traces run every original `tests/table.rs` assertion body in
`settle/table_tests.rs` against pinned upstream and record every production
input, ordered output and public state view for C replay. Adaptations are
imports/test registration, observing (not changing) digest construction, one
private gate-writer fixture, and removing candidate-only output arms absent
from the dependency's `test-hooks` build. Original behavioral assertions are
preserved. Cleanup closes/drains each table explicitly and is also replayed.
The additional 55 model traces preserve all original `model/tests/` assertion
bodies; the C replay checks judgments and complete public model state after
every event. Thirteen gate/model histories also run production C gates alongside
the independent C judge and compare their refusals. The model shares no production
table/gate/rekey algorithm or state. Its copyable arenas and histories are bounded
to 128 entries; capacity/clock failures leave state and outputs unchanged. As in
upstream, model APIs require `FIPS_TEST_HOOKS`: test executables compile the model
directly, and it is not included in the default library archive. This is not yet
full table-versus-model world or join equivalence.
Native `test_settle_table.c` additionally checks post-partial-write arena
rollback, input lease preservation, connecting facts overriding dial facts,
unconsumed-output ownership barriers and idempotent close.
Join adds 76 pinned upstream routing rows covering rekey routing, path/core
interleaving with held/released link combinations, and active/standby roam/bind.
Native tests also exercise real IK start/stop, refused foreign-key shifts,
successful carry/renewal, lease returns and both option-slot flags. Rekey/path
sources are compiled directly in both test executables for TSan qualification.
D1's completed pin branch was merged normally as `5420a09a0`; full-SHA Cargo
entries are retained. The workflow target and corpus names match the actual
settlement targets; no CI line correction was needed.

Parity adds 791 parser/round-trip/core-override rows, all 46 immutable shared
schedules, the ten unshared-start explanations and derived rekey/path settings.
All four original parity assertion bodies run in the Rust exporter, with their
C adaptations in `test_settle_parity.c`. Diagnostic strings become typed bit
masks. C-local text is NUL-free ASCII, names at most95 bytes, and each trace is
bounded to8 settings/128 lines/64 overrides; parse/format/capacity failure is
transactional. The data-only include is generated from the pinned Rust oracle
by `tests/tools/generate_settle_parity.py` (`--check` detects drift). Parity's
reader is not itself evidence that either side executed those schedules.

`FIPS_ENABLE_FUZZERS=ON` adds `fips_fuzz_settle`. The core is instrumented with
`-fsanitize=fuzzer-no-link`. Bounded link/leg operations and all 28 table inputs plus poll/close check
lease conservation after every step, failure rollback and owner draining.
Table fuzz identities are bounded to the caller arena's four identity slots. This is not a
replacement for upstream worlds, scheduler exploration or mutation testing.

## Not-ported reasons

- **WORLD**: deterministic transports/nodes, starts, simulation, regression,
  shadow and comparison infrastructure have not yet been ported.
- **JOINT**: bounded explorers compose settlement, rekey/FMP/FSP, multipath and
  ordered effects. Existing C rekey/path modules alone cannot qualify this.
- **LEGACY**: current-node/shell simulation and explorers need node parity;
  node wiring is outside this task. No legacy/live compatibility claim.
- **MUTATION**: fault switches/candidate sweeps and parity hooks still need the
  complete joined machine, so are deferred rather than ported as ineffective toggles.
- **STRUCTURE**: Rust lint/module/source-scanning fences are language-specific;
  no equivalent C structural scan is claimed by this partial module.

Infrastructure helpers without test attributes are not counted as tests.
The production `reap_leg` shell adapter is implemented; test-only
replant/naming/drive helpers are not exposed. See the module README for API bounds.

## Qualification commands

2026-11-21: vocabulary/gates, table, independent model, join and parity layers
pass **2/2, zero skipped** on macOS arm64 (Homebrew LLVM), separately under
ASan+UBSan and TSan, with warnings-as-errors. The previous layers (through the
independent model) also qualified on Ubuntu24.04 arm64 (`clang-18`) with2/2 in
each sanitizer configuration and20,000 clean fuzz runs. The final Linux run for
join/parity is pending until the remaining layer is finished.
The TSan test executables compile all production settlement sources directly because
global CMake deliberately leaves the core archive uninstrumented under TSan.
No suppression or sanitizer exclusion was added.

```sh
# macOS:
SRC=/Users/bizarro/Documents/Projects/nostrc-wt/w26-settle
CC=/opt/homebrew/opt/llvm/bin/clang
# Linux VM uses SRC=/home/bizarro/w26-settle/src and CC=clang-18.
# Choose separate BUILD paths: /tmp/w26-settle-{asan,tsan} on macOS,
# /home/bizarro/w26-settle/{asan,tsan} on Linux.
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_BUILD_TYPE=Debug -DGO_WARNINGS_AS_ERRORS=ON \
  -DBUILD_FIPS=ON -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF \
  -DBUILD_LIBHANAMI=OFF -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF \
  -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF -DSIGNET_ENABLE=OFF \
  -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF \
  -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON -DFIPS_ENABLE_FUZZERS=ON
# macOS additionally uses -DNOSTR_USE_SYSTEM_NSYNC=OFF.
# For TSan replace all three ASan/UBSan/fuzzer flags with -DGO_ENABLE_TSAN=ON.
cmake --build "$BUILD" --target fips_core_settle_test fips_diff_settle -j4
ctest --test-dir "$BUILD" -R '^(FipsCoreSettleTest|FipsDiffSettle)$' --output-on-failure
# Linux: prefix EVERY build with flock /tmp/w26-build.lock (VM has 3 GB RAM).

# Fuzz smoke: ASan/UBSan build only, seeds copied into a scratch CORPUS.
cmake --build "$BUILD" --target fips_fuzz_settle -j4
"$BUILD/libfips-core/fuzz/fips_fuzz_settle" "$CORPUS" -runs=20000 -max_len=1024
```

Vectors were generated using a scratch Rust driver importing the unmodified new
module with pinned `fips-core`, `serde_json` and `hex`; regeneration is byte-equal.
`cargo check --manifest-path "$SRC/tools/fips-vectors/Cargo.toml" --target-dir
/tmp/w26-settle-export/check` also passes for the registered full exporter.
The registered full exporter was also run with `export --out` into scratch;
its settlement output is byte-equal. Negative checks reject wrong pin, family,
schema, trace count, a corrupted model judgment and a corrupted model state;
all four forbidden mutation environment variables are rejected too.
Only settlement vectors are delivered here; D1 owns regeneration of other
families and shared pin metadata. Scratch builds/drivers are removed at handoff.

### CI handoff (D1 owns the workflow)

Verified after merging D1: both sanitizer build targets are present, including
TSan; its explicit `FipsCoreSettleTest` run compensates for the broader core
exclusion, while `FipsDiffSettle` is not excluded. `fips_fuzz_settle` appears in
both fuzz build/run lists, using the actual `libfips-core/fuzz/corpus/fips_fuzz_settle`
directory. No workflow correction was needed. Local VM/host evidence is not
hosted CI acceptance.

## Complete pinned upstream inventory

### `model/tests/classify.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `rule_1_a_msg1_from_an_identity_not_held_installs_the_peer` (L9) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_2_a_changed_epoch_is_a_restart` (L26) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_3_1_a_resend_of_the_setup_msg1_draws_the_same_msg2` (L44) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_3_2_a_copy_with_a_rewritten_header_is_discarded` (L61) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_3_3_any_other_msg1_on_a_young_link_replaces_the_peer` (L77) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_3_3_a_refused_replacement_is_recorded_so_its_resends_are_copies` (L96) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_4_1_an_eligible_copy_off_a_reachable_link_is_discarded` (L122) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_4_1_an_eligible_msg1_off_an_unreachable_link_goes_on_to_the_next_rule` (L140) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_4_2_while_a_pending_session_is_held_only_its_arming_msg1_is_answered` (L158) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_4_3_an_eligible_copy_the_record_holds_is_discarded` (L175) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_4_4_a_rekey_msg1_during_this_nodes_own_rekey_is_discarded_by_the_smaller_node` (L191) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `rule_4_5_an_eligible_msg1_on_the_link_is_the_peers_rekey` (L211) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
### `model/tests/close.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `the_losing_handshakes_connection_is_closed_and_no_disconnect_is_sent` (L26) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_connectionless_losing_link_closes_nothing` (L48) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_connection_a_remaining_link_still_needs_is_kept_open` (L57) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `after_a_crossing_each_end_needs_only_its_kept_links_setup_address` (L79) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_reaped_attempts_connection_is_closed_unless_a_live_link_needs_it` (L88) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
### `model/tests/crossing.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `the_tie_break_moves_the_smaller_node_onto_its_own_dial` (L35) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `the_tie_break_keeps_the_larger_nodes_inbound_handshake` (L50) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `both_ends_of_a_crossing_settle_on_the_handshake_the_smaller_node_dialed` (L58) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_held_link_this_node_dialed_moves_onto_its_later_dial_whatever_the_ordering` (L72) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_msg2_with_a_changed_epoch_is_not_a_cross_connection` (L91) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_msg1_that_arrives_on_the_link_after_the_peer_is_held_is_a_replacement_the_heard_floor_refuses` (L107) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_msg1_that_arrives_off_the_link_after_the_peer_is_held_is_a_redial_its_first_frame_settles` (L125) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `after_a_late_second_dial_the_peers_first_frame_under_the_old_session_moves_back` (L149) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `after_a_late_second_dial_the_peers_first_frame_under_the_new_session_drops_the_old` (L173) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_stored_msg2_sent_after_the_peer_is_held_elsewhere_installs_nothing` (L194) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_stored_msg2_sent_with_no_peer_held_installs_the_peer` (L222) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_dial_on_the_peers_aged_link_completing_while_held_is_left_open` (L245) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
### `model/tests/gates.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `the_unheard_floor_refuses_a_replacement_until_the_peering_is_one_second_old` (L23) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `the_heard_floor_refuses_a_replacement_within_fifteen_seconds_of_the_last_frame` (L36) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `the_heard_floor_counts_from_installation_when_no_frame_has_arrived` (L48) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `heard_from_starts_at_the_first_frame_for_a_peering_installed_from_a_msg1` (L64) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `the_dampener_refuses_until_fifteen_seconds_after_the_last_accepted_restart` (L79) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `between_the_floors_and_the_ceiling_the_model_refuses_only_while_dampened` (L115) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_replacement_at_a_msg1_stamps_the_dampener_only_if_the_old_peering_was_heard` (L125) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_restart_found_at_this_nodes_own_msg2_does_not_stamp_the_dampener` (L138) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_restart_revealed_in_a_rekey_msg2_keeps_the_link_and_does_not_stamp_the_dampener` (L159) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
### `model/tests/redial.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_fresh_msg1_off_an_eligible_reachable_link_is_answered_and_leaves_the_peering_until_its_frame` (L18) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_fresh_msg1_off_a_young_link_is_answered_as_a_redial_and_not_held_by_the_heard_floor` (L35) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_frame_under_the_held_session_leaves_the_redial_pending_and_its_timeout_drops_it` (L45) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_fresh_msg1_off_the_link_of_an_unheard_peering_is_a_replacement_at_the_msg1_and_not_a_redial` (L58) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_second_redial_within_one_second_is_refused_and_a_later_one_replaces_it` (L75) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `the_swap_of_a_heard_peering_stamps_the_dampener_and_an_answered_redial_does_not` (L88) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `an_armed_redials_copy_is_a_copy_and_its_byte_identical_resend_draws_the_stored_msg2` (L105) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_redial_whose_peering_is_lost_first_installs_the_peer_at_its_frame` (L119) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
### `model/tests/roam.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_switch_of_the_active_path_moves_the_peer_there_and_is_no_transition` (L9) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_switch_with_no_peer_held_changes_nothing` (L28) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_frame_from_a_new_address_on_the_one_paths_transport_roams_the_peer` (L36) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_frame_from_another_transport_moves_nothing_at_one_path` (L46) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_frame_from_a_new_address_moves_nothing_while_the_link_holds_several_paths` (L75) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_path_message_from_a_new_address_moves_nothing_on_a_link_that_runs_multipath` (L95) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_follow_of_the_path_the_peer_is_at_moves_the_peer_and_is_no_transition` (L109) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_follow_of_another_path_leaves_the_peer` (L128) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
| `a_frame_at_a_node_with_no_bundle_roams_the_peer_from_any_transport` (L145) | ported | Original model assertion body in `settle/model_tests/`; independent C judge replay, full state and judgment. |
### `tests/candidates.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `the_dialer_settle_candidate_explored_shows_exactly_the_breaches_recorded_for_it` (L504) | not ported | MUTATION (see boundary above) |
| `the_responder_hold_candidate_explored_shows_exactly_the_breaches_recorded_for_it` (L510) | not ported | MUTATION (see boundary above) |
| `the_aged_move_candidate_explored_shows_exactly_the_breaches_recorded_for_it` (L516) | not ported | MUTATION (see boundary above) |
| `both_late_dial_candidates_settle_a_late_second_dial_answered_in_the_other_order` (L535) | not ported | MUTATION (see boundary above) |
| `the_aged_move_candidate_answers_the_rekey_after_a_dial_on_the_aged_link_once_its_frame_arrives_first` (L552) | not ported | MUTATION (see boundary above) |
### `tests/cmp.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `the_core_agrees_with_the_model_on_every_model_table_history` (L639) | not ported | WORLD (see boundary above) |
| `a_held_peers_reachability_reads_the_bundles_active_path_not_the_cores_record` (L670) | not ported | WORLD (see boundary above) |
| `every_named_difference_from_the_model_still_differs` (L771) | not ported | WORLD (see boundary above) |
### `tests/explore.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `every_named_start_reaches_its_target_depth_over_the_model` (L513) | not ported | WORLD (see boundary above) |
| `a_replayed_path_rebuilds_the_same_world` (L519) | not ported | WORLD (see boundary above) |
### `tests/faults.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `every_guard_switch_turns_each_test_it_names_red_and_each_passes_with_none` (L2016) | not ported | MUTATION (see boundary above) |
### `tests/gates.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `the_unheard_floor_refuses_until_one_second_and_accepts_at_it` (L78) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_heard_floor_counts_from_the_last_frame` (L91) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_heard_floor_counts_from_installation_when_no_frame_has_arrived` (L101) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_dampener_refuses_until_its_ceiling_after_the_last_accepted_restart` (L111) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `an_own_msg2_restart_goes_through_the_gate_and_is_accepted_inside_its_floors` (L136) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `an_own_msg2_restart_does_not_stamp_the_dampener` (L159) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_replay_memory_refuses_an_ephemeral_that_already_promoted_an_unheard_peering` (L173) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_replay_memory_holds_at_most_eight` (L201) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `a_frame_forgets_the_replay_memory_and_the_silent_sessions` (L226) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `silent_sessions_start_a_refusal_at_the_third_in_a_row` (L252) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `each_further_silent_session_doubles_the_refusal_up_to_its_cap` (L298) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `gate_memory_holds_no_entry_once_an_identitys_state_has_lapsed` (L330) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_refused_count_goes_with_its_silent_session_record` (L388) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_default_intervals_are_the_frcs` (L433) | ported | `test_settle_model.c`: independent model defaults compared to gate intervals. |
| `the_gates_agree_with_the_model_on_every_gate_history` (L625) | ported | All 13 original histories, paired production C gates and independent C judge in differential replay. |
| `a_redial_is_not_held_by_the_heard_floor` (L732) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_redial_memory_refuses_a_seen_ephemeral_and_a_full_memory_until_it_ages_out` (L749) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_swap_stamps_the_dampener_only_for_a_heard_peering_and_clears_the_redial_memory` (L774) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_redial_memory_outlives_a_frame_and_the_lapse_of_the_rest_of_the_identitys_memory` (L810) | ported | `test_settle_gates.c`: same-named adapted upstream test |
| `the_redial_memory_outlives_the_first_frame_of_a_heard_session_and_a_move` (L831) | ported | `test_settle_gates.c`: same-named adapted upstream test |
### `tests/join.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `each_rekey_output_routes_to_the_core_or_the_shell` (L210) | ported | `test_settle_join.c`: adapted upstream routing/lifecycle assertions; borrowed owners and real rekey/path machines. |
| `each_bundle_output_mirrors_to_its_input` (L379) | ported | `test_settle_join.c`: adapted upstream routing/lifecycle assertions; borrowed owners and real rekey/path machines. |
| `a_bundle_output_at_a_removal_reaches_no_link` (L396) | ported | `test_settle_join.c`: adapted upstream routing/lifecycle assertions; borrowed owners and real rekey/path machines. |
| `a_released_path_goes_to_the_link_moved_off` (L408) | ported | `test_settle_join.c`: adapted upstream routing/lifecycle assertions; borrowed owners and real rekey/path machines. |
| `a_roam_is_moved_and_a_bind_is_a_live_path` (L458) | ported | `test_settle_join.c`: adapted upstream routing/lifecycle assertions; borrowed owners and real rekey/path machines. |
| `a_standby_paths_roam_is_moved_and_not_marked_a_roam` (L501) | ported | `test_settle_join.c`: adapted upstream routing/lifecycle assertions; borrowed owners and real rekey/path machines. |
| `a_steps_outputs_come_back_in_order` (L528) | ported | `test_settle_join.c`: adapted upstream routing/lifecycle assertions; borrowed owners and real rekey/path machines. |
| `a_rekey_step_through_join_allocates_nothing` (L604) | covered-by | The C iterator borrows the original output owner (pointer-identity checks), has no allocator calls and builds no intermediate output collection; Rust's thread allocation counter is not translated. |
| `a_start_mirrors_the_original_path_as_live` (L822) | ported | `test_settle_join.c`: adapted upstream routing/lifecycle assertions; borrowed owners and real rekey/path machines. |
| `a_move_refused_for_another_key_leaves_both_machines_as_they_were` (L873) | ported | `test_settle_join.c`: adapted upstream routing/lifecycle assertions; borrowed owners and real rekey/path machines. |
| `a_stop_files_what_the_rekey_machine_carried` (L939) | ported | `test_settle_join.c`: adapted upstream routing/lifecycle assertions; borrowed owners and real rekey/path machines. |
### `tests/joint/explore.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `every_named_start_reaches_its_target_depth_with_the_machines_off` (L65) | not ported | JOINT (see boundary above) |
| `every_named_start_reaches_its_target_depth_with_the_machines_on` (L73) | not ported | JOINT (see boundary above) |
| `every_named_start_reaches_its_target_depth_with_the_machines_on_against_off` (L81) | not ported | JOINT (see boundary above) |
| `every_named_start_reaches_its_target_depth_with_the_machines_off_against_on` (L89) | not ported | JOINT (see boundary above) |
| `the_cold_start_reaches_its_stated_depth_off_with_off` (L97) | not ported | JOINT (see boundary above) |
| `the_cold_start_reaches_its_stated_depth_on_with_on` (L104) | not ported | JOINT (see boundary above) |
| `the_cold_start_is_explored_as_deep_as_the_reach_budget_allows_at_off_and_at_on` (L125) | not ported | JOINT (see boundary above) |
| `two_worlds_on_one_path_have_one_digest` (L156) | not ported | JOINT (see boundary above) |
| `a_state_differing_only_inside_a_machine_has_its_own_key` (L235) | not ported | JOINT (see boundary above) |
| `each_breach_the_pins_hold_is_exempted_by_its_own_cause_and_a_settled_pair_by_none` (L410) | not ported | JOINT (see boundary above) |
| `every_explored_interleaving_of_the_joint_party_keeps_the_properties_off` (L602) | not ported | JOINT (see boundary above) |
| `every_explored_interleaving_of_the_joint_party_keeps_the_properties_on` (L612) | not ported | JOINT (see boundary above) |
| `the_sweep_of_start_2_keeps_the_properties_on_with_on` (L622) | not ported | JOINT (see boundary above) |
| `the_sweep_of_start_3_keeps_the_properties_on_with_on` (L629) | not ported | JOINT (see boundary above) |
| `the_sweep_of_start_9_keeps_the_properties_on_with_on` (L636) | not ported | JOINT (see boundary above) |
| `the_sweeps_of_the_two_path_starts_keep_the_properties_on_with_on` (L643) | not ported | JOINT (see boundary above) |
| `every_explored_interleaving_of_the_joint_party_keeps_the_properties_on_against_off` (L653) | not ported | JOINT (see boundary above) |
| `every_explored_interleaving_of_the_joint_party_keeps_the_properties_off_against_on` (L663) | not ported | JOINT (see boundary above) |
| `every_explored_interleaving_of_the_core_keeps_the_properties_and_heals` (L692) | not ported | JOINT (see boundary above) |
### `tests/joint/feeds.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `an_on_pair_with_a_candidate_becomes_capable_and_holds_a_second_path` (L214) | not ported | JOINT (see boundary above) |
| `an_on_end_against_an_off_end_stays_not_capable_within_the_probe_budget` (L233) | not ported | JOINT (see boundary above) |
| `a_capable_live_peer_is_not_dialled_and_a_stale_or_not_capable_one_is` (L254) | not ported | JOINT (see boundary above) |
| `a_rekey_answer_goes_on_the_arrival_path_at_on_and_the_established_link_at_off` (L312) | not ported | JOINT (see boundary above) |
| `a_closed_connection_on_a_link_of_two_paths_fails_that_path_and_keeps_the_link` (L377) | not ported | JOINT (see boundary above) |
| `a_path_message_roams_a_one_path_link_at_off_and_not_at_on` (L417) | not ported | JOINT (see boundary above) |
### `tests/joint/machines.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `two_peers_machines_rekey_and_both_cut_over_on_one_session` (L1246) | not ported | JOINT (see boundary above) |
| `a_move_keeps_the_record_the_link_age_and_renews_the_bundle` (L1314) | not ported | JOINT (see boundary above) |
| `an_abstract_far_ends_frame_opens_at_the_machine_holding_its_session` (L1378) | not ported | JOINT (see boundary above) |
| `a_disabled_bundle_takes_a_candidate_as_a_poll_and_an_enabled_one_holds_it` (L1410) | not ported | JOINT (see boundary above) |
### `tests/joint/names.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `the_stub_keys_keep_the_worlds_address_order_in_both_flips` (L240) | not ported | JOINT (see boundary above) |
| `an_abstract_msg1_reads_as_the_ik_msg1_of_its_handshake` (L264) | not ported | JOINT (see boundary above) |
| `an_abstract_msg2_reads_as_the_ik_msg2_of_its_handshake` (L307) | not ported | JOINT (see boundary above) |
| `an_abstract_frame_opens_only_under_its_handshake` (L344) | not ported | JOINT (see boundary above) |
| `the_stub_profile_selects_the_rows_the_link_profile_does` (L379) | not ported | JOINT (see boundary above) |
### `tests/joint/oracle.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `one_view_reads_a_restart_on_both_sides_of_the_core` (L644) | not ported | JOINT (see boundary above) |
| `one_view_reads_the_session_a_move_built_from` (L691) | not ported | JOINT (see boundary above) |
| `one_view_fires_on_a_planted_identity` (L713) | not ported | JOINT (see boundary above) |
| `leases_fire_on_a_free_the_shell_lost` (L787) | not ported | JOINT (see boundary above) |
| `no_orphan_fires_on_a_machine_past_its_peer` (L808) | not ported | JOINT (see boundary above) |
| `mirrored_fires_on_a_path_the_core_did_not_take` (L831) | not ported | JOINT (see boundary above) |
| `delivered_matches_each_output_to_its_input` (L868) | not ported | JOINT (see boundary above) |
| `attributed_reads_a_roamed_link_as_the_node_does` (L920) | not ported | JOINT (see boundary above) |
| `each_machine_predicate_fires_on_a_planted_state` (L969) | not ported | JOINT (see boundary above) |
| `each_path_check_fires_on_a_planted_output` (L1155) | not ported | JOINT (see boundary above) |
### `tests/joint/parity.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `every_shared_schedule_gives_its_trace_on_the_model_of_the_node` (L318) | not ported | JOINT (see boundary above) |
| `every_shared_schedule_gives_its_trace_on_joint_nodes` (L337) | not ported | JOINT (see boundary above) |
### `tests/joint/party.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_crossing_on_a_connection_oriented_transport_leaves_the_smaller_node_at_its_dials_pair_on_real_machines` (L3067) | not ported | JOINT (see boundary above) |
| `a_crossing_settles_on_one_session_on_real_machines` (L3084) | not ported | JOINT (see boundary above) |
| `a_rekey_cuts_over_at_both_ends_and_the_old_session_drains` (L3100) | not ported | JOINT (see boundary above) |
| `after_a_frame_from_another_transport_sends_stay_on_the_active_path` (L3133) | not ported | JOINT (see boundary above) |
| `a_restart_revealed_in_a_rekey_msg2_reaches_the_core` (L3185) | not ported | JOINT (see boundary above) |
| `a_fresh_msg1_on_a_young_link_reaches_the_gates` (L3230) | not ported | JOINT (see boundary above) |
| `a_one_path_roam_reaches_the_link_record` (L3258) | not ported | JOINT (see boundary above) |
| `a_peer_silent_for_the_link_dead_timeout_is_removed_then_and_redialled` (L3303) | not ported | JOINT (see boundary above) |
| `a_peer_silent_past_the_link_dead_timeout_on_a_link_of_several_paths_is_removed_and_the_pair_heals` (L3363) | not ported | JOINT (see boundary above) |
| `every_start_settles_scripted_off_with_off` (L3436) | not ported | JOINT (see boundary above) |
| `an_on_link_replacement_in_a_rekeys_drain_is_held_by_the_heard_floor_off_with_off_and_on_with_on` (L3472) | not ported | JOINT (see boundary above) |
| `starts_6_and_7_show_their_two_defects_on_real_machines` (L3518) | not ported | JOINT (see boundary above) |
### `tests/joint/pins.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_crossing_moved_onto_the_pair_of_the_peers_dial_keeps_that_connection_open` (L23) | not ported | JOINT (see boundary above) |
| `a_proven_path_whose_connection_failed_is_not_counted_in_use_without_a_connection` (L77) | not ported | JOINT (see boundary above) |
| `a_frame_from_another_transport_leaves_the_cores_record_of_the_peer_at_the_active_path` (L189) | not ported | JOINT (see boundary above) |
| `a_switch_moves_the_cores_record_of_the_peer_to_the_new_active_path` (L209) | not ported | JOINT (see boundary above) |
| `a_msg1_from_a_capable_peers_setup_pair_with_no_path_is_off_the_link` (L240) | not ported | JOINT (see boundary above) |
| `a_switch_to_a_path_that_is_not_live_leaves_it_acknowledged_in_the_core` (L263) | not ported | JOINT (see boundary above) |
| `a_msg1_from_the_pair_a_frame_from_another_transport_came_from_is_off_the_link` (L296) | not ported | JOINT (see boundary above) |
| `a_msg1_from_the_pair_a_frame_moved_the_cores_record_to_whose_path_is_probing_is_off_the_link` (L321) | not ported | JOINT (see boundary above) |
### `tests/joint/runs.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `every_start_settles_scripted_on_with_on` (L105) | not ported | JOINT (see boundary above) |
| `the_reach_of_start_14_stays_within_its_cap_off_with_off` (L122) | not ported | JOINT (see boundary above) |
| `the_reach_of_start_14_stays_within_its_cap_on_with_on` (L129) | not ported | JOINT (see boundary above) |
| `the_reach_of_start_14_stays_within_its_cap_on_with_off` (L136) | not ported | JOINT (see boundary above) |
| `the_reach_of_start_14_stays_within_its_cap_off_with_on` (L143) | not ported | JOINT (see boundary above) |
| `the_sweep_of_start_14_keeps_the_properties_off_with_off` (L150) | not ported | JOINT (see boundary above) |
| `the_sweep_of_the_drain_start_keeps_the_properties_off_with_off` (L157) | not ported | JOINT (see boundary above) |
| `the_sweep_of_start_10_keeps_the_properties_off_with_off` (L164) | not ported | JOINT (see boundary above) |
| `the_sweep_of_start_12_keeps_the_properties_on_with_on` (L171) | not ported | JOINT (see boundary above) |
| `simultaneous_rekeys_settle_on_one_session` (L180) | not ported | JOINT (see boundary above) |
| `a_rekey_msg1_answered_as_a_redial_off_the_link_is_refused_as_a_copy_when_it_comes_on_the_link` (L227) | not ported | JOINT (see boundary above) |
| `a_copy_of_an_answered_rekey_msg1_after_a_move_is_refused_as_answered_before` (L316) | not ported | JOINT (see boundary above) |
### `tests/joint/starts.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_crossing_while_capability_probes_are_out_renews_the_bundle_and_keeps_both_ends_mirrored` (L775) | not ported | JOINT (see boundary above) |
| `a_carrier_loss_and_a_mandatory_switch_keep_both_ends_mirrored` (L815) | not ported | JOINT (see boundary above) |
| `the_only_paths_transport_removed_orphans_the_peer_and_a_redial_settles` (L837) | not ported | JOINT (see boundary above) |
| `a_nat_rebound_of_the_second_path_moves_it_in_the_link_record` (L866) | not ported | JOINT (see boundary above) |
| `a_nat_rebound_of_the_active_path_moves_the_peer_with_it` (L887) | not ported | JOINT (see boundary above) |
| `a_path_removed_at_the_peer_leaves_the_link_record` (L922) | not ported | JOINT (see boundary above) |
| `a_link_retired_beside_a_dial_in_flight_keeps_the_connection_the_dial_uses` (L940) | not ported | JOINT (see boundary above) |
### `tests/legacy/explore.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_joint_node_off_against_a_model_of_maint_converges_on_every_start_in_both_address_orders` (L379) | not ported | LEGACY (see boundary above) |
| `a_joint_node_on_against_a_model_of_maint_converges_on_every_start_in_both_address_orders` (L385) | not ported | LEGACY (see boundary above) |
| `a_joint_node_off_against_a_model_of_master_off_converges_on_every_start_in_both_address_orders` (L391) | not ported | LEGACY (see boundary above) |
| `a_joint_node_off_against_a_model_of_master_on_converges_on_every_start_in_both_address_orders` (L400) | not ported | LEGACY (see boundary above) |
| `a_joint_node_on_against_a_model_of_master_off_converges_on_every_start_in_both_address_orders` (L408) | not ported | LEGACY (see boundary above) |
| `a_joint_node_on_against_a_model_of_master_on_converges_on_the_quicker_starts` (L416) | not ported | LEGACY (see boundary above) |
| `a_joint_node_on_against_a_model_of_master_on_converges_on_a_crossing_of_two_transports` (L422) | not ported | LEGACY (see boundary above) |
| `a_joint_node_on_against_a_model_of_master_on_converges_on_a_connection_oriented_crossing` (L430) | not ported | LEGACY (see boundary above) |
| `a_joint_node_on_against_a_model_of_master_on_converges_on_a_crossing_during_a_rekey` (L441) | not ported | LEGACY (see boundary above) |
| `a_joint_node_on_against_a_model_of_master_on_converges_on_the_starts_on_real_bundles` (L449) | not ported | LEGACY (see boundary above) |
| `a_model_of_the_node_off_against_a_model_of_maint_converges_on_every_start_in_both_address_orders` (L457) | not ported | LEGACY (see boundary above) |
| `a_model_of_the_node_on_against_a_model_of_maint_converges_on_every_start_in_both_address_orders` (L464) | not ported | LEGACY (see boundary above) |
### `tests/legacy/findings.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `against_a_model_of_the_node_crossed_dials_on_one_connectionless_transport_leave_the_smaller_node_on_the_link_it_dialed_smaller_first` (L133) | not ported | LEGACY (see boundary above) |
| `against_a_model_of_the_node_crossed_dials_on_one_connectionless_transport_leave_the_smaller_node_on_the_link_it_dialed_larger_first` (L139) | not ported | LEGACY (see boundary above) |
| `against_a_model_of_the_node_crossed_dials_on_a_connection_oriented_transport_leave_the_smaller_node_on_the_link_it_dialed_smaller_first` (L145) | not ported | LEGACY (see boundary above) |
| `against_a_model_of_the_node_crossed_dials_on_a_connection_oriented_transport_leave_the_smaller_node_on_the_link_it_dialed_larger_first` (L151) | not ported | LEGACY (see boundary above) |
| `against_a_model_of_master_a_late_second_dial_answered_in_the_other_order_splits_the_session_larger_dialer` (L169) | not ported | LEGACY (see boundary above) |
| `against_a_model_of_master_a_late_second_dial_answered_in_the_other_order_splits_the_session_smaller_dialer` (L175) | not ported | LEGACY (see boundary above) |
| `against_a_model_of_master_a_dial_on_the_peers_aged_link_leaves_a_pending_the_peer_cannot_adopt` (L181) | not ported | LEGACY (see boundary above) |
| `against_a_model_of_master_a_forward_wall_step_does_not_let_a_msg1_replace_a_peering_heard_just_now` (L196) | not ported | LEGACY (see boundary above) |
| `the_model_of_master_against_itself_on_every_start_shows_the_two_defects_and_no_wrong_link` (L312) | not ported | LEGACY (see boundary above) |
| `the_model_of_master_with_multipath_on_against_itself_on_a_crossing_of_two_transports` (L332) | not ported | LEGACY (see boundary above) |
| `the_model_of_master_with_multipath_on_holds_the_dialed_link_on_a_connection_oriented_crossing` (L338) | not ported | LEGACY (see boundary above) |
| `the_model_of_master_with_multipath_on_against_itself_on_a_crossing_during_a_rekey` (L344) | not ported | LEGACY (see boundary above) |
### `tests/legacy/maint.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_path_message_reaches_the_model_as_a_frame_and_draws_no_answer` (L1386) | not ported | LEGACY (see boundary above) |
| `a_joint_node_holds_the_model_on_the_models_frames_alone` (L1409) | not ported | LEGACY (see boundary above) |
| `a_copy_of_the_models_msg2_to_a_dial_is_no_reply_to_an_own_rekey` (L1436) | not ported | LEGACY (see boundary above) |
### `tests/legs.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_dial_leg_is_reaped_at_its_deadline_and_returns_its_lease` (L72) | covered-by | `legs_test`; differential leg traces |
| `resends_follow_the_backoff_and_repeat_the_msg1_byte_for_byte` (L131) | covered-by | `legs_test`; differential leg traces |
| `a_configured_multiplier_spaces_the_resends_as_the_node_truncates_them` (L154) | covered-by | `timing_test`; four upstream differential multipliers |
| `a_dials_first_resend_comes_a_second_after_its_msg1_whatever_the_configured_interval` (L182) | covered-by | `legs_test`; differential leg traces |
| `an_mdns_directed_dial_stops_at_its_resend_cap` (L209) | covered-by | `legs_test`; differential leg traces |
| `a_responder_leg_answers_a_resend_with_its_stored_msg2_and_is_reclaimed` (L242) | covered-by | `legs_test`; differential leg traces |
| `a_poll_before_the_next_wake_does_nothing` (L273) | covered-by | `legs_test`; differential leg traces |
| `a_deadline_past_the_end_of_the_clock_saturates` (L319) | covered-by | `short_clock_test`, `timing_test` (host overflow/halving) |
| `close_returns_every_lease_and_retires_every_link` (L343) | covered-by | `legs_test`; differential leg traces |
### `tests/links.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `the_index_holds_each_live_links_setup_address_and_proven_paths` (L18) | covered-by | `links_test`; differential link snapshots |
| `retiring_a_link_closes_only_addresses_no_other_live_link_uses` (L42) | covered-by | `links_test`; differential link snapshots |
| `a_retired_links_address_that_is_another_links_live_path_stays_open` (L58) | covered-by | `links_test`; differential link snapshots |
| `a_probing_path_keeps_no_connection_open` (L72) | covered-by | `links_test`; differential link snapshots |
| `a_released_path_marked_keep_closes_nothing` (L85) | covered-by | `links_test`; differential link snapshots |
| `a_released_setup_address_leaves_the_index_and_a_roamed_one_stays` (L107) | covered-by | `links_test`; differential link snapshots |
| `each_attribution_question_answers_as_the_node_does` (L163) | covered-by | `links_test`; differential link snapshots |
### `tests/mod.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `the_wildcard_arm_fence_is_declared_at_the_settlement_root` (L125) | not ported | STRUCTURE (see boundary above) |
| `no_core_source_defines_a_setter_and_the_scan_finds_a_planted_one` (L145) | not ported | STRUCTURE (see boundary above) |
| `no_core_source_reaches_into_the_model_and_the_scan_finds_a_planted_path` (L177) | not ported | STRUCTURE (see boundary above) |
| `no_model_source_reaches_into_the_core_and_the_scan_finds_a_planted_path` (L254) | not ported | STRUCTURE (see boundary above) |
| `every_core_and_model_source_file_is_scanned` (L276) | not ported | STRUCTURE (see boundary above) |
| `every_settlement_test_is_named_by_a_switch_or_listed_and_the_scan_finds_a_planted_one` (L311) | not ported | STRUCTURE (see boundary above) |
| `every_switch_has_a_row_and_the_scan_finds_a_row_removed` (L379) | not ported | STRUCTURE (see boundary above) |
| `no_joint_source_defines_a_setter_and_the_scan_finds_a_planted_one` (L429) | not ported | STRUCTURE (see boundary above) |
| `no_joint_source_allows_a_wildcard_arm_and_the_scan_finds_a_planted_one` (L453) | not ported | STRUCTURE (see boundary above) |
| `the_cores_record_of_where_a_peer_is_is_read_only_for_the_oracles_and_the_scan_finds_a_planted_read` (L533) | not ported | STRUCTURE (see boundary above) |
| `no_settlement_test_names_the_retired_rekey_transcription_and_the_scan_finds_a_planted_one` (L588) | not ported | STRUCTURE (see boundary above) |
### `tests/oracle.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `right_link_fires_when_both_ends_of_a_crossing_settle_on_the_larger_nodes_dial` (L660) | not ported | WORLD (see boundary above) |
| `one_session_fires_on_complementary_indices_with_no_fresh_frame` (L683) | not ported | WORLD (see boundary above) |
| `a_crossing_on_the_connection_oriented_transport_with_its_frames_lost_leaves_every_connection_in_use` (L701) | not ported | WORLD (see boundary above) |
| `model_parties_settle_on_every_start` (L738) | not ported | WORLD (see boundary above) |
| `classified_fires_on_a_refusal_the_model_does_not_make` (L775) | not ported | WORLD (see boundary above) |
| `a_split_of_another_cause_on_a_shaped_path_is_not_exempt` (L880) | not ported | WORLD (see boundary above) |
| `the_split_a_shape_left_is_exempt_after_both_ends_rekey` (L956) | not ported | WORLD (see boundary above) |
### `tests/parity.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_trace_reads_back_as_it_was_written` (L24) | ported | `test_settle_parity.c` +791 pinned parity parser rows. |
| `a_script_names_no_node_or_transport_it_does_not_hold` (L82) | ported | Native planted-script scans and all46 pinned schedule descriptors. |
| `every_script_has_its_lines_and_every_line_its_script` (L175) | ported | Native missing/duplicate/orphan/settings/restated-field fixtures and C core overrides. |
| `a_core_line_names_a_listed_cause` (L251) | ported | Both named causes and all original bad-cause/empty-field plants; atomic parse failure. |
### `tests/regress.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `after_a_late_second_dial_answered_in_order_both_ends_hold_one_session_larger_dialer` (L96) | not ported | WORLD (see boundary above) |
| `after_a_late_second_dial_answered_in_order_both_ends_hold_one_session_smaller_dialer` (L101) | not ported | WORLD (see boundary above) |
| `after_a_late_second_dial_answered_in_the_other_order_the_link_dead_timer_and_the_redial_settle_one_session_within_a_heal_larger_dialer` (L157) | not ported | WORLD (see boundary above) |
| `after_a_late_second_dial_answered_in_the_other_order_the_link_dead_timer_and_the_redial_settle_one_session_within_a_heal_smaller_dialer` (L163) | not ported | WORLD (see boundary above) |
| `after_a_dial_on_the_peers_aged_link_the_peer_answers_the_larger_nodes_rekey` (L255) | not ported | WORLD (see boundary above) |
| `after_a_dial_on_the_peers_young_link_the_peer_answers_the_larger_nodes_rekey` (L267) | not ported | WORLD (see boundary above) |
| `a_removed_peers_link_leaves_no_entry_in_the_address_index` (L324) | not ported | WORLD (see boundary above) |
| `a_crossing_over_two_transports_settles_on_one_session` (L343) | not ported | WORLD (see boundary above) |
| `a_crossing_moves_the_smaller_node_with_a_reset_and_keeps_the_maps` (L349) | not ported | WORLD (see boundary above) |
| `a_stored_answer_whose_send_failed_installs_at_the_resend` (L355) | not ported | WORLD (see boundary above) |
| `a_restart_dial_inside_the_heard_floor_is_judged_on_one_clock` (L371) | not ported | WORLD (see boundary above) |
| `a_replayed_msg1_that_promoted_an_unheard_peering_stays_refused` (L377) | not ported | WORLD (see boundary above) |
| `an_own_dial_answered_from_a_restarted_peer_replaces_the_stale_session` (L411) | not ported | WORLD (see boundary above) |
| `a_second_restart_inside_the_dampener_interval_is_refused` (L443) | not ported | WORLD (see boundary above) |
### `tests/shadow.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_msg1_on_a_proven_pair_is_compared_as_on_the_link_and_a_refusal_there_still_reds` (L548) | not ported | WORLD (see boundary above) |
| `a_frame_under_the_drained_session_counts_as_heard_at_the_next_gate` (L619) | not ported | WORLD (see boundary above) |
| `a_redial_over_a_connection_opened_again_is_compared_with_the_address_open` (L677) | not ported | WORLD (see boundary above) |
| `a_deployed_releases_refusal_of_a_redial_is_named_and_a_redialing_nodes_is_a_difference` (L733) | not ported | WORLD (see boundary above) |
| `the_late_dial_resync_closes_what_the_model_closes_when_its_fallback_is_dropped` (L762) | not ported | WORLD (see boundary above) |
### `tests/shapes.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_stale_frame_moves_a_late_dialer_back_onto_the_session_its_peer_dropped` (L92) | not ported | WORLD (see boundary above) |
| `observed_a_late_msg1_from_the_peers_previous_life_is_accepted_as_a_restart` (L100) | not ported | WORLD (see boundary above) |
| `observed_a_replayed_msg1_from_the_peers_previous_life_is_accepted_as_a_restart` (L112) | not ported | WORLD (see boundary above) |
| `observed_a_restarted_node_installed_from_the_peers_rekey_is_parted_by_its_late_crossing_dial` (L125) | not ported | WORLD (see boundary above) |
| `each_starts_own_script_shows_only_the_shape_it_carries` (L164) | not ported | WORLD (see boundary above) |
| `the_two_defects_left_for_later_show_their_shapes_in_the_current_and_deployed_node` (L182) | not ported | WORLD (see boundary above) |
### `tests/sim.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `seeded_faulty_runs_on_the_connectionless_transport_keep_the_properties_and_heal` (L527) | not ported | WORLD (see boundary above) |
| `seeded_faulty_runs_on_the_connection_oriented_transport_keep_the_properties_and_heal` (L532) | not ported | WORLD (see boundary above) |
| `seeded_faulty_runs_on_ble_keep_the_properties_and_heal` (L537) | not ported | WORLD (see boundary above) |
| `seeded_faulty_runs_on_two_transports_keep_the_properties_and_heal` (L542) | not ported | WORLD (see boundary above) |
| `seeded_runs_that_reached_a_resynced_difference_keep_the_properties_and_heal` (L560) | not ported | WORLD (see boundary above) |
| `seeded_clean_runs_on_every_transport_model_keep_the_properties_and_heal` (L567) | not ported | WORLD (see boundary above) |
| `a_seeded_run_replays_byte_identically_from_its_seed_and_from_its_schedule` (L574) | not ported | WORLD (see boundary above) |
| `a_planted_failing_run_shrinks_to_its_cause` (L616) | not ported | WORLD (see boundary above) |
### `tests/table.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_restarted_peers_epoch_its_rekey_took_makes_its_msg1_at_that_epoch_a_rekey` (L251) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_path_moved_rekeys_the_links_index_entry_to_the_new_pair` (L278) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_path_switched_adds_the_new_pair_to_the_index` (L300) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_path_released_drops_its_pair_and_closes_unless_in_use_or_kept` (L308) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_path_state_change_to_probing_stops_it_counting_as_in_use` (L343) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_msg1_from_a_proven_path_is_on_the_link` (L358) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_capable_peers_bare_setup_pair_is_off_the_link_in_the_table` (L425) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_msg1_from_the_pair_the_core_holds_the_peer_at_with_no_path_there_is_off_the_link` (L460) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_msg1_from_a_pair_whose_path_is_probing_is_off_the_link` (L476) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_msg1_from_a_setup_pair_the_index_names_another_link_at_is_off_the_link` (L515) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_punched_input_opens_a_dial_leg_at_the_punched_pair` (L545) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_failed_msg2_send_keeps_the_stored_answer_for_the_resend` (L559) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `an_unsent_dial_resend_is_tried_again_at_the_next_tick_and_not_before` (L576) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_failed_dial_resend_is_due_at_the_next_poll_whatever_its_instant` (L623) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_send_failure_after_a_dials_last_resend_asks_for_no_more` (L679) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `an_answer_whose_msg2_cannot_be_sent_is_given_up_and_closes_nothing` (L728) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_msg1_from_the_address_of_an_unsent_answer_draws_that_answer_whatever_its_bytes` (L753) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `an_orphaned_bundle_removes_the_peer_and_retires_its_link` (L773) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `an_orphaned_bundles_peer_is_retried` (L790) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_restarted_or_replaced_peer_is_retried_in_each_of_the_three_ways` (L808) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `an_orphaned_peer_closes_its_bundles_paths_and_not_a_setup_pair_released_to_keep` (L872) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_teardown_of_a_capable_link_closes_each_path_but_the_one_the_peer_sends_on` (L924) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_link_not_known_capable_keeps_its_other_paths_at_an_operator_disconnect` (L951) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_link_not_known_capable_keeps_its_other_paths_at_a_move` (L965) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `an_operator_disconnect_closes_the_peers_pair_though_a_dial_uses_it` (L1003) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_msg1_past_the_admission_cap_is_rejected_and_holds_no_leg` (L1023) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_crossing_moves_the_smaller_node_and_retires_the_inbound_link` (L1075) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_crossing_decided_by_the_tie_break_alone_moves_the_smaller_node_onto_its_dial` (L1093) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_second_dial_to_a_peer_held_on_this_nodes_own_dial_moves_the_peer_onto_it` (L1113) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_move_onto_this_nodes_own_dial_makes_the_peering_heard` (L1140) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_frame_moves_the_instant_of_the_peerings_last_frame` (L1157) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_move_onto_a_dial_answered_from_the_peers_other_pair_keeps_that_pair_open` (L1185) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_move_onto_a_dial_answered_from_the_peers_other_pair_closes_the_dials_own_connection` (L1207) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_move_that_makes_the_peering_heard_forgets_its_replay_memory_and_keeps_its_silent_sessions` (L1233) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_frame_on_a_peering_heard_from_its_install_keeps_the_replay_memory_and_forgets_the_silent_sessions` (L1283) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `an_own_dials_session_that_ends_before_the_peers_first_frame_counts_as_a_silent_session` (L1332) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_late_second_dial_in_order_retires_the_link_moved_off` (L1375) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_move_resets_peer_data` (L1403) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `the_table_and_the_record_read_the_mirror_the_same_way` (L1432) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_connecting_dials_pair_is_in_use_before_its_msg1_and_a_failed_connect_retires_it_with_no_lease` (L1497) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_dial_given_up_while_connecting_closes_nothing` (L1529) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_dial_after_its_connect_starts_its_leg_on_the_link_the_connect_opened` (L1554) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_move_onto_a_later_dial_to_a_peer_held_on_the_link_it_dialled_ends_the_pin` (L1625) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_move_onto_a_dial_made_before_the_peer_was_held_keeps_the_pin` (L1646) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_second_dial_made_before_the_peer_was_held_and_answered_after_a_crossing_ends_the_pin` (L1667) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_refused_msg1_asks_for_no_answer` (L1725) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_rekey_msg1_asks_for_no_answer` (L1746) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `an_abandoned_answer_frees_its_link_and_takes_no_lease` (L1765) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_restarts_teardown_comes_before_its_answer_is_asked_for` (L1800) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_msg1_from_a_new_identity_at_the_peer_cap_is_refused_before_any_answer` (L1854) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_replacement_for_an_identity_not_held_at_the_peer_cap_is_refused_before_any_answer` (L1869) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_held_identity_at_the_peer_cap_restarts_as_before` (L1888) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_dial_completing_to_a_new_identity_at_the_peer_cap_is_not_installed` (L1916) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `an_answer_sent_to_a_new_identity_once_the_peer_cap_filled_is_not_installed` (L1948) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_zero_peer_cap_admits_any_number` (L1981) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_switch_moves_the_held_record_to_the_new_active_path_in_the_table` (L1990) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_frame_from_another_transport_moves_nothing_in_the_core` (L2031) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_one_path_roam_moves_the_cores_record_and_resets_once` (L2060) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_path_the_bundle_followed_moves_the_cores_record_and_resets_nothing` (L2092) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_switch_to_an_acknowledged_path_leaves_it_acknowledged` (L2131) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_redial_opens_a_leg_and_leaves_the_held_peer` (L2194) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_msg1_from_the_address_of_a_sent_answer_is_read_unless_it_is_the_msg1_answered` (L2208) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_frame_under_a_sent_redial_leg_replaces_the_held_peer_for_redial` (L2233) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_redial_leg_reaped_at_its_deadline_leaves_the_held_peer` (L2279) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_redial_leg_whose_peer_is_gone_installs_at_its_send_and_is_no_longer_a_redial` (L2306) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `one_redial_leg_per_identity` (L2332) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `the_redial_memory_refuses_a_seen_ephemeral` (L2367) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_reap_of_one_leg_reaps_only_that_leg_and_only_from_its_deadline` (L2394) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_second_redial_within_the_unheard_floor_of_the_first_is_held_back` (L2428) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_redial_within_the_dampener_of_a_swap_is_refused` (L2455) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_redials_answer_leaves_the_held_peer_until_a_frame_proves_it` (L2496) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_pending_redials_leg_is_reaped_at_its_deadline` (L2517) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_redials_leg_installed_at_its_send_is_no_longer_a_pending_redial` (L2533) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_redial_abandoned_after_its_acl_passed_is_noted_in_the_redial_memory` (L2559) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_peer_a_redials_swap_replaces_is_retried` (L2605) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_redials_swap_installs_the_peer_unheard_and_its_frames_heard_hears_it` (L2626) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `an_operator_disconnect_abandons_the_peers_pending_redial` (L2666) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
| `a_redials_answer_given_up_as_unsendable_is_no_longer_a_pending_redial` (L2698) | ported | same-named upstream assertion body in `settle/table_tests.rs`; C table trace replay |
### `tests/world.rs`

| Upstream test (attribute line) | Disposition | C coverage / reason |
|---|---|---|
| `a_frame_opens_only_under_a_session_the_receiver_holds` (L1427) | not ported | WORLD (see boundary above) |
| `two_orders_of_commuting_sends_reach_one_key` (L1505) | not ported | WORLD (see boundary above) |
| `a_party_holding_a_session_sends_a_heartbeat_at_each_heartbeat_wake` (L1521) | not ported | WORLD (see boundary above) |
| `a_connection_oriented_reply_goes_on_the_arrival_connection` (L1548) | not ported | WORLD (see boundary above) |
| `an_eof_reaches_the_far_end` (L1581) | not ported | WORLD (see boundary above) |
| `a_ble_link_holds_one_connection_per_pair` (L1606) | not ported | WORLD (see boundary above) |
| `a_dial_inside_the_connect_window_sends_nothing_until_it_connects` (L1626) | not ported | WORLD (see boundary above) |
| `what_is_in_flight_is_lost_when_the_clock_moves_and_when_its_receiver_restarts` (L1651) | not ported | WORLD (see boundary above) |
| `a_dialer_inside_its_connect_window_is_told_when_the_acceptor_restarts` (L1682) | not ported | WORLD (see boundary above) |
| `an_inbound_connection_whose_msg1_was_refused_is_closed_at_the_handshake_timeout` (L1706) | not ported | WORLD (see boundary above) |
| `a_replacement_of_an_unheard_peering_over_another_connection_closes_the_replaced_links_connection` (L1758) | not ported | WORLD (see boundary above) |
