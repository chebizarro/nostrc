/* Behavioral ports of core/src/proto/stp/tests/{state,classify,limits,wire,acks}.
 * Coordinate-only tests already live in test_coord.c. All time is TestClock. */
#include <fips/proto/stp/mod.h>
#include "fakes/host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x);                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#define OK(x) CHECK((x) == FIPS_OK)
#define A(x) addr(x)
#define PATH(...) (const uint8_t[]){__VA_ARGS__}, sizeof((uint8_t[]){__VA_ARGS__})
static fips_tree_state *s;
static fips_stp_peer *storage;
static fips_tree_coordinate *coords, *dest;
static fips_tree_announce *announce, *decoded;
static fips_test_clock clock_value;
static fips_stp_ranking ranking;
static fips_stp_peer_cost costs[8];
static fips_node_addr skips[8];
static fips_node_addr addr(uint8_t n) {
    fips_node_addr a = {{0}};
    a.bytes[0] = n;
    return a;
}
static bool same(fips_node_addr a, fips_node_addr b) { return !memcmp(a.bytes, b.bytes, 16); }
static fips_instant at(uint64_t ms) {
    clock_value.millis = ms;
    fips_clock c = fips_test_clock_adapter(&clock_value);
    return c.now(c.ctx);
}
static void path(fips_tree_coordinate *c, const uint8_t *p, size_t n) {
    c->len = (uint16_t)n;
    for (size_t i = 0; i < n; i++)
        c->entries[i] = fips_coord_entry_addr_only(A(p[i]));
}
static void init(uint8_t me) {
    fips_tree_state_new(s, A(me), 1000, at(0), storage, 8);
    ranking = (fips_stp_ranking){0};
}
static void place(uint8_t me, const uint8_t *p, size_t n) {
    init(me);
    path(&s->my_coords, p, n);
    s->root = A(p[n - 1]);
}
static void put(uint8_t id, uint64_t seq, const uint8_t *p, size_t n) {
    path(coords, p, n);
    fips_parent_declaration d = fips_parent_declaration_new(A(id), A(p[n > 1 ? 1 : 0]), seq, 1000);
    bool updated = false;
    if (fips_tree_state_update_peer(s, &d, coords, &updated) != FIPS_OK || !updated)
        abort();
}
static void parent(uint8_t id, uint64_t seq, uint64_t ms) {
    fips_tree_state_set_parent(s, A(id), seq, 1000, at(ms));
    if (fips_tree_state_recompute_coords(s) != FIPS_OK)
        abort();
}
static void cost(uint8_t id, double value) {
    costs[ranking.cost_count++] = (fips_stp_peer_cost){A(id), value};
    ranking.peer_costs = costs;
}
static fips_parent_eval eval(void) { return fips_tree_state_evaluate_parent(s, &ranking); }
static int target(int id) {
    fips_parent_eval e = eval();
    return id < 0 ? e.kind == FIPS_PARENT_None
                  : e.kind != FIPS_PARENT_None && same(e.peer, A((uint8_t)id));
}
static fips_tree_decision classify_parent(bool suppressed) {
    return fips_stp_classify_announce(s, A(4), &ranking, suppressed);
}
static int test_parent_declaration_new(void) {
    fips_parent_declaration d = fips_parent_declaration_new(A(1), A(2), 1, 1000);
    CHECK(same(d.node_addr, A(1)) && same(d.parent_id, A(2)) && d.sequence == 1 &&
          d.timestamp == 1000);
    CHECK(!fips_parent_declaration_is_root(&d) && !d.is_signed);
    return 0;
}
static int test_parent_declaration_self_root(void) {
    fips_parent_declaration d = fips_parent_declaration_self_root(A(1), 5, 2000);
    CHECK(fips_parent_declaration_is_root(&d) && d.sequence == 5);
    return 0;
}
static int test_parent_declaration_freshness(void) {
    fips_parent_declaration a = fips_parent_declaration_new(A(1), A(2), 1, 1000),
                            b = fips_parent_declaration_new(A(1), A(2), 2, 2000);
    CHECK(fips_parent_declaration_is_fresher_than(&b, &a));
    CHECK(!fips_parent_declaration_is_fresher_than(&a, &b));
    CHECK(!fips_parent_declaration_is_fresher_than(&a, &a));
    return 0;
}
static int test_parent_declaration_signing_bytes(void) {
    fips_parent_declaration d = fips_parent_declaration_new(A(1), A(2), 100, 1234567890);
    uint8_t b[48];
    fips_writer w;
    fips_writer_init(&w, b, 48);
    OK(fips_parent_declaration_signing_bytes(&d, &w, NULL));
    CHECK(w.pos == 48 && b[0] == 1 && b[16] == 2 && b[32] == 100 && b[40] == 0xd2 &&
          b[41] == 0x02 && b[42] == 0x96 && b[43] == 0x49);
    fips_reader r;
    fips_parent_declaration out;
    fips_reader_init(&r, b, 48);
    OK(fips_parent_declaration_decode(&r, &out, NULL));
    CHECK(fips_parent_declaration_equal(&d, &out) && r.pos == 48);
    return 0;
}
static int test_parent_declaration_equality(void) {
    fips_parent_declaration a = fips_parent_declaration_new(A(1), A(2), 1, 1000), b = a;
    uint8_t sig[64] = {0};
    fips_parent_declaration_set_signature(&b, sig);
    CHECK(fips_parent_declaration_equal(&a, &b));
    b.sequence++;
    CHECK(!fips_parent_declaration_equal(&a, &b));
    return 0;
}
static int test_tree_state_new(void) {
    init(1);
    CHECK(fips_tree_state_is_root(s) && same(s->root, A(1)) && s->my_coords.len == 1 &&
          s->peer_count == 0);
    return 0;
}
static int test_tree_state_update_peer(void) {
    init(0);
    put(1, 1, PATH(1, 2));
    CHECK(s->peer_count == 1 && fips_tree_state_peer(s, A(1))->has_declaration);
    bool updated = true;
    fips_parent_declaration d = fips_parent_declaration_new(A(1), A(2), 1, 1000);
    OK(fips_tree_state_update_peer(s, &d, coords, &updated));
    CHECK(!updated);
    d.sequence = 2;
    OK(fips_tree_state_update_peer(s, &d, coords, &updated));
    CHECK(updated);
    return 0;
}
static int test_tree_state_remove_peer(void) {
    init(0);
    put(1, 1, PATH(1, 2));
    fips_tree_state_remove_peer(s, A(1));
    CHECK(!s->peer_count && !fips_tree_state_peer(s, A(1)));
    return 0;
}
static int test_tree_state_distance_to_peer(void) {
    init(0);
    put(1, 1, PATH(1));
    size_t d = 0;
    CHECK(fips_tree_state_distance_to_peer(s, A(1), &d) && d == SIZE_MAX);
    place(0, PATH(0, 99));
    put(1, 2, PATH(1, 99));
    CHECK(fips_tree_state_distance_to_peer(s, A(1), &d) && d == 2);
    CHECK(!fips_tree_state_distance_to_peer(s, A(7), &d));
    return 0;
}
static int test_tree_state_peer_ids(void) {
    init(0);
    put(2, 1, PATH(2));
    put(1, 1, PATH(1));
    CHECK(s->peer_count == 2 && same(s->peers[0].id, A(1)) && same(s->peers[1].id, A(2)));
    return 0;
}
static int test_evaluate_parent_picks_smallest_root(void) {
    init(5);
    put(3, 1, PATH(3, 1));
    put(7, 1, PATH(7, 2));
    CHECK(target(3));
    return 0;
}
static int test_evaluate_parent_prefers_shallowest_depth(void) {
    init(5);
    put(1, 1, PATH(1, 0));
    put(2, 1, PATH(2, 3, 4, 0));
    CHECK(target(1));
    return 0;
}
static int test_evaluate_parent_stays_root_when_smallest(void) {
    init(0);
    put(1, 1, PATH(1, 0));
    CHECK(target(-1));
    return 0;
}
static int test_evaluate_parent_no_switch_when_already_best(void) {
    init(5);
    put(1, 1, PATH(1, 0));
    parent(1, 1, 1000);
    CHECK(target(-1));
    return 0;
}
static int test_evaluate_parent_no_peers(void) {
    init(5);
    CHECK(target(-1));
    return 0;
}
static int test_evaluate_parent_depth_threshold(void) {
    init(5);
    put(2, 1, PATH(2, 6, 7, 0));
    parent(2, 1, 1000);
    CHECK(s->my_coords.len == 5);
    put(3, 1, PATH(3, 0));
    CHECK(target(3));
    return 0;
}
static int test_evaluate_parent_rejects_loop_candidate(void) {
    init(5);
    put(1, 1, PATH(1, 5, 0));
    CHECK(target(-1));
    return 0;
}
static int test_evaluate_parent_picks_loop_free_over_loopy(void) {
    init(5);
    put(1, 1, PATH(1, 5, 0));
    put(2, 1, PATH(2, 3, 4, 0));
    CHECK(target(2));
    return 0;
}
static void two_peers(void) {
    init(5);
    put(1, 1, PATH(1, 0));
    put(2, 1, PATH(2, 0));
}
static int test_handle_parent_lost_finds_alternative(void) {
    two_peers();
    parent(1, 1, 1000);
    fips_tree_state_remove_peer(s, A(1));
    bool changed = false;
    OK(fips_tree_state_handle_parent_lost(s, &ranking, 1000, at(1000), &changed));
    CHECK(changed && !fips_tree_state_is_root(s) && same(s->my_declaration.parent_id, A(2)));
    return 0;
}
static int test_handle_parent_lost_becomes_root_when_self_smaller_than_remaining(void) {
    init(1);
    put(0, 1, PATH(0));
    put(2, 1, PATH(2));
    put(3, 1, PATH(3));
    parent(0, 2, 2000);
    fips_tree_state_remove_peer(s, A(0));
    fips_parent_loss loss;
    OK(fips_tree_state_recover(s, &ranking, 1000, at(1000), &loss));
    CHECK(loss.changed && fips_tree_state_is_root(s) && same(s->root, A(1)) &&
          s->my_coords.len == 1);
    return 0;
}
static int test_recompute_coords_demotes_when_self_smaller_than_parent_root(void) {
    init(5);
    put(7, 1, PATH(7));
    parent(7, 2, 2000);
    CHECK(fips_tree_state_is_root(s) && fips_parent_declaration_is_root(&s->my_declaration) &&
          s->my_coords.len == 1);
    return 0;
}
static int test_handle_parent_lost_becomes_root(void) {
    init(5);
    put(1, 1, PATH(1, 0));
    parent(1, 1, 1000);
    fips_tree_state_remove_peer(s, A(1));
    bool changed = false;
    OK(fips_tree_state_handle_parent_lost(s, &ranking, 1000, at(1000), &changed));
    CHECK(changed && fips_tree_state_is_root(s) && s->my_declaration.sequence == 2);
    return 0;
}
static int hop(const uint8_t *p, size_t n, int want) {
    path(dest, p, n);
    fips_node_addr out = A(255);
    bool got = fips_tree_state_find_next_hop(s, dest, ranking.skip, ranking.skip_count, &out);
    CHECK(want < 0 ? !got : got && same(out, A((uint8_t)want)));
    return 0;
}
static int test_find_next_hop_chain(void) {
    place(5, PATH(5, 0));
    put(1, 1, PATH(1, 5, 0));
    put(2, 1, PATH(2, 1, 5, 0));
    return hop(PATH(2, 1, 5, 0), 2);
}
static int test_find_next_hop_chain_indirect(void) {
    place(5, PATH(5, 0));
    put(1, 1, PATH(1, 5, 0));
    return hop(PATH(2, 1, 5, 0), 1);
}
static int test_find_next_hop_toward_root(void) {
    place(5, PATH(5, 1, 0));
    put(1, 1, PATH(1, 0));
    return hop(PATH(0), 1);
}
static int test_find_next_hop_sibling(void) {
    place(5, PATH(5, 0));
    put(3, 1, PATH(3, 0));
    return hop(PATH(3, 0), 3);
}
static int test_find_next_hop_tie_breaking(void) {
    place(5, PATH(5, 0));
    put(3, 1, PATH(3, 0));
    put(2, 1, PATH(2, 0));
    return hop(PATH(4, 0), -1);
}
static int test_find_next_hop_different_root(void) {
    place(5, PATH(5, 0));
    put(1, 1, PATH(1, 0));
    return hop(PATH(3, 9), -1);
}
static int test_find_next_hop_no_peers(void) {
    place(5, PATH(5, 0));
    return hop(PATH(3, 0), -1);
}
static int test_find_next_hop_local_minimum(void) {
    place(5, PATH(5, 0));
    put(8, 1, PATH(8, 5, 0));
    return hop(PATH(3, 0), -1);
}
static int test_find_next_hop_best_of_multiple(void) {
    place(5, PATH(5, 1, 0));
    put(1, 1, PATH(1, 0));
    put(3, 1, PATH(3, 1, 0));
    return hop(PATH(7, 3, 1, 0), 3);
}
static int test_effective_depth_selects_lower_cost_deeper_peer(void) {
    init(5);
    put(1, 1, PATH(1, 0));
    put(2, 1, PATH(2, 3, 0));
    cost(1, 6);
    cost(2, 1.01);
    CHECK(target(2));
    return 0;
}
static int test_effective_depth_equal_cost_degenerates_to_depth(void) {
    init(5);
    put(1, 1, PATH(1, 0));
    put(2, 1, PATH(2, 3, 4, 0));
    cost(1, 1);
    cost(2, 1);
    CHECK(target(1));
    return 0;
}
static int test_effective_depth_tiebreak_by_node_addr(void) {
    two_peers();
    cost(2, 1);
    cost(1, 1);
    CHECK(target(1));
    return 0;
}
static int test_hysteresis_prevents_marginal_switch(void) {
    two_peers();
    parent(1, 1, 1000);
    fips_tree_state_set_parent_hysteresis(s, 0.2);
    cost(1, 2.5);
    cost(2, 2.2);
    CHECK(target(-1));
    return 0;
}
static int test_hysteresis_allows_significant_switch(void) {
    init(5);
    put(1, 1, PATH(1, 0));
    put(2, 1, PATH(2, 3, 0));
    parent(1, 1, 1000);
    fips_tree_state_set_parent_hysteresis(s, 0.2);
    cost(1, 6);
    cost(2, 1.01);
    CHECK(target(2));
    return 0;
}
static int test_cold_start_default_cost(void) {
    init(5);
    put(1, 1, PATH(1, 0));
    put(2, 1, PATH(2, 3, 4, 0));
    CHECK(target(1));
    return 0;
}
static int test_hold_down_suppresses_reeval(void) {
    two_peers();
    fips_tree_state_set_hold_down(s, 60);
    parent(1, 1, 1000);
    cost(1, 5);
    cost(2, 1);
    CHECK(fips_tree_state_is_switch_suppressed(s, at(1000)) &&
          eval().kind == FIPS_PARENT_Discretionary && target(2));
    return 0;
}
static int test_mandatory_switch_bypasses_hold_down(void) {
    two_peers();
    fips_tree_state_set_hold_down(s, 60);
    parent(1, 1, 1000);
    fips_tree_state_remove_peer(s, A(1));
    CHECK(fips_tree_state_is_switch_suppressed(s, at(1000)) &&
          eval().kind == FIPS_PARENT_Mandatory && target(2));
    return 0;
}
static int test_heterogeneous_7node_avoids_bottleneck(void) {
    two_peers();
    CHECK(target(1));
    cost(1, 1.01);
    cost(2, 6);
    CHECK(target(1));
    parent(2, 1, 1000);
    CHECK(s->my_coords.len == 3 && target(1));
    fips_tree_state_set_parent_hysteresis(s, 0.2);
    CHECK(target(1));
    return 0;
}
static int test_cost_degradation_triggers_switch(void) {
    two_peers();
    fips_tree_state_set_parent_hysteresis(s, 0.2);
    cost(1, 1.05);
    cost(2, 1.08);
    CHECK(target(1));
    parent(1, 1, 1000);
    CHECK(target(-1));
    costs[0].cost = 6;
    CHECK(target(2));
    return 0;
}
static int test_cost_improvement_within_hysteresis_no_switch(void) {
    two_peers();
    parent(1, 1, 1000);
    fips_tree_state_set_parent_hysteresis(s, 0.2);
    cost(1, 2);
    cost(2, 1.5);
    CHECK(target(-1));
    return 0;
}
static int test_single_peer_no_reeval_benefit(void) {
    init(5);
    put(1, 1, PATH(1, 0));
    cost(1, 1.05);
    CHECK(target(1));
    parent(1, 1, 1000);
    costs[0].cost = 50;
    CHECK(target(-1));
    return 0;
}
static void bridged(void) {
    init(50);
    put(60, 1, PATH(60, 40));
    put(70, 1, PATH(70, 10));
    parent(60, 2, 1000);
}
static int an_unmeasured_peer_on_a_smaller_root_is_adopted_when_no_measured_peer_reaches_it(void) {
    bridged();
    CHECK(fips_stp_classify_periodic(s, &ranking, false).kind == FIPS_TREE_Switch);
    cost(60, 1);
    CHECK(target(70) && eval().kind == FIPS_PARENT_Mandatory);
    CHECK(fips_stp_classify_periodic(s, &ranking, true).kind == FIPS_TREE_Switch &&
          fips_stp_classify_announce(s, A(70), &ranking, false).kind == FIPS_TREE_Switch);
    return 0;
}
static int parent_loss_takes_the_unmeasured_peer_on_a_smaller_root_rather_than_self_rooting(void) {
    bridged();
    put(61, 1, PATH(61, 40));
    fips_tree_state_remove_peer(s, A(60));
    cost(61, 1);
    fips_parent_loss loss;
    OK(fips_tree_state_recover(s, &ranking, 2000, at(2000), &loss));
    CHECK(loss.changed && !fips_tree_state_is_root(s) && same(s->root, A(10)) &&
          same(s->my_declaration.parent_id, A(70)));
    CHECK(fips_stp_classify_periodic(s, &ranking, false).kind == FIPS_TREE_PeriodicRebroadcast &&
          fips_stp_classify_announce(s, A(61), &ranking, false).kind == FIPS_TREE_NoChange);
    return 0;
}
static int a_measured_peer_on_the_smallest_root_still_beats_a_shallower_unmeasured_one(void) {
    init(50);
    put(60, 1, PATH(60, 62, 10));
    put(70, 1, PATH(70, 10));
    put(80, 1, PATH(80, 40));
    parent(80, 2, 1000);
    cost(60, 3);
    cost(80, 1);
    CHECK(target(60));
    return 0;
}
static int an_unmeasured_candidate_never_wins_a_cost_based_switch(void) {
    init(50);
    put(60, 1, PATH(60, 40));
    put(70, 1, PATH(70, 72, 73, 10));
    put(71, 1, PATH(71, 10));
    parent(70, 2, 1000);
    cost(60, 1);
    CHECK(target(-1));
    return 0;
}
/* classify.rs: parent P=4, node X=5. */
static void under_parent(const uint8_t *p, size_t n, uint64_t hold) {
    init(5);
    fips_tree_state_set_hold_down(s, hold);
    put(4, 1, p, n);
    parent(4, 2, 1000);
}
static int switch_to(uint8_t p, bool suppressed) {
    fips_tree_decision d = classify_parent(suppressed);
    CHECK(d.kind == FIPS_TREE_Switch && same(d.parent, A(p)) && d.new_seq == 3);
    return 0;
}
static int ancestry_update(bool suppressed) {
    fips_tree_decision d = classify_parent(suppressed);
    CHECK(d.kind == FIPS_TREE_AncestryUpdate && same(d.parent, A(4)) && d.new_seq == 3);
    return 0;
}
static int parent_moving_to_worse_root_switches_to_peer_still_on_better_root(void) {
    under_parent(PATH(4, 1), 0);
    put(2, 1, PATH(2, 1));
    put(4, 2, PATH(4));
    CHECK(eval().kind == FIPS_PARENT_Mandatory);
    return switch_to(2, false);
}
static int parent_moving_to_worse_root_switches_even_while_hold_down_is_active(void) {
    under_parent(PATH(4, 1), 30);
    put(2, 1, PATH(2, 1));
    CHECK(fips_tree_state_is_switch_suppressed(s, at(1500)));
    put(4, 2, PATH(4));
    return switch_to(2, true);
}
static int parent_ancestry_change_with_no_better_root_yields_ancestry_update(void) {
    under_parent(PATH(4, 1), 0);
    put(2, 1, PATH(2, 6, 7, 1));
    put(4, 2, PATH(4, 3, 1));
    CHECK(target(-1));
    return ancestry_update(false);
}
static int parent_moving_to_worse_root_with_no_better_root_visible_yields_ancestry_update(void) {
    under_parent(PATH(4, 1), 0);
    put(4, 2, PATH(4));
    CHECK(target(-1));
    return ancestry_update(false);
}
static int parent_moving_to_better_root_yields_ancestry_update(void) {
    under_parent(PATH(4, 1), 0);
    put(2, 1, PATH(2, 1));
    put(4, 2, PATH(4, 0));
    CHECK(target(-1));
    return ancestry_update(false);
}
static int parent_moving_to_worse_root_does_not_switch_to_sibling_reached_through_parent(void) {
    under_parent(PATH(4, 1), 0);
    put(3, 1, PATH(3, 4, 1));
    put(4, 2, PATH(4));
    CHECK(target(-1));
    return ancestry_update(false);
}
static int
parent_moving_to_worse_root_follows_parent_when_best_candidate_is_reached_through_parent(void) {
    under_parent(PATH(4, 1), 0);
    put(3, 1, PATH(3, 4, 1));
    put(2, 1, PATH(2, 6, 7, 1));
    put(4, 2, PATH(4));
    CHECK(target(-1));
    return ancestry_update(false);
}
static int parent_ancestry_change_on_same_root_leaves_better_peer_to_the_hold_down_veto(void) {
    under_parent(PATH(4, 6, 7, 1), 30);
    put(2, 1, PATH(2, 1));
    put(4, 2, PATH(4, 8, 7, 1));
    CHECK(fips_tree_state_is_switch_suppressed(s, at(1500)) &&
          eval().kind == FIPS_PARENT_Discretionary && target(2));
    return ancestry_update(true);
}
static int drive_loop(void) {
    fips_tree_decision d = classify_parent(fips_tree_state_is_switch_suppressed(s, at(1500)));
    if (d.kind == FIPS_TREE_Switch) {
        parent(d.parent.bytes[0], d.new_seq, 1500);
    } else {
        CHECK(d.kind == FIPS_TREE_LoopDrop);
        fips_parent_loss loss;
        OK(fips_tree_state_recover(s, &ranking, 3000, at(1500), &loss));
        CHECK(loss.changed);
    }
    return 0;
}
static int loop_drop_takes_a_loop_free_candidate_on_the_same_root_while_hold_down_is_active(void) {
    under_parent(PATH(4, 1), 30);
    put(2, 1, PATH(2, 1));
    put(4, 2, PATH(4, 5, 9, 1));
    CHECK(fips_tree_state_is_switch_suppressed(s, at(1500)));
    CHECK(!drive_loop());
    CHECK(!fips_tree_state_is_root(s) && same(s->my_declaration.parent_id, A(2)));
    return 0;
}
static int loop_drop_takes_a_loop_free_candidate_that_does_not_clear_the_hysteresis_margin(void) {
    under_parent(PATH(4, 1), 0);
    put(2, 1, PATH(2, 8, 7, 1));
    fips_tree_state_set_parent_hysteresis(s, 0.2);
    put(4, 2, PATH(4, 5, 9, 1));
    CHECK(!drive_loop());
    CHECK(!fips_tree_state_is_root(s) && same(s->my_declaration.parent_id, A(2)));
    return 0;
}
static int loop_drop_with_only_candidates_reached_through_this_node_still_becomes_root(void) {
    under_parent(PATH(4, 1), 0);
    put(3, 1, PATH(3, 5, 4, 1));
    put(4, 2, PATH(4, 5, 9, 1));
    CHECK(target(-1) && classify_parent(false).kind == FIPS_TREE_LoopDrop);
    CHECK(!drive_loop());
    CHECK(fips_tree_state_is_root(s) && same(s->root, A(5)));
    return 0;
}
/* limits.rs: threshold switches are actual parent changes, not redeclarations. */
static void flap_setup(uint32_t threshold, uint64_t window, uint64_t duration) {
    two_peers();
    fips_tree_state_set_flap_dampening(s, threshold, window, duration);
}
static bool flap_parent(uint8_t p, uint64_t seq, uint64_t ms) {
    bool d = fips_tree_state_set_parent(s, A(p), seq, 1000, at(ms));
    if (fips_tree_state_recompute_coords(s) != FIPS_OK)
        abort();
    return d;
}
static int test_flap_dampening_engages_after_threshold(void) {
    flap_setup(3, 60, 3600);
    CHECK(!flap_parent(1, 1, 1000));
    CHECK(!flap_parent(2, 2, 2000));
    CHECK(flap_parent(1, 3, 3000));
    cost(1, 10);
    cost(2, 1);
    CHECK(fips_tree_state_is_switch_suppressed(s, at(3000)) && target(2) &&
          eval().kind == FIPS_PARENT_Discretionary);
    return 0;
}
static int test_flap_dampening_allows_mandatory_switches(void) {
    flap_setup(3, 60, 3600);
    parent(1, 1, 1000);
    parent(2, 2, 2000);
    parent(1, 3, 3000);
    fips_tree_state_remove_peer(s, A(1));
    CHECK(fips_tree_state_is_switch_suppressed(s, at(3000)) && target(2) &&
          eval().kind == FIPS_PARENT_Mandatory);
    return 0;
}
static int test_flap_dampening_expires(void) {
    flap_setup(3, 60, 0);
    parent(1, 1, 1000);
    parent(2, 2, 2000);
    CHECK(flap_parent(1, 3, 3000));
    cost(1, 10);
    cost(2, 1);
    CHECK(!fips_tree_state_is_switch_suppressed(s, at(3000)) && target(2));
    return 0;
}
static int test_flap_dampening_below_threshold(void) {
    flap_setup(4, 60, 3600);
    parent(1, 1, 1000);
    parent(2, 2, 2000);
    parent(1, 3, 3000);
    cost(1, 10);
    cost(2, 1);
    CHECK(!fips_tree_state_is_switch_suppressed(s, at(3000)) && target(2));
    return 0;
}
static int test_flap_dampening_window_reset(void) {
    flap_setup(3, 0, 3600);
    CHECK(!flap_parent(1, 1, 1000));
    CHECK(!flap_parent(2, 2, 2000));
    CHECK(!flap_parent(1, 3, 3000));
    CHECK(!fips_tree_state_is_switch_suppressed(s, at(3000)));
    return 0;
}
static int test_flap_dampening_same_parent_no_count(void) {
    flap_setup(3, 60, 3600);
    parent(1, 1, 1000);
    for (uint64_t i = 2; i <= 5; i++)
        CHECK(!flap_parent(1, i, i * 1000));
    CHECK(!fips_tree_state_is_switch_suppressed(s, at(5000)));
    return 0;
}
static int test_flap_dampening_engages_a_second_time_after_first_episode_lapses(void) {
    flap_setup(3, 60, 10);
    CHECK(!flap_parent(1, 1, 1000));
    CHECK(!flap_parent(2, 2, 2000));
    CHECK(flap_parent(1, 3, 3000));
    CHECK(!fips_tree_state_is_switch_suppressed(s, at(14000)));
    CHECK(!flap_parent(2, 4, 14000));
    CHECK(!flap_parent(1, 5, 15000));
    CHECK(flap_parent(2, 6, 16000));
    CHECK(fips_tree_state_is_switch_suppressed(s, at(16000)));
    return 0;
}
static int test_flap_dampening_duration_at_u64_max_does_not_panic(void) {
    flap_setup(3, 60, UINT64_MAX);
    parent(1, 1, 1000);
    parent(2, 2, 2000);
    CHECK(flap_parent(1, 3, 3000));
    CHECK(fips_tree_state_is_switch_suppressed(s, at(3000)) &&
          fips_tree_state_dampening_secs(s) == 365 * 24 * 60 * 60);
    return 0;
}
static int test_parent_loss_recovery_reports_the_engagement_that_arms_dampening(void) {
    flap_setup(2, 60, 120);
    CHECK(!flap_parent(1, 1, 1000));
    fips_tree_state_remove_peer(s, A(1));
    fips_parent_loss loss;
    OK(fips_tree_state_recover(s, &ranking, 2000, at(2000), &loss));
    CHECK(loss.changed && loss.dampened && same(s->my_declaration.parent_id, A(2)) &&
          fips_tree_state_is_switch_suppressed(s, at(2000)));
    return 0;
}
static int test_parent_loss_recovery_to_self_root_reports_no_engagement(void) {
    init(5);
    fips_tree_state_set_flap_dampening(s, 1, 60, 120);
    put(1, 1, PATH(1, 0));
    parent(1, 1, 1000);
    fips_tree_state_remove_peer(s, A(1));
    fips_parent_loss loss;
    OK(fips_tree_state_recover(s, &ranking, 2000, at(2000), &loss));
    CHECK(loss.changed && !loss.dampened && fips_tree_state_is_root(s));
    return 0;
}
static int test_handle_parent_lost_keeps_its_published_bool_return(void) {
    /* C has a status + bool out rather than Rust's infallible bool; keep the
     * boolean wrapper separate from the richer recovery result. */
    fips_status (*published)(fips_tree_state *, const fips_stp_ranking *, uint64_t, fips_instant,
                             bool *) = fips_tree_state_handle_parent_lost;
    init(5);
    put(1, 1, PATH(1, 0));
    parent(1, 1, 1000);
    fips_tree_state_remove_peer(s, A(1));
    bool changed = false;
    OK(published(s, &ranking, 2000, at(2000), &changed));
    CHECK(changed && fips_tree_state_is_root(s));
    return 0;
}
static int test_flap_dampening_counter_resets_when_the_episode_lapses_not_when_it_engages(void) {
    flap_setup(3, 60, 2);
    CHECK(!flap_parent(1, 1, 1000));
    CHECK(!flap_parent(2, 2, 2000));
    CHECK(flap_parent(1, 3, 3000));
    CHECK(!flap_parent(2, 4, 4000));
    CHECK(fips_tree_state_is_switch_suppressed(s, at(4000)));
    CHECK(!fips_tree_state_is_switch_suppressed(s, at(6000)));
    CHECK(!flap_parent(1, 5, 6000));
    CHECK(!flap_parent(2, 6, 7000));
    CHECK(flap_parent(1, 7, 8000));
    return 0;
}
static int test_hold_down_runs_from_the_last_parent_change_not_from_each_ancestry_update(void) {
    under_parent(PATH(4, 6, 7, 1), 30);
    put(2, 1, PATH(2, 1));
    for (uint64_t i = 1; i <= 10; i++) {
        put(4, 1 + i, PATH(4, 6, 7, 1));
        parent(4, 2 + i, 1000 + i * 29000);
    }
    fips_tree_state_set_parent_hysteresis(s, 0.2);
    CHECK(eval().kind == FIPS_PARENT_Discretionary && target(2) &&
          !fips_tree_state_is_switch_suppressed(s, at(292000)));
    return 0;
}
/* wire.rs uses opaque signatures: cryptographic signing/parity is exercised by
 * the seeded differential check; byte parsing accepts all 64-byte signatures. */
