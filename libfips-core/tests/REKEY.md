# Shared rekey tests

`FipsCoreRekeyTest` is in the `fips_core_host_test` closure. The production
library has no test hooks; this test recompiles the shared module with
`FIPS_TEST_HOOKS`. `fakes/rekey.c` is an intentionally non-production handshake
transcript oracle, using real Noise transport sessions and replay windows.

There are **91 registered C scenarios**, adapted/consolidated from the **92
Rust test functions** in the direct behavioral unit files below (28 FMP, 41
FSP, 8 next-shaped, 15 helpers). This is a many-to-many coverage map, not a
claim that every Rust digest, table row, helper or assertion was translated
literally. C also checks the exact `rekey.json` `params_debug` string, invalid
nanosecond durations, index/write failure cleanup, destroyed-machine refusal,
key-slot erasure, pending-prompt hard-limit refusal, and linear output ownership. `rk_test_conserved` runs after
every step, checking index uniqueness, leases including externally held legs,
profile candidate bounds, and cleared session/handshake/candidate slots.

### Adapted source-test mapping

#### `fmp.rs` (28)

| Upstream function | C scenario / invariant |
|---|---|
| `ik_rekey_completes_through_the_attempt_the_reply_the_poll_and_the_adoption` | `ik_cycle` |
| `a_resent_answer_follows_the_route_of_the_first_answer` | `answer_routes` |
| `with_arrival_on_an_off_link_msg1_accepted_while_unreachable_is_answered_on_the_link` | `off_link_arrival_unreachable` |
| `every_ik_msg1_cell_takes_its_rows_action` | `young_and_off_link_msg1_rows / replay_rewritten_index` |
| `larger_node_with_an_attempt_abandons_it_and_answers_a_fresh_eligible_msg1` | `crossing_msg1` |
| `replay_with_a_rewritten_sender_idx_is_refused` | `replay_rewritten_index` |
| `residual_msg1_during_an_unevidenced_drain_retires_nothing` | `residual_msg1_does_not_retire_drain` |
| `responder_never_cuts_over_on_a_timer` | `responder_hold_expires` |
| `link_age_anchor_survives_cutover` | `carry_preserves_anchors` |
| `unbind_uses_the_transport_recorded_at_bind_across_a_rebind` | `transport_recorded_at_bind` |
| `leases_outstanding_equal_leases_held` | `rk_test_conserved (every step)` |
| `unanswered_attempt_is_abandoned_and_retried_after_backoff` | `msg1_resends_and_backoff` |
| `a_trigger_starts_an_own_rekey_only_where_a_scheduled_start_would` | `trigger_floors / msg1_resends_and_backoff / dampening_and_cutover_floors` |
| `peer_on_previous_epoch_is_not_reaped_while_drain_held` | `previous_drain_retention` |
| `cutover_over_an_open_drain_releases_its_index` | `cutover_over_open_drain` |
| `reoffered_receive_copy_refuses_counters_the_lost_worker_saw` | `reoffered_worker_floor` |
| `every_worker_and_delegation_report_takes_its_rows_action` | `worker_report_rows` |
| `counter_limits_force_a_rekey_and_then_tear_down` | `counter_floors_soft_hard` |
| `every_ik_reply_cell_takes_its_rows_action` | `forged_reply_unchanged / ik_cycle` |
| `every_ik_frame_cell_takes_its_rows_action` | `frame_replay_and_short / pending_frame_before_poll / worker_report_rows` |
| `rekey_msg2_from_a_restarted_peer_retires_the_old_epoch_and_reports_the_restart_after_the_install` | `restart_order` |
| `a_frame_under_the_pending_epoch_before_the_poll_cuts_over_with_the_k_fixed_at_completion` | `pending_frame_before_poll` |
| `a_frame_for_a_delegated_previous_epoch_is_refused_by_the_machine` | `worker_report_rows` |
| `cross_connection_move_keeps_the_link_and_session_anchors` | `carry_preserves_anchors` |
| `refused_msg1_copy_never_arms_a_pending` | `refused_msg1_never_arms_pending` |
| `initiator_epoch_takes_the_inverse_of_the_peers_observed_k_not_of_its_own` | `peer_k_not_own_k` |
| `close_in_every_state_retires_abandons_unbinds_and_releases_everything_held` | `close_every_state / rk_test_close` |
| `cross_connection_move_keeps_the_time_the_peer_was_last_heard` | `carry_preserves_anchors` |

#### `fsp.rs` (41)

