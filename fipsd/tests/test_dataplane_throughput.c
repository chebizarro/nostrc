#ifdef NDEBUG
#undef NDEBUG /* Test operations and correctness assertions run in optimized smoke builds too. */
#endif
/* Throughput smoke, not a performance qualification. Bounded batches keep the
 * application consumer in lockstep with notices (no sleeps or packet retries). */
#include "node_test.h"
int main(int argc, char **argv)
{
    size_t n = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 10000;
    assert(n > 0 && n <= 10000000);
    fipsd_runtime_config cfg; fipsd_runtime_config_default(&cfg); cfg.workers = 4; cfg.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&cfg); assert(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); assert(net);
    fipsd_node_handlers_config hc; fipsd_node_handlers_config_default(&hc);
    hc.peer_capacity = hc.tree_peer_capacity = hc.session_capacity = 4; hc.cache_entries = 64; hc.notice_capacity = 256;
    test_node a, b;
    const char *ya = "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}}";
    const char *yb = "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}}";
    assert(nt_init(&a, net, "A", "loop:a", ya, &hc, 1));
    assert(nt_init(&b, net, "B", "loop:b", yb, &hc, 1));
    assert(!fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)));
    assert(!fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)));
    assert(!fipsd_runtime_start(rt)); nt_identity(&a); nt_identity(&b);
    assert(!fipsd_node_handlers_connect(a.h, &b.identity, (fips_transport_id){1}, b.addr));
    fipsd_node_notice *e = nt_await(&a, FIPSD_NOTICE_LINK_UP, NULL, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_UP, NULL, NT_WAIT_MS); assert(e); free(e);
    /* Wait for the tree before opening XK, not a clock-dependent startup delay. */
    bool smaller = memcmp(a.identity.node_addr.bytes, b.identity.node_addr.bytes, 16) < 0;
    e = nt_await(smaller ? &b : &a, FIPSD_NOTICE_TREE_CHANGED, NULL, NT_WAIT_MS); assert(e); free(e);
    uint8_t payload[512] = {0};
    assert(!fipsd_node_handlers_send(a.h, &b.identity.node_addr, 1000, 2000, payload, sizeof payload));
    e = nt_await(&b, FIPSD_NOTICE_DATAGRAM, NULL, NT_WAIT_MS); assert(e); free(e);
    uint64_t start = nt_ms();
    size_t received = 0;
    while (received < n) {
        size_t batch = n - received < 16 ? n - received : 16;
        for (size_t i = 0; i < batch; ++i) {
            uint64_t seq = received + i; memcpy(payload, &seq, sizeof seq);
            assert(!fipsd_node_handlers_send(a.h, &b.identity.node_addr, 1000, 2000, payload, sizeof payload));
        }
        bool seen[16] = {false};
        for (size_t i = 0; i < batch; ++i) {
            e = nt_await(&b, FIPSD_NOTICE_DATAGRAM, NULL, NT_WAIT_MS); assert(e);
            assert(e->len == sizeof payload); uint64_t seq; memcpy(&seq, e->payload, sizeof seq);
            assert(seq >= received && seq < received + batch && !seen[seq - received]);
            seen[seq - received] = true; free(e);
        }
        received += batch; nt_drain(&a);
    }
    uint64_t elapsed = nt_ms() - start;
    double seconds = elapsed / 1000.0;
    printf("loopback dataplane: N=%zu payload=512 scheduler_workers=4 crypto_workers=2/node elapsed_ms=%llu pps=%.0f MB/s=%.3f\n",
        n, (unsigned long long)elapsed, n / seconds, n * 512.0 / seconds / 1000000.0);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    nt_drain(&a); nt_drain(&b);
    /* Exercise shared query/trace helpers and keep this harness Werror-clean. */
    json_t *status = nt_query(&a, "show_status"); assert(status); json_decref(status);
    nt_print_trace("throughput startup");
    fipsd_runtime_destroy(rt); nt_destroy(&a); nt_destroy(&b); fipsd_loopback_network_destroy(net);
    return 0;
}