static void make_announce(const uint8_t *p, size_t n, uint64_t seq) {
    path(&announce->ancestry, p, n);
    announce->declaration = fips_parent_declaration_new(A(p[0]), A(p[n > 1 ? 1 : 0]), seq, 5000);
    uint8_t sig[64];
    memset(sig, 0x5a, 64);
    fips_parent_declaration_set_signature(&announce->declaration, sig);
    for (size_t i = 0; i < n; i++) {
        announce->ancestry.entries[i].sequence = seq - i;
        announce->ancestry.entries[i].timestamp = 5000 - i * 1000;
    }
}
static uint8_t wire_bytes[512];
static size_t encode(void) {
    fips_writer w;
    fips_writer_init(&w, wire_bytes, sizeof(wire_bytes));
    if (fips_tree_announce_encode(announce, &w, NULL) != FIPS_OK)
        abort();
    return w.pos;
}
static int test_tree_announce(void) {
    make_announce(PATH(1, 2, 0), 5);
    CHECK(same(announce->declaration.node_addr, A(1)) &&
          fips_tree_coordinate_depth(&announce->ancestry) == 2);
    return 0;
}
static int roundtrip(size_t count, size_t expected) {
    size_t n = encode();
    CHECK(n == expected && wire_bytes[0] == 0x10);
    fips_reader r;
    fips_reader_init(&r, wire_bytes + 1, n - 1);
    OK(fips_tree_announce_decode(&r, decoded, NULL));
    CHECK(r.pos == n - 1 && decoded->ancestry.len == count &&
          fips_parent_declaration_equal(&announce->declaration, &decoded->declaration) &&
          !memcmp(announce->declaration.signature, decoded->declaration.signature, 64));
    for (size_t i = 0; i < count; i++)
        CHECK(
            fips_coord_entry_equal(&announce->ancestry.entries[i], &decoded->ancestry.entries[i]));
    return 0;
}
static int test_tree_announce_encode_decode_root(void) {
    make_announce(PATH(9), 1);
    CHECK(!roundtrip(1, 132));
    CHECK(fips_parent_declaration_is_root(&decoded->declaration) && decoded->declaration.is_signed);
    return 0;
}
static int test_tree_announce_encode_decode_depth3(void) {
    make_announce(PATH(9, 2, 3, 4), 5);
    CHECK(!roundtrip(4, 228));
    CHECK(!fips_parent_declaration_is_root(&decoded->declaration));
    return 0;
}
static int test_tree_announce_decode_unsupported_version(void) {
    make_announce(PATH(9), 1);
    size_t n = encode();
    wire_bytes[1] = 2;
    fips_reader r;
    fips_proto_error e;
    fips_reader_init(&r, wire_bytes + 1, n - 1);
    CHECK(fips_tree_announce_decode(&r, decoded, &e) == FIPS_ERR_RANGE &&
          e.variant == FIPS_PROTO_UnsupportedVersion && e.as.unsupported_version == 2 && !r.pos);
    return 0;
}
static int test_tree_announce_decode_truncated(void) {
    make_announce(PATH(9, 2, 3, 4), 5);
    size_t n = encode();
    for (size_t i = 0; i < n - 1; i++) {
        fips_reader r;
        fips_proto_error e;
        fips_reader_init(&r, wire_bytes + 1, i);
        CHECK(fips_tree_announce_decode(&r, decoded, &e) == FIPS_ERR_RANGE &&
              e.variant == FIPS_PROTO_MessageTooShort && !r.pos &&
              e.as.message_too_short.expected == (i < 99 ? 99 : n - 1) &&
              e.as.message_too_short.got == i);
    }
    return 0;
}
static int test_tree_announce_decode_ancestry_count_mismatch(void) {
    make_announce(PATH(9), 1);
    size_t n = encode();
    wire_bytes[34] = 100;
    fips_reader r;
    fips_proto_error e;
    fips_reader_init(&r, wire_bytes + 1, n - 1);
    CHECK(fips_tree_announce_decode(&r, decoded, &e) == FIPS_ERR_RANGE &&
          e.variant == FIPS_PROTO_MessageTooShort &&
          e.as.message_too_short.expected == 35 + 3200 + 64);
    return 0;
}
static int test_tree_announce_encode_unsigned_fails(void) {
    make_announce(PATH(9), 1);
    announce->declaration.is_signed = false;
    fips_writer w;
    fips_proto_error e;
    fips_writer_init(&w, wire_bytes, sizeof(wire_bytes));
    CHECK(fips_tree_announce_encode(announce, &w, &e) == FIPS_ERR_RANGE &&
          e.variant == FIPS_PROTO_InvalidSignature && !w.pos);
    return 0;
}
static int test_tree_announce_validate_semantics_accepts_valid_non_root(void) {
    make_announce(PATH(9, 2, 1), 5);
    OK(fips_tree_announce_validate_semantics(announce, NULL));
    return 0;
}
static int semantic_error(fips_tree_error_variant v) {
    fips_tree_error e;
    CHECK(fips_tree_announce_validate_semantics(announce, &e) == FIPS_ERR_RANGE && e.variant == v);
    return 0;
}
static int test_tree_announce_validate_semantics_rejects_non_minimal_root(void) {
    make_announce(PATH(9, 0, 1), 5);
    return semantic_error(FIPS_TREE_AncestryRootNotMinimum);
}
static int test_tree_announce_validate_semantics_rejects_parent_mismatch(void) {
    make_announce(PATH(9, 3, 1), 5);
    announce->declaration.parent_id = A(2);
    return semantic_error(FIPS_TREE_AncestryParentMismatch);
}
static int test_tree_announce_validate_semantics_rejects_sender_mismatch(void) {
    make_announce(PATH(9, 2, 1), 5);
    announce->declaration.node_addr = A(8);
    return semantic_error(FIPS_TREE_AncestryNodeMismatch);
}
static int test_tree_announce_validate_semantics_rejects_root_with_ancestors(void) {
    make_announce(PATH(9, 0), 5);
    announce->declaration.parent_id = A(9);
    return semantic_error(FIPS_TREE_RootDeclarationMismatch);
}
/* acks.rs receiver report lineage tests. */
static fips_stp_link_evidence link_ev(uint64_t epoch, uint64_t next, bool has, uint64_t h,
                                      uint64_t received, uint32_t reordered) {
    return (fips_stp_link_evidence){epoch, next, has, {h, received, reordered}};
}
static void send_ack(uint64_t seq, uint64_t counter, fips_stp_link_evidence l, uint64_t ms) {
    if (fips_tree_state_record_announce(s, A(1), seq, counter, &l, at(ms)) != FIPS_OK)
        abort();
}
static bool check_ack(fips_stp_link_evidence l, uint64_t ms, fips_stp_resend_reason *reason) {
    return fips_tree_state_check_announce(s, A(1), &l, at(ms), reason);
}
static fips_stp_link_evidence lossy(uint64_t counter, bool recording) {
    return recording ? link_ev(1, counter + 2, true, counter, counter + 1, 0)
                     : link_ev(1, counter + 2, true, counter + 1, counter + 1, 0);
}
static int spend_budget(void) {
    init(0);
    send_ack(5, 12, lossy(11, true), 0);
    fips_stp_resend_reason reason;
    unsigned n = 0;
    for (uint64_t ms = 0; ms <= 6000; ms += 1000) {
        bool resend = check_ack(lossy(12, false), ms, &reason);
        CHECK(resend == (ms == 0 || ms == 1000 || ms == 3000));
        if (resend) {
            CHECK(reason == FIPS_STP_Loss);
            n++;
        }
    }
    uint64_t counter;
    CHECK(n == 3 && fips_tree_state_outstanding_counter(s, A(1), &counter) && counter == 12);
    return 0;
}
static int test_tree_ack_new_sequence_starts_a_new_lineage(void) {
    CHECK(!spend_budget());
    send_ack(6, 13, lossy(12, true), 10000);
    fips_stp_resend_reason reason;
    CHECK(check_ack(lossy(13, false), 11000, &reason) && reason == FIPS_STP_Loss);
    uint64_t seq;
    CHECK(fips_tree_state_outstanding_seq(s, A(1), &seq) && seq == 6);
    return 0;
}
static int test_tree_ack_periodic_resend_of_the_same_sequence_keeps_the_budget(void) {
    CHECK(!spend_budget());
    uint64_t counter = 13;
    send_ack(5, counter, lossy(12, true), 10000);
    unsigned count = 0;
    fips_stp_resend_reason reason;
    for (uint64_t ms = 11000; ms <= 120000; ms += 1000)
        if (check_ack(lossy(counter, false), ms, &reason)) {
            CHECK(ms == 40000 && reason == FIPS_STP_Timeout);
            count++;
            fips_stp_link_evidence ev = lossy(counter, true);
            counter = ev.next_counter - 1;
            send_ack(5, counter, ev, ms);
        }
    CHECK(count == 1);
    return 0;
}
static int test_tree_ack_removed_peer_starts_fresh(void) {
    init(0);
    send_ack(5, 12, link_ev(1, 13, true, 9, 10, 0), 0);
    send_ack(5, 3, link_ev(2, 4, false, 0, 0, 0), 1000);
    fips_stp_resend_reason reason;
    CHECK(check_ack(link_ev(2, 4, true, 3, 4, 0), 2000, &reason) && reason == FIPS_STP_Unverified);
    init(0);
    send_ack(5, 12, link_ev(1, 13, true, 9, 10, 0), 0);
    fips_tree_state_remove_peer(s, A(1));
    uint64_t n;
    CHECK(!fips_tree_state_outstanding_counter(s, A(1), &n));
    send_ack(5, 3, link_ev(2, 4, false, 0, 0, 0), 1000);
    CHECK(!check_ack(link_ev(2, 4, true, 3, 4, 0), 2000, &reason));
    CHECK(!fips_tree_state_outstanding_counter(s, A(1), &n));
    return 0;
}
static int test_tree_ack_in_window_reorder_on_the_zero_base_confirms(void) {
    init(0);
    send_ack(5, 3, link_ev(1, 4, false, 0, 0, 0), 0);
    fips_stp_resend_reason reason;
    CHECK(!check_ack(link_ev(1, 5, true, 4, 5, 2), 1000, &reason));
    uint64_t n;
    CHECK(!fips_tree_state_outstanding_counter(s, A(1), &n));
    return 0;
}
static int test_tree_ack_short_upper_bound_is_a_loss(void) {
    init(0);
    send_ack(5, 12, link_ev(1, 13, true, 9, 10, 0), 0);
    fips_stp_resend_reason reason;
    CHECK(check_ack(link_ev(1, 15, true, 14, 14, 0), 1000, &reason) && reason == FIPS_STP_Loss);
    uint64_t n;
    CHECK(fips_tree_state_outstanding_counter(s, A(1), &n));
    return 0;
}
static int test_tree_ack_fallback_resends_once_per_lineage(void) {
    init(0);
    uint64_t counter = 12;
    send_ack(5, counter, link_ev(1, 13, false, 0, 0, 0), 0);
    fips_stp_resend_reason reason;
    CHECK(!check_ack(link_ev(1, 13, false, 0, 0, 0), 29999, &reason));
    unsigned n = 0;
    for (uint64_t ms = 30000; ms <= 120000; ms += 1000)
        if (check_ack(link_ev(1, counter + 1, false, 0, 0, 0), ms, &reason)) {
            CHECK(ms == 30000 && reason == FIPS_STP_Timeout);
            n++;
            counter++;
            send_ack(5, counter, link_ev(1, counter + 1, false, 0, 0, 0), ms);
        }
    CHECK(n == 1);
    return 0;
}
/* Explicit 2026-11-12 handler regressions, exercising the real action driver. */
static size_t actions_of(const fips_stp_actions *a, fips_stp_action_kind kind) {
    size_t n = 0;
    for (size_t i = 0; i < a->len; i++)
        n += a->items[i].kind == kind;
    return n;
}
static int regression_loop_drop_root_refreshes_filters(void) {
    under_parent(PATH(4, 1), 0);
    put(3, 1, PATH(3, 5, 4, 1));
    make_announce(PATH(4, 5, 9, 1), 2);
    fips_stp_actions a;
    OK(fips_stp_receive_announce(s, A(4), announce, true, true, &ranking, 3000, at(1500), &a));
    CHECK(fips_tree_state_is_root(s) && actions_of(&a, FIPS_STP_BecameRoot) == 1 &&
          actions_of(&a, FIPS_STP_SendAnnounce) == 1);
    bool forced = false, invalidated = false;
    for (size_t i = 0; i < a.len; i++) {
        if (a.items[i].kind == FIPS_STP_RefreshFilters && a.items[i].all_peers &&
            !a.items[i].changed_only)
            forced = true;
        if (a.items[i].kind == FIPS_STP_BecameRoot && a.items[i].invalidate_other_roots &&
            a.items[i].invalidate_via_node)
            invalidated = true;
    }
    CHECK(forced && invalidated);
    return 0;
}
static int regression_loop_drop_reparents_under_veto(void) {
    under_parent(PATH(4, 1), 30);
    put(2, 1, PATH(2, 8, 7, 1));
    fips_tree_state_set_parent_hysteresis(s, 0.9);
    make_announce(PATH(4, 5, 9, 1), 2);
    fips_stp_actions a;
    OK(fips_stp_receive_announce(s, A(4), announce, true, true, &ranking, 3000, at(1500), &a));
    CHECK(!fips_tree_state_is_root(s) && same(s->my_declaration.parent_id, A(2)) &&
          actions_of(&a, FIPS_STP_ChangeParent) == 1 && !actions_of(&a, FIPS_STP_BecameRoot));
    return 0;
}
static int regression_announce_side_effects_are_gated(void) {
    init(1);
    put(4, 5, PATH(4));
    fips_stp_actions a;
    uint64_t seq = s->my_declaration.sequence;
    /* A stale declaration claims us as parent and a worse root. Neither the
     * child-membership refresh nor the better-root echo may escape the gate. */
    make_announce(PATH(4, 1), 5);
    OK(fips_stp_receive_announce(s, A(4), announce, true, true, &ranking, 2000, at(2000), &a));
    CHECK(a.len == 1 && a.items[0].kind == FIPS_STP_Drop && a.items[0].reason == FIPS_STP_Stale &&
          s->my_declaration.sequence == seq);
    make_announce(PATH(4), 6);
    OK(fips_stp_receive_announce(s, A(4), announce, false, true, &ranking, 2000, at(2000), &a));
    CHECK(a.len == 1 && a.items[0].reason == FIPS_STP_UnknownPeer);
    OK(fips_stp_receive_announce(s, A(3), announce, true, true, &ranking, 2000, at(2000), &a));
    CHECK(a.len == 1 && a.items[0].reason == FIPS_STP_SenderMismatch);
    OK(fips_stp_receive_announce(s, A(4), announce, true, false, &ranking, 2000, at(2000), &a));
    CHECK(a.len == 1 && a.items[0].reason == FIPS_STP_BadSignature);
    make_announce(PATH(4, 0, 1), 6);
    OK(fips_stp_receive_announce(s, A(4), announce, true, true, &ranking, 2000, at(2000), &a));
    CHECK(a.len == 1 && a.items[0].reason == FIPS_STP_BadAncestry);
    CHECK(fips_tree_state_peer(s, A(4))->declaration.sequence == 5 &&
          s->my_declaration.sequence == seq);
    return 0;
}
static int regression_accepted_child_flip_refreshes_before_no_change(void) {
    init(1);
    put(4, 1, PATH(4));
    make_announce(PATH(4, 1), 2);
    fips_stp_actions a;
    OK(fips_stp_receive_announce(s, A(4), announce, true, true, &ranking, 2000, at(2000), &a));
    CHECK(a.len == 1 && a.items[0].kind == FIPS_STP_RefreshFilters && a.items[0].changed_only &&
          a.items[0].all_peers);
    return 0;
}
static int regression_sequence_only_does_not_cascade(void) {
    under_parent(PATH(4, 1), 30);
    make_announce(PATH(4, 1), 2);
    fips_stp_actions a;
    OK(fips_stp_receive_announce(s, A(4), announce, true, true, &ranking, 2000, at(2000), &a));
    CHECK(a.len == 1 && a.items[0].kind == FIPS_STP_SignDeclaration &&
          s->my_declaration.sequence == 3);
    CHECK(s->flap.last_parent_switch == 1000);
    return 0;
}
static int regression_midchain_swap_cascades(void) {
    under_parent(PATH(4, 7, 1), 0);
    make_announce(PATH(4, 8, 1), 2);
    fips_stp_actions a;
    OK(fips_stp_receive_announce(s, A(4), announce, true, true, &ranking, 2000, at(2000), &a));
    CHECK(actions_of(&a, FIPS_STP_SendAnnounce) == 1 &&
          actions_of(&a, FIPS_STP_AncestryChanged) == 1 && s->my_coords.len == 4 &&
          same(s->my_coords.entries[2].node_addr, A(8)));
    CHECK(a.items[a.len - 1].kind == FIPS_STP_RefreshFilters && a.items[a.len - 1].changed_only);
    return 0;
}
static int periodic_actions_and_echo(void) {
    init(5);
    fips_stp_actions a;
    OK(fips_stp_periodic(s, &ranking, 9000, at(9000), &a));
    CHECK(a.len == 1 && a.items[0].kind == FIPS_STP_SendAnnounce &&
          s->my_declaration.sequence == 1 && s->my_declaration.timestamp == 1000);
    CHECK(fips_stp_should_echo(A(9), A(1)) && !fips_stp_should_echo(A(1), A(9)) &&
          !fips_stp_should_echo(A(1), A(1)));
    put(1, 1, PATH(1, 0));
    OK(fips_stp_periodic(s, &ranking, 9000, at(9000), &a));
    CHECK(same(s->my_declaration.parent_id, A(1)) && a.items[0].kind == FIPS_STP_SignDeclaration);
    OK(fips_stp_remove_peer(s, A(1), &ranking, 9001, at(9001), &a));
    CHECK(fips_tree_state_is_root(s) && actions_of(&a, FIPS_STP_BecameRoot) == 1);
    return 0;
}
static int storage_refusal_and_skip(void) {
    init(5);
    s->peer_capacity = 1;
    put(1, 1, PATH(1, 0));
    make_announce(PATH(2, 0), 1);
    fips_stp_actions a = {.len = 77};
    CHECK(fips_stp_receive_announce(s, A(2), announce, true, true, &ranking, 2000, at(2000), &a) ==
              FIPS_ERR_RANGE &&
          a.len == 77 && s->peer_count == 1 && s->my_declaration.sequence == 1);
    skips[0] = A(1);
    ranking.skip = skips;
    ranking.skip_count = 1;
    CHECK(target(-1));
    s->peer_capacity = 8;
    ranking.skip_count = 0;
    put(2, 1, PATH(2, 0));
    skips[0] = A(1);
    ranking.skip_count = 1;
    CHECK(target(2));
    place(5, PATH(5, 0));
    put(1, 1, PATH(1, 0));
    path(dest, PATH(0));
    ranking.skip = skips;
    ranking.skip_count = 1;
    fips_node_addr out;
    CHECK(!fips_tree_state_find_next_hop(s, dest, skips, 1, &out));
    return 0;
}
static int codec_capacity_atomicity_and_max_count(void) {
    make_announce(PATH(9, 2, 1), 5);
    size_t n = encode();
    uint8_t before[512];
    memcpy(before, wire_bytes, sizeof(before));
    for (size_t cap = 0; cap < n; cap++) {
        fips_writer w;
        fips_proto_error e;
        fips_writer_init(&w, wire_bytes, cap);
        CHECK(fips_tree_announce_encode(announce, &w, &e) == FIPS_ERR_RANGE && !w.pos &&
              e.variant == FIPS_PROTO_MessageTooLong && e.as.message_too_long.got == n &&
              !memcmp(wire_bytes, before, sizeof(before)));
    }
    fips_reader r;
    fips_reader_init(&r, wire_bytes + 1, n - 1);
    OK(fips_tree_announce_decode(&r, decoded, NULL));
    fips_parent_declaration old = decoded->declaration;
    uint16_t len = decoded->ancestry.len;
    for (size_t i = 0; i < n - 1; i++) {
        fips_reader_init(&r, wire_bytes + 1, i);
        CHECK(fips_tree_announce_decode(&r, decoded, NULL) == FIPS_ERR_RANGE && !r.pos &&
              decoded->ancestry.len == len &&
              fips_parent_declaration_equal(&old, &decoded->declaration));
    }
    /* Trailing bytes are not consumed. Count 0 is Malformed, not BadCoord. */
    wire_bytes[n] = 0xab;
    fips_reader_init(&r, wire_bytes + 1, n);
    OK(fips_tree_announce_decode(&r, decoded, NULL));
    CHECK(r.pos == n - 1);
    wire_bytes[34] = wire_bytes[35] = 0;
    fips_reader_init(&r, wire_bytes + 1, n - 1);
    fips_proto_error e;
    CHECK(fips_tree_announce_decode(&r, decoded, &e) == FIPS_ERR_RANGE &&
          e.variant == FIPS_PROTO_Malformed &&
          !strcmp(e.as.malformed, "ancestry must have at least one entry"));
    /* Entire u16 wire domain, no arbitrary max-depth cutoff in the codec. */
    announce->ancestry.len = UINT16_MAX;
    for (size_t i = 0; i < UINT16_MAX; i++)
        announce->ancestry.entries[i] = fips_coord_entry_new(A(9), i, i + 1);
    n = fips_tree_announce_encoded_len(announce);
    uint8_t *large = malloc(n);
    CHECK(large);
    fips_writer w;
    fips_writer_init(&w, large, n);
    OK(fips_tree_announce_encode(announce, &w, NULL));
    fips_reader_init(&r, large + 1, n - 1);
    OK(fips_tree_announce_decode(&r, decoded, NULL));
    CHECK(decoded->ancestry.len == UINT16_MAX && r.pos == n - 1 &&
          decoded->ancestry.entries[UINT16_MAX - 1].sequence == UINT16_MAX - 1);
    free(large);
    init(10);
    bool updated;
    OK(fips_tree_state_update_peer(s, &announce->declaration, &announce->ancestry, &updated));
    fips_stp_actions a = {.len = 77};
    CHECK(fips_stp_periodic(s, &ranking, 2000, at(2000), &a) == FIPS_ERR_RANGE && a.len == 77 &&
          s->my_declaration.sequence == 1);
    return 0;
}
static int declaration_codec_atomicity(void) {
    fips_parent_declaration d = fips_parent_declaration_new(A(1), A(2), 3, 4), out = d;
    uint8_t b[64];
    memset(b, 0xcc, sizeof(b));
    for (size_t n = 0; n < 48; n++) {
        fips_writer w;
        fips_reader r;
        fips_proto_error e;
        fips_writer_init(&w, b, n);
        CHECK(fips_parent_declaration_signing_bytes(&d, &w, &e) == FIPS_ERR_RANGE && !w.pos &&
              b[0] == 0xcc && e.as.message_too_long.got == 48);
        fips_reader_init(&r, b, n);
        CHECK(fips_parent_declaration_decode(&r, &out, &e) == FIPS_ERR_RANGE && !r.pos &&
              fips_parent_declaration_equal(&d, &out) && e.as.message_too_short.expected == 48);
    }
    return 0;
}

