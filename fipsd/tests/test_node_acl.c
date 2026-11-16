/* Real admission gates and SIGHUP config-snapshot publication, two workers.
 * Rejection/link/session/rekey notices and the config log are the barriers;
 * there are no sleeps or source-state reads while dispatchers are running. */
#define _POSIX_C_SOURCE 200809L
#include "node_test.h"
#include "node/acl.h"
#include "test_tmpdir.h"
#include <stdatomic.h>
static GoChannel *reloaded;
static _Atomic uint64_t msg1_from_b, msg2_from_b;
static const fips_transport_addr *b_addr;
static bool observe(void *user, const fips_transport_addr *from, const fips_transport_addr *to, const uint8_t *wire, size_t len)
{
    (void)user; (void)to;
    if (len && fips_transport_addr_equal(from, b_addr)) {
        if (wire[0] == 1) atomic_fetch_add(&msg1_from_b, 1);
        if (wire[0] == 2) atomic_fetch_add(&msg2_from_b, 1);
    }
    return true;
}
static int log_line(void *user, const char *line, size_t len)
{
    (void)user; (void)fwrite(line, 1, len, stderr);
    if (strstr(line, "configuration reload complete")) assert(go_channel_try_send(reloaded, (void *)1) == 0);
    return 0;
}
static void put(const char *path, const char *s)
{ FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(s, 1, strlen(s), f) == strlen(s)); assert(fclose(f) == 0); }
static json_t *data(json_t *r) { assert(r); json_t *d = json_object_get(r, "data"); assert(d); return d; }
static void no_link(test_node *n)
{ json_t *r = nt_query(n, "show_peers"); assert(json_array_size(json_object_get(data(r), "peers")) == 0); json_decref(r); }
void test_node_acl_admission(void);
int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) { test_node_acl_admission(); return 0; }
    const char *tmp = fipsd_test_tmpdir(NULL);
    char allow[4096], deny[4096], hosts[4096], config[4096];
    snprintf(allow, sizeof allow, "%s/peers.allow", tmp); snprintf(deny, sizeof deny, "%s/peers.deny", tmp);
    snprintf(hosts, sizeof hosts, "%s/hosts", tmp); snprintf(config, sizeof config, "%s/b.yaml", tmp);
    const char *ya = "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111'}, control: {enabled: false}}";
    const char *yb = "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222'}, control: {enabled: false}}";
    put(deny, "ALL\n"); put(config, yb);
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); assert(net);
    fipsd_node_handlers_config hc; fipsd_node_handlers_config_default(&hc);
    hc.peer_capacity = hc.tree_peer_capacity = hc.session_capacity = 4; hc.cache_entries = 32;
    hc.notice_capacity = 256; hc.tick_ms = 200; hc.timing.handshake_timeout_ms = 1000;
    fipsd_config *base = fipsd_config_parse(ya, strlen(ya), NULL); assert(base);
    fipsd_config_core_params(base, &hc.core); hc.has_core = true; fipsd_config_free(base);
    hc.core.session_rekey.after = (fips_duration){3, 0}; hc.core.session_rekey.jitter = 1;
    hc.core.session_rekey.link_age = hc.core.session_rekey.session_age = (fips_duration){0, 0};
    hc.core.session_rekey.floor_link = hc.core.session_rekey.floor_session = (fips_duration){0, 0};
    test_node a, b; assert(nt_init(&a, net, "A", "loop:a", ya, &hc, 1)); assert(nt_init(&b, net, "B", "loop:b", yb, &hc, 1));
    b_addr = b.addr; fipsd_loopback_network_set_filter(net, observe, NULL);
    fipsd_acl_paths paths = {allow, deny, hosts}; assert(fipsd_config_enable_acl(b.config, &paths));
    reloaded = go_channel_create(4); assert(reloaded);
    fipsd_log_sink sink = {.write = log_line}; fipsd_log *logger = fipsd_log_create(FIPSD_LOG_INFO, &sink); assert(logger);
    fipsd_log_set_default(logger);
    fipsd_config_service service; fipsd_config_service_init(&service, b.configs, config, logger, NULL);
    assert(fipsd_runtime_register(rt, &service.subsystem) == 0);
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0);
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)) == 0);
    assert(fipsd_runtime_start(rt) == 0); nt_identity(&a); nt_identity(&b);
    /* B's dial gate refuses before any msg1/DH. */
    assert(fipsd_node_handlers_connect(b.h, &a.identity, (fips_transport_id){1}, a.addr) == 0);
    fipsd_node_notice *e = nt_await(&b, FIPSD_NOTICE_HANDSHAKE_FAILED, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    assert(atomic_load(&msg1_from_b) == 0); no_link(&a); no_link(&b);
    /* A can dial, but B's authenticated msg1 admission refuses before msg2. */
    assert(fipsd_node_handlers_connect(a.h, &b.identity, (fips_transport_id){1}, b.addr) == 0);
    e = nt_await(&b, FIPSD_NOTICE_HANDSHAKE_FAILED, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&a, FIPSD_NOTICE_HANDSHAKE_FAILED, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    assert(atomic_load(&msg2_from_b) == 0); no_link(&a); no_link(&b);
    json_t *r = nt_query(&b, "show_acl"); assert(json_is_true(json_object_get(data(r), "deny_all")));
    assert(!strcmp(json_string_value(json_object_get(data(r), "effective_mode")), "deny_all")); json_decref(r);
    r = nt_query(&b, "show_metrics");
    assert(json_integer_value(json_object_get(json_object_get(data(r), "handshake"), "bad_state")) >= 2); json_decref(r);
    puts("ACL denied trace: outbound msg1=0; inbound msg2=0; no links; show_acl=deny_all; handshake.bad_state>=2");
    /* SIGHUP and automatic maintenance share one immutable config writer. */
    char npub[128]; assert(fips_peer_identity_npub(&a.identity, npub, sizeof npub) == FIPS_IDENTITY_OK); put(allow, npub);
    assert(go_channel_try_send(fipsd_runtime_reload_channel(rt), NULL) == 0);
    void *token = NULL; GoSelectCase cases[] = {{GO_SELECT_RECEIVE, reloaded, NULL, &token}};
    GoSelectResult done = go_select_timeout(cases, 1, NT_WAIT_MS); assert(done.selected_case == 0 && token);
    assert(fipsd_node_handlers_connect(a.h, &b.identity, (fips_transport_id){1}, b.addr) == 0);
    e = nt_await(&a, FIPSD_NOTICE_LINK_UP, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_LINK_UP, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    r = nt_query(&b, "show_acl"); assert(json_array_size(json_object_get(data(r), "allow_entries")) == 1);
    assert(!strcmp(json_string_value(json_object_get(data(r), "effective_mode")), "allow_then_deny_all")); json_decref(r);
    /* Wait for tree coordinates, then an actual application session/datagram. */
    e = nt_await(&a, FIPSD_NOTICE_FILTER_ACCEPTED, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_FILTER_ACCEPTED, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    static const uint8_t message[] = "ACL admitted data";
    assert(fipsd_node_handlers_send(a.h, &b.identity.node_addr, 111, 222, message, sizeof message) == 0);
    e = nt_await(&b, FIPSD_NOTICE_DATAGRAM, &a.identity.node_addr, NT_WAIT_MS); assert(e && e->len == sizeof message); free(e);
    /* The scheduled rekey is a real event after multiple one-second history
     * ticks, not an arbitrary delay introduced to manufacture history. */
    e = nt_await(&a, FIPSD_NOTICE_SESSION_REKEYED, &b.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    e = nt_await(&b, FIPSD_NOTICE_SESSION_REKEYED, &a.identity.node_addr, NT_WAIT_MS); assert(e); free(e);
    for (test_node *n = &a; n; n = n == &a ? &b : NULL) {
        GoRefPtr ref = nt_snapshot(n); const fipsd_control_snapshot *s = ref.ptr;
        assert(s->history && s->history->has_data && s->history->fast_pushes >= 2 && s->history->peer_count == 1);
        go_refptr_release(&ref);
        r = nt_query(n, "show_stats_peers"); assert(json_integer_value(json_object_get(data(r), "count")) == 1); json_decref(r);
        r = nt_query(n, "show_mmp"); assert(json_array_size(json_object_get(data(r), "peers")) == 1); json_decref(r);
        r = nt_query(n, "show_links"); assert(json_array_size(json_object_get(data(r), "links")) == 1); json_decref(r);
    }
    puts("ACL allowed trace: SIGHUP -> allow_then_deny_all -> LINK_UP both -> datagram -> rekey; non-empty node/peer rings and diagnostic rows both");
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    nt_drain(&a); nt_drain(&b); nt_print_trace("ACL deny / SIGHUP / allow");
    fipsd_runtime_destroy(rt); nt_destroy(&a); nt_destroy(&b); fipsd_loopback_network_destroy(net);
    fipsd_log_set_default(NULL); fipsd_log_destroy(logger); go_channel_unref(reloaded);
    return 0;
}