| Upstream function | C scenario / invariant |
|---|---|
| `xk_rekey_completes_with_probes_and_the_responders_prompt` | `xk_cycle` |
| `every_xk_setup_cell_takes_its_rows_action` | `setup_rows` |
| `every_xk_ack_cell_takes_its_rows_action` | `ack_rows` |
| `completion_with_another_static_installs_nothing` | `other_static_no_install` |
| `every_xk_final_cell_takes_its_rows_action` | `final_rows` |
| `stale_candidate_never_completes_after_a_cutover_on_evidence` | `stale_candidate_after_evidence` |
| `late_msg3_after_a_newer_setup_keeps_the_newer_candidate` | `late_final_keeps_new_candidate` |
| `current_epoch_frame_after_the_last_probe_marks_the_peer_as_staying` | `peer_stays_after_probe` |
| `probe_grace_never_outlasts_the_confirmation_deadline` | `grace_cap` |
| `late_poll_never_sends_a_probe_it_cannot_judge_before_the_confirmation_deadline` | `late_poll_no_unjudgeable_probe` |
| `no_seal_call_produces_a_ciphertext_under_a_pending_epoch` | `no_pending_data_seal` |
| `responders_only_prompt_lost_loses_no_initiator_frame` | `lost_responder_prompt` |
| `a_previous_epoch_frame_after_an_assumed_cutover_returns_the_epoch_to_pending` | `assumed_revert_and_expiry` |
| `assumed_epoch_frame_during_a_repair_reverts_and_abandons_the_repair` | `repair_revert` |
| `drained_epoch_frame_during_a_confirming_repair_marks_the_peer_as_staying` | `drained_frame_during_repair_confirmation` |
| `hs_rtt_ignores_a_reply_to_a_resent_message` | `resent_reply_not_rtt_sample` |
| `black_holed_sender_rekeys_within_silence` | `silence_repair` |
| `silence_step_doubles_per_start_and_a_heard_frame_resets_it_the_anchor_and_the_data_flag` | `silence_frame_resets / silence_repair` |
| `fsp_ack_from_a_restarted_peer_cuts_over_at_msg3_and_retains_it` | `restart_at_ack` |
| `previous_epoch_frame_with_a_counter_inside_the_current_window_is_delivered` | `previous_counter_overlap` |
| `forged_setups_never_defer_or_cancel_own_rotation` | `forged_setup_preserves_rotation` |
| `forged_frames_change_nothing` | `forged_frames_unchanged` |
| `first_handshake_installs_at_both_ends_and_the_first_frame_confirms_the_retained_msg3` | `first_handshake` |
| `an_unconfirmed_first_handshake_msg3_is_resent_five_times_then_repaired` | `first_retained_msg3_resends` |
| `inbound_opening_with_no_candidate_left_is_torn_down` | `opening_expiry` |
| `forged_setup_before_a_wanted_session_never_delays_its_first_attempt` | `open_not_blocked_by_candidate` |
| `forged_setups_on_one_carrier_cannot_evict_another_carriers_candidate` | `carrier_eviction_preserves_other` |
| `the_smaller_node_refuses_a_crossing_setup_while_its_attempt_is_young_and_the_larger_holds_back_early_resends` | `young_crossing_resends` |
| `first_handshake_candidate_is_dropped_once_the_peer_is_heard_under_cur` | `first_crossing_heard_drops_candidate` |
| `late_first_handshake_setup_copy_never_moves_the_smaller_node_off_its_confirmed_epoch` | `late_first_setup_refused` |
| `restart_crossing_with_a_smaller_restarted_peer_converges_on_its_epoch` | `restart_crossing_converges` |
| `previous_epoch_frame_never_confirms_or_clears_retained_msg3` | `previous_does_not_confirm` |
| `late_poll_processes_stored_deadlines_in_order` | `late_deadlines_ordered` |
| `session_address_is_stable_across_cutover_demotion_and_revert` | `assumed_revert_and_expiry` |
| `assumed_cutover_at_the_larger_node_keeps_an_acknowledged_setup` | `assumed_larger_keeps_candidate` |
| `deployed_rekey_acknowledged_before_the_first_handshake_is_confirmed_completes_at_the_larger_node` | `later_candidate_survives_first_confirmation` |
| `no_rekey_starts_without_a_route_and_one_starts_once_there_is_one` | `unreachable_gate` |
| `first_rekey_after_answering_the_first_handshake_waits_for_the_measured_rtt` | `measured_responder_rtt` |
| `rekey_setup_first_resend_waits_twice_the_session_rtt_up_to_the_rung_cap` | `srtt_first_rung` |
| `fsp_completion_with_a_changed_epoch_reports_a_restart` | `final_restart` |
| `no_own_rekey_starts_inside_dampening_or_the_drain_window_after_a_cutover` | `dampening_and_cutover_floors` |