static int regression_echo_signing_order(void) {
    under_parent(PATH(4, 1), 0);
    make_announce(PATH(4), 2);
    fips_stp_actions a;
    OK(fips_stp_receive_announce(s, A(4), announce, true, true, &ranking, 2000, at(2000), &a));
    bool signed_first = false;
    size_t sends = 0;
    for (size_t i = 0; i < a.len; i++) {
        if (a.items[i].kind == FIPS_STP_SignDeclaration)
            signed_first = true;
        if (a.items[i].kind == FIPS_STP_SendAnnounce) {
            CHECK(signed_first);
            sends++;
        }
    }
    CHECK(sends == 1);
    return 0;
}
static int sequence_exhaustion_is_atomic(void) {
    two_peers();
    parent(1, UINT64_MAX, 1000);
    fips_stp_actions a = {.len = 77};
    make_announce(PATH(2, 0), 2);
    uint64_t peer_seq = fips_tree_state_peer(s, A(2))->declaration.sequence;
    CHECK(fips_stp_receive_announce(s, A(2), announce, true, true, &ranking, 2000, at(2000), &a) ==
              FIPS_ERR_RANGE &&
          a.len == 77);
    CHECK(fips_tree_state_peer(s, A(2))->declaration.sequence == peer_seq &&
          s->my_declaration.sequence == UINT64_MAX);
    OK(fips_stp_periodic(s, &ranking, 2000, at(2000), &a));
    CHECK(a.len == 1 && a.items[0].kind == FIPS_STP_SendAnnounce);
    a.len = 77;
    cost(1, 10);
    cost(2, 1);
    CHECK(fips_stp_periodic(s, &ranking, 2000, at(2000), &a) == FIPS_ERR_RANGE && a.len == 77);
    CHECK(fips_stp_remove_peer(s, A(1), &ranking, 2000, at(2000), &a) == FIPS_ERR_RANGE &&
          s->peer_count == 2 && a.len == 77);
    fips_parent_loss loss = {false, true};
    CHECK(fips_tree_state_recover(s, &ranking, 2000, at(2000), &loss) == FIPS_ERR_RANGE &&
          !loss.changed && loss.dampened);
    CHECK(fips_tree_state_become_root(s, 2000) == FIPS_ERR_RANGE && !fips_tree_state_is_root(s));
    return 0;
}

