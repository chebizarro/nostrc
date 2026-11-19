/* Crossing dials and link loss over UDP (nostrc-8rtmi). Two in-process nodes
 * on real UDP sockets; a bound socket nobody reads stands in for an answer
 * that never comes, so the crossing is deterministic: one node's handshake leg
 * to its peer stays pending (legs are keyed by the peer's identity) while the
 * peer's own dial promotes the link the other way, at a different address per
 * direction (BLE's shared-address crossing is test_ble_two_nodes.c).
 * Whichever side of the IK tie-break holds it, the leg stays beside the link
 * while the link carries authenticated traffic (as upstream, only the leg's
 * own msg2 or its handshake timeout settles it), and the link's loss ends it
 * at once, so that node's redial recovers well inside the handshake timeout
 * instead of being swallowed by the stale leg. The winner's leg is lost to a
 * link the loser ends; the loser's to one the winner ends. Every wait is a
 * notice or a published snapshot; nothing sleeps. */
#include "node_test.h"
#include "transport/transport.h"
#include "node/handlers/internal.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *yaml_a =
    "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}, control: {enabled: false}}";
static const char *yaml_b =
    "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}, control: {enabled: false}}";

/* A loopback UDP address that receives and never answers. */
static int black_hole(fipsd_transport *t, fips_transport_addr *out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0); assert(fd >= 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof sa;
    assert(bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 && getsockname(fd, (struct sockaddr *)&sa, &len) == 0);
    char text[32]; snprintf(text, sizeof text, "127.0.0.1:%u", (unsigned)ntohs(sa.sin_port));
    assert(fipsd_transport_parse_address(t, text, out) == FIPS_OK);
    return fd;
}
static void links_up(test_node *a, test_node *b)
{
    fipsd_node_notice *e = nt_await(a, FIPSD_NOTICE_LINK_UP, &b->identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(b, FIPSD_NOTICE_LINK_UP, &a->identity.node_addr, NT_WAIT_MS); assert(e); free(e);
}
static void links_down(test_node *a, test_node *b)
{
    fipsd_node_notice *e = nt_await(a, FIPSD_NOTICE_LINK_DOWN, &b->identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(b, FIPSD_NOTICE_LINK_DOWN, &a->identity.node_addr, NT_WAIT_MS); assert(e); free(e);
}
static size_t legs_of(test_node *n)
{
    assert(nt_barrier(n, nt_ms() + NT_WAIT_MS));
    GoRefPtr ref = nt_snapshot(n); assert(ref.ptr);
    size_t legs = ((const fipsd_control_snapshot *)ref.ptr)->connections_count;
    go_refptr_release(&ref);
    return legs;
}
static void connect_to(test_node *from, test_node *to, const fips_transport_addr *at)
{ assert(fipsd_node_handlers_connect(from->h, &to->identity, (fips_transport_id){1}, at) == 0); }
int main(void)
{
    (void)fipsd_test_tmpdir(NULL);
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64;
    hcfg.notice_capacity = 256; hcfg.tick_ms = 200; hcfg.tree_announce_min_interval_ms = 100; hcfg.bloom_debounce_ms = 100;
    hcfg.link_dead_timeout_ms = 30000;
    const uint64_t budget = hcfg.timing.handshake_timeout_ms / 4;
    test_node a, b;
    assert(nt_init(&a, NULL, "A", NULL, yaml_a, &hcfg, 1) && nt_init(&b, NULL, "B", NULL, yaml_b, &hcfg, 1));
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0);
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)) == 0);
    nt_watch_publications();
    assert(fipsd_runtime_start(rt) == 0);
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        void *value = NULL;
        GoSelectCase bound = {GO_SELECT_RECEIVE, nt_socket_bound(n->transport), NULL, &value};
        GoSelectResult result = go_select_timeout(&bound, 1, NT_WAIT_MS);
        assert(result.selected_case == 0 && value);
        assert(nt_socket_address(n->transport, &n->udp_addr));
    }
    nt_identity(&a); nt_identity(&b);
    fips_transport_addr hole;
    int hole_fd = black_hole(a.transport, &hole);
    /* IK tie-break: the smaller node address's outbound wins. */
    bool a_wins = fips_fmp_cross_connection_winner(&a.identity.node_addr, &b.identity.node_addr, true);
    test_node *winner = a_wins ? &a : &b, *loser = a_wins ? &b : &a;
    size_t one = 1, none = 0;

    for (int round = 0; round < 2; ++round) {
        /* Round 0: the winner's leg is unanswered; round 1: the loser's. */
        test_node *holder = round ? loser : winner, *other = round ? winner : loser;
        connect_to(holder, other, &hole);
        assert(nt_await_converged(holder, nt_leg_count, &one, nt_ms() + NT_WAIT_MS));
        connect_to(other, holder, holder->addr);
        links_up(&a, &b);
        for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
            nt_pair_state want = {.root = winner, .is_root = n == winner}; /* the smaller address is root too */
            assert(nt_await_converged(n, nt_pair_converged, &want, nt_ms() + NT_WAIT_MS));
        }
        /* Authenticated frames have crossed the link; the leg is still held. */
        assert(legs_of(holder) == 1 && legs_of(other) == 0);
        /* The peer's authenticated Disconnect ends the link and the leg with it. */
        uint64_t lost = nt_ms();
        assert(fipsd_node_handlers_disconnect(other->h, &holder->identity.node_addr) == 0);
        links_down(&a, &b);
        assert(nt_await_converged(holder, nt_leg_count, &none, lost + budget));
        connect_to(holder, other, other->addr);
        links_up(&a, &b);
        uint64_t recovered = nt_ms() - lost;
        printf("udp crossing: %s's leg (tie-break %s) ended by the link loss; redial recovered in %llu ms (handshake timeout %llu ms)\n",
               holder->name, holder == winner ? "winner" : "loser", (unsigned long long)recovered,
               (unsigned long long)hcfg.timing.handshake_timeout_ms);
        assert(recovered < budget);
        assert(fipsd_node_handlers_disconnect(holder->h, &other->identity.node_addr) == 0);
        links_down(&a, &b);
    }

    fipsd_runtime_shutdown(rt);
    assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    nt_drain(&a); nt_drain(&b);
    nt_print_trace("crossing dials and link loss over UDP");
    nt_unwatch_publications();
    fipsd_runtime_destroy(rt);
    nt_destroy(&a); nt_destroy(&b);
    close(hole_fd);
    puts("udp crossing dials: a winner's and a loser's leg each kept beside the link, ended by its loss, prompt redial: ok");
    return 0;
}