#### `next.rs` (8)

| Upstream function | C scenario / invariant |
|---|---|
| `delayed_initiator_cuts_over_after_its_delay_and_the_responder_on_its_frame` | `delayed_cutover` |
| `retained_msg3_never_outlives_its_ladder` | `retained_delayed_ladder` |
| `every_leg_completion_cell_takes_its_rows_action` | `leg_refusal_and_release / leg_tie_break` |
| `completed_own_attempts_meet_a_completion_by_the_tie_break` | `leg_tie_break` |
| `a_completion_while_the_own_delayed_epoch_is_unheard_is_decided_by_the_tie_break` | `delayed_completion_tie` |
| `impostor_reply_never_reports_a_restart` | `impostor_reply_no_restart` |
| `a_frame_driven_session_responder_holds_its_completion_until_the_initiators_frame` | `frame_driven_responder` |
| `a_previous_epoch_frame_beside_a_responder_pending_after_an_assumed_cutover_changes_no_epoch` | `previous_beside_responder_pending` |

#### `params.rs` (5)

| Upstream function | C scenario / invariant |
|---|---|
| `params_reject_after_secs_not_above_jitter` | `after_jitter` |
| `params_reject_zero_after_messages` | `zero_messages` |
| `params_reject_after_secs_below_jitter_plus_min_link_age` | `after_link_floor` |
| `params_reject_settings_under_which_a_timer_fires_on_every_poll_or_never` | `invalid_settings` |
| `derived_values_match_their_derivations_at_the_defaults` | `derived_defaults` |

#### `ladder.rs` (3)

| Upstream function | C scenario / invariant |
|---|---|
| `doubling_ladder_keeps_only_rungs_before_its_deadline_and_counts_the_rest` | `doubling` |
| `probe_ladder_fills_every_gap_longer_than_the_keepalive_gap_before_its_deadline` | `probes` |
| `a_late_take_passes_every_overdue_rung_and_reports_one_send` | `late_ladder` |

#### `candidates.rs` (1)

| Upstream function | C scenario / invariant |
|---|---|
| `eviction_takes_the_oldest_unprotected_candidate_of_the_fullest_carrier_and_never_a_carriers_only_one` | `candidate_eviction` |

#### `lease.rs` (1)

| Upstream function | C scenario / invariant |
|---|---|
| `a_lease_dropped_without_release_is_counted_as_leaked_and_a_released_one_is_not` | `lease_drop` |

#### `record.rs` (3)

| Upstream function | C scenario / invariant |
|---|---|
| `replay_record_never_exceeds_its_bound` | `record_bound` |
| `absorbing_an_older_record_of_the_same_peer_epoch_keeps_its_ephemerals_within_the_bound` | `record_absorb` |
| `the_record_store_hands_a_record_back_only_for_its_peer_and_epoch_and_drops_the_least_recently_filed_past_its_bound` | `record_store` |

#### `reason.rs` (1)

| Upstream function | C scenario / invariant |
|---|---|
| `local_faults_are_never_attributed_to_the_peer_and_unauthenticated_causes_are_rate_limited` | `attribution` |

#### `resolve.rs` (1)

| Upstream function | C scenario / invariant |
|---|---|
| `both_ends_name_the_same_survivor_for_every_pair` | `agreement` |

## Deliberately excluded upstream infrastructure

The following **20 Rust `#[test]` functions**, and their supporting models,
are not ported by this shared-core task:

| Area | Test functions | Reason |
|---|---:|---|
| `mod.rs`, `survivors.rs` | 3 | Rust source/visibility/mutant source scanners, not machine behavior |
| `stub.rs` | 1 | Self-test of the Rust-only fake transport; C uses landed Noise transport tests |
| `faults.rs` | 1 | Mutant/process fault-injection runner |
| `sim.rs`, `explore.rs`, `regress.rs` | 7 (5+1+1) | Simulator, bounded explorer and simulator-driven historical schedules |
| `legacy/**` | 8 | Twelve representative differential traces from the actual v0.5.x models; not the full fault/explorer suite; `nostrc-f12v2.3` |