int main(void) {
    s = malloc(sizeof(*s));
    storage = calloc(8, sizeof(*storage));
    coords = malloc(sizeof(*coords));
    dest = malloc(sizeof(*dest));
    announce = malloc(sizeof(*announce));
    decoded = malloc(sizeof(*decoded));
    if (!s || !storage || !coords || !dest || !announce || !decoded)
        return 1;
    const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"test_parent_declaration_new", test_parent_declaration_new},
        {"test_parent_declaration_self_root", test_parent_declaration_self_root},
        {"test_parent_declaration_freshness", test_parent_declaration_freshness},
        {"test_parent_declaration_signing_bytes", test_parent_declaration_signing_bytes},
        {"test_parent_declaration_equality", test_parent_declaration_equality},
        {"test_tree_state_new", test_tree_state_new},
        {"test_tree_state_update_peer", test_tree_state_update_peer},
        {"test_tree_state_remove_peer", test_tree_state_remove_peer},
        {"test_tree_state_distance_to_peer", test_tree_state_distance_to_peer},
        {"test_tree_state_peer_ids", test_tree_state_peer_ids},
        {"test_evaluate_parent_picks_smallest_root", test_evaluate_parent_picks_smallest_root},
        {"test_evaluate_parent_prefers_shallowest_depth",
         test_evaluate_parent_prefers_shallowest_depth},
        {"test_evaluate_parent_stays_root_when_smallest",
         test_evaluate_parent_stays_root_when_smallest},
        {"test_evaluate_parent_no_switch_when_already_best",
         test_evaluate_parent_no_switch_when_already_best},
        {"test_evaluate_parent_no_peers", test_evaluate_parent_no_peers},
        {"test_evaluate_parent_depth_threshold", test_evaluate_parent_depth_threshold},
        {"test_evaluate_parent_rejects_loop_candidate",
         test_evaluate_parent_rejects_loop_candidate},
        {"test_evaluate_parent_picks_loop_free_over_loopy",
         test_evaluate_parent_picks_loop_free_over_loopy},
        {"test_handle_parent_lost_finds_alternative", test_handle_parent_lost_finds_alternative},
        {"test_handle_parent_lost_becomes_root_when_self_smaller_than_remaining",
         test_handle_parent_lost_becomes_root_when_self_smaller_than_remaining},
        {"test_recompute_coords_demotes_when_self_smaller_than_parent_root",
         test_recompute_coords_demotes_when_self_smaller_than_parent_root},
        {"test_handle_parent_lost_becomes_root", test_handle_parent_lost_becomes_root},
        {"test_find_next_hop_chain", test_find_next_hop_chain},
        {"test_find_next_hop_chain_indirect", test_find_next_hop_chain_indirect},
        {"test_find_next_hop_toward_root", test_find_next_hop_toward_root},
        {"test_find_next_hop_sibling", test_find_next_hop_sibling},
        {"test_find_next_hop_tie_breaking", test_find_next_hop_tie_breaking},
        {"test_find_next_hop_different_root", test_find_next_hop_different_root},
        {"test_find_next_hop_no_peers", test_find_next_hop_no_peers},
        {"test_find_next_hop_local_minimum", test_find_next_hop_local_minimum},
        {"test_find_next_hop_best_of_multiple", test_find_next_hop_best_of_multiple},
        {"test_effective_depth_selects_lower_cost_deeper_peer",
         test_effective_depth_selects_lower_cost_deeper_peer},
        {"test_effective_depth_equal_cost_degenerates_to_depth",
         test_effective_depth_equal_cost_degenerates_to_depth},
        {"test_effective_depth_tiebreak_by_node_addr", test_effective_depth_tiebreak_by_node_addr},
        {"test_hysteresis_prevents_marginal_switch", test_hysteresis_prevents_marginal_switch},
        {"test_hysteresis_allows_significant_switch", test_hysteresis_allows_significant_switch},
        {"test_cold_start_default_cost", test_cold_start_default_cost},
        {"test_hold_down_suppresses_reeval", test_hold_down_suppresses_reeval},
        {"test_mandatory_switch_bypasses_hold_down", test_mandatory_switch_bypasses_hold_down},
        {"test_heterogeneous_7node_avoids_bottleneck", test_heterogeneous_7node_avoids_bottleneck},
        {"test_cost_degradation_triggers_switch", test_cost_degradation_triggers_switch},
        {"test_cost_improvement_within_hysteresis_no_switch",
         test_cost_improvement_within_hysteresis_no_switch},
        {"test_single_peer_no_reeval_benefit", test_single_peer_no_reeval_benefit},
        {"an_unmeasured_peer_on_a_smaller_root_is_adopted_when_no_measured_peer_reaches_it",
         an_unmeasured_peer_on_a_smaller_root_is_adopted_when_no_measured_peer_reaches_it},
        {"parent_loss_takes_the_unmeasured_peer_on_a_smaller_root_rather_than_self_rooting",
         parent_loss_takes_the_unmeasured_peer_on_a_smaller_root_rather_than_self_rooting},
        {"a_measured_peer_on_the_smallest_root_still_beats_a_shallower_unmeasured_one",
         a_measured_peer_on_the_smallest_root_still_beats_a_shallower_unmeasured_one},
        {"an_unmeasured_candidate_never_wins_a_cost_based_switch",
         an_unmeasured_candidate_never_wins_a_cost_based_switch},
        {"parent_moving_to_worse_root_switches_to_peer_still_on_better_root",
         parent_moving_to_worse_root_switches_to_peer_still_on_better_root},
        {"parent_moving_to_worse_root_switches_even_while_hold_down_is_active",
         parent_moving_to_worse_root_switches_even_while_hold_down_is_active},
        {"parent_ancestry_change_with_no_better_root_yields_ancestry_update",
         parent_ancestry_change_with_no_better_root_yields_ancestry_update},
        {"parent_moving_to_worse_root_with_no_better_root_visible_yields_ancestry_update",
         parent_moving_to_worse_root_with_no_better_root_visible_yields_ancestry_update},
        {"parent_moving_to_better_root_yields_ancestry_update",
         parent_moving_to_better_root_yields_ancestry_update},
        {"parent_moving_to_worse_root_does_not_switch_to_sibling_reached_through_parent",
         parent_moving_to_worse_root_does_not_switch_to_sibling_reached_through_parent},
        {"parent_moving_to_worse_root_follows_parent_when_best_candidate_is_reached_through_parent",
         parent_moving_to_worse_root_follows_parent_when_best_candidate_is_reached_through_parent},
        {"parent_ancestry_change_on_same_root_leaves_better_peer_to_the_hold_down_veto",
         parent_ancestry_change_on_same_root_leaves_better_peer_to_the_hold_down_veto},
        {"loop_drop_takes_a_loop_free_candidate_on_the_same_root_while_hold_down_is_active",
         loop_drop_takes_a_loop_free_candidate_on_the_same_root_while_hold_down_is_active},
        {"loop_drop_takes_a_loop_free_candidate_that_does_not_clear_the_hysteresis_margin",
         loop_drop_takes_a_loop_free_candidate_that_does_not_clear_the_hysteresis_margin},
        {"loop_drop_with_only_candidates_reached_through_this_node_still_becomes_root",
         loop_drop_with_only_candidates_reached_through_this_node_still_becomes_root},
        {"test_flap_dampening_engages_after_threshold",
         test_flap_dampening_engages_after_threshold},
        {"test_flap_dampening_allows_mandatory_switches",
         test_flap_dampening_allows_mandatory_switches},
        {"test_flap_dampening_expires", test_flap_dampening_expires},
        {"test_flap_dampening_below_threshold", test_flap_dampening_below_threshold},
        {"test_flap_dampening_window_reset", test_flap_dampening_window_reset},
        {"test_flap_dampening_same_parent_no_count", test_flap_dampening_same_parent_no_count},
        {"test_flap_dampening_engages_a_second_time_after_first_episode_lapses",
         test_flap_dampening_engages_a_second_time_after_first_episode_lapses},
        {"test_flap_dampening_duration_at_u64_max_does_not_panic",
         test_flap_dampening_duration_at_u64_max_does_not_panic},
        {"test_parent_loss_recovery_reports_the_engagement_that_arms_dampening",
         test_parent_loss_recovery_reports_the_engagement_that_arms_dampening},
        {"test_parent_loss_recovery_to_self_root_reports_no_engagement",
         test_parent_loss_recovery_to_self_root_reports_no_engagement},
        {"test_handle_parent_lost_keeps_its_published_bool_return",
         test_handle_parent_lost_keeps_its_published_bool_return},
        {"test_flap_dampening_counter_resets_when_the_episode_lapses_not_when_it_engages",
         test_flap_dampening_counter_resets_when_the_episode_lapses_not_when_it_engages},
        {"test_hold_down_runs_from_the_last_parent_change_not_from_each_ancestry_update",
         test_hold_down_runs_from_the_last_parent_change_not_from_each_ancestry_update},
        {"test_tree_announce", test_tree_announce},
        {"test_tree_announce_encode_decode_root", test_tree_announce_encode_decode_root},
        {"test_tree_announce_encode_decode_depth3", test_tree_announce_encode_decode_depth3},
        {"test_tree_announce_decode_unsupported_version",
         test_tree_announce_decode_unsupported_version},
        {"test_tree_announce_decode_truncated", test_tree_announce_decode_truncated},
        {"test_tree_announce_decode_ancestry_count_mismatch",
         test_tree_announce_decode_ancestry_count_mismatch},
        {"test_tree_announce_encode_unsigned_fails", test_tree_announce_encode_unsigned_fails},
        {"test_tree_announce_validate_semantics_accepts_valid_non_root",
         test_tree_announce_validate_semantics_accepts_valid_non_root},
        {"test_tree_announce_validate_semantics_rejects_non_minimal_root",
         test_tree_announce_validate_semantics_rejects_non_minimal_root},
        {"test_tree_announce_validate_semantics_rejects_parent_mismatch",
         test_tree_announce_validate_semantics_rejects_parent_mismatch},
        {"test_tree_announce_validate_semantics_rejects_sender_mismatch",
         test_tree_announce_validate_semantics_rejects_sender_mismatch},
        {"test_tree_announce_validate_semantics_rejects_root_with_ancestors",
         test_tree_announce_validate_semantics_rejects_root_with_ancestors},
        {"test_tree_ack_new_sequence_starts_a_new_lineage",
         test_tree_ack_new_sequence_starts_a_new_lineage},
        {"test_tree_ack_periodic_resend_of_the_same_sequence_keeps_the_budget",
         test_tree_ack_periodic_resend_of_the_same_sequence_keeps_the_budget},
        {"test_tree_ack_removed_peer_starts_fresh", test_tree_ack_removed_peer_starts_fresh},
        {"test_tree_ack_in_window_reorder_on_the_zero_base_confirms",
         test_tree_ack_in_window_reorder_on_the_zero_base_confirms},
        {"test_tree_ack_short_upper_bound_is_a_loss", test_tree_ack_short_upper_bound_is_a_loss},
        {"test_tree_ack_fallback_resends_once_per_lineage",
         test_tree_ack_fallback_resends_once_per_lineage},
        {"regression_loop_drop_root_refreshes_filters",
         regression_loop_drop_root_refreshes_filters},
        {"regression_loop_drop_reparents_under_veto", regression_loop_drop_reparents_under_veto},
        {"regression_announce_side_effects_are_gated", regression_announce_side_effects_are_gated},
        {"regression_accepted_child_flip_refreshes_before_no_change",
         regression_accepted_child_flip_refreshes_before_no_change},
        {"regression_sequence_only_does_not_cascade", regression_sequence_only_does_not_cascade},
        {"regression_midchain_swap_cascades", regression_midchain_swap_cascades},
        {"periodic_actions_and_echo", periodic_actions_and_echo},
        {"storage_refusal_and_skip", storage_refusal_and_skip},
        {"codec_capacity_atomicity_and_max_count", codec_capacity_atomicity_and_max_count},
        {"declaration_codec_atomicity", declaration_codec_atomicity},
        {"regression_echo_signing_order", regression_echo_signing_order},
        {"sequence_exhaustion_is_atomic", sequence_exhaustion_is_atomic},
    };
    size_t failed = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (tests[i].run()) {
            failed++;
            fprintf(stderr, "FAIL %s\n", tests[i].name);
        } else
            printf("PASS %s\n", tests[i].name);
    }
    free(decoded);
    free(announce);
    free(dest);
    free(coords);
    free(storage);
    free(s);
    printf("STP: 89 upstream tests + 12 C/driver regressions, %zu failed\n", failed);
    return failed ? 1 : 0;
}