`oracles.rs` has no test functions; its simulator-only oracle framework and
Rust test-shell internals are not shipped. The C event fuzzer is not a claim
of equivalence to the simulator/explorer or legacy interoperability coverage.
Production IK/XK profile tests and the 16-event `rekey.json` replay are now
implemented by `nostrc-f12v2.2`; see below.

## Fuzzing and validation

`fips_fuzz_rekey` links the production machine (no hooks), deterministic stub
profile, and real Noise transport. It advances a supplied clock and exercises
poll, delivery/reorder/corruption/drop, application sends, silence/path facts,
and close. The assertions cover index uniqueness/conservation, bounded slots,
completed-handshake provenance for new epochs, completed pending/previous
provenance for timer/frame cutovers and reverts, and destroyed-machine
inertness. `fuzz/corpus/rekey/` contains IK, XK and crossing/silence sequences.
Run with ASan/UBSan and pass a **copy** of the seed directory as writable corpus;
libFuzzer may add/minimize entries there.

The fake owns packet queues and executes outputs synchronously. `Release`
returns an allocated local index, `Prompt` reserves/seals immediately, and
`outputs_clear` destroys unretained worker clones. Any real host must provide
those same ownership guarantees, not just discard an action list.

## Production profile evidence (nostrc-f12v2.2)

`FipsCoreRekeyProfilesTest` has 17 real-Noise scenarios: failed IK/XK reads on
immutable forks; IK and XK full cycles in both initiator roles; crossed rekeys
and lost-setup resend/backoff ladders for both; restart replacement/zeroization
for both; current-coordinate wrapping around unchanged XK retry bytes; rewritten
IK sender-index replay proofs; failed-send lease cleanup; table-driven first XK
handshake/eviction; indexed table import/attempt/pending/previous/retired lookup/close; and
oldest-idle eviction with stable-slot reuse. Every cycle
checks cleared arena slots and lease conservation, stable session addresses and
zeroed retired key storage. The test target recompiles only the shared machine
with FIPS_TEST_HOOKS; profile/codec/Noise/executor/table code is production code.

`FipsDiffRekey` reports **1 checked, 0 skipped, 0 failed** for the complete
16-event trace, comparing actual initial XK sessions before any frame exports,
all before/after/initial/final fields, each ordered action and wire byte, prompt
and application plaintext/counter/K, and authenticated delivery slot/tag/range.
The exporter clock and actual next-wake schedule are checked, not guessed.
No C/vector divergence was observed while enabling the trace. One successful
XK trace is not FMP differential, legacy interop, or fault-model equivalence.

`fips_fuzz_rekey_profile` uses real IK/XK profiles, production triggers (no
hooks), strict envelope ingress, ordered execution, bounded loss/reorder/
corruption, application traffic and path changes. It checks bounds, key erasure
and index conservation after every operation. Writable fuzz corpus is always a
copy of `fuzz/corpus/rekey_profile`, never the checked-in seeds themselves.

## Legacy 0.5.x differential replay (nostrc-f12v2.3)

`FipsDiffRekeyLegacy` checks **12 complete traces, 0 skipped**: four for the
shared v0.5.0/v0.5.1 model, four for v0.5.2 and four for v0.5.3. Each model
runs FMP/IK and FSP/XK in both rekey-initiation directions. The traces are in
`vectors/rekey-legacy/` (792 current-node events total), generated from the
actual pinned Rust `tests/legacy/{fmp,fsp}.rs` models, not reconstructed peers.
See `tools/fips-vectors/README.md` for capture/provenance and regeneration.

The existing `check_rekey()` FSP replay remains in place; its state/output
comparison is extended for indexed FMP and one-current-node legacy transcripts.
Replay uses the real C machine, real IK/XK profiles and production FMP receive /
authenticated-msg1 ingress, with the real seeded index allocator. It compares
every ordered output, exact Send and sealed Prompt/data bytes, authenticated
Deliver plaintext/ranges, all event before/after views, and initial/final state.
Release effects are returned to the allocator and close must conserve indices.
Every missing matrix document or malformed/stale-pin/sidecar record fails before
dispatch; no compatibility case is counted as a skip.

The fixture shell establishes real Noise sessions, exchanges old-key traffic,
forces a single rotation trigger at 60 s, then sends authenticated traffic both
ways and follows current-node wakes and legacy ticks through cutover and drain.
Current-node events have explicit entropy checkpoints so the replay does not
need a second copy of the model. This is representative local byte/state parity,
not exhaustive loss/retry/restart/first-handshake coverage, production trigger
qualification, live deployed-node interop or hosted CI evidence. No C behaviour
divergence was found in this matrix; no production implementation was changed.
