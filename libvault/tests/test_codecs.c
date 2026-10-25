/* SPDX-License-Identifier: MIT */
#include "helpers.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
typedef struct {
    NostrEvent *event;
    uint8_t sender[32];
    unsigned sends;
    int fail;
} carrier_state;
static int send_grant(void *ctx, const uint8_t recipient[32], const NostrEvent *rumor)
{
    carrier_state *c = ctx;
    (void)recipient;
    ++c->sends;
    assert(rumor->kind == 30804 && !rumor->sig && nostr_tags_size(rumor->tags) == 0);
    c->event = nostr_event_copy(rumor);
    assert(c->event);
    return c->fail ? VAULT_ERR_IO : 0;
}
static int receive_grant(void *ctx, NostrEvent **out, uint8_t sender[32])
{
    carrier_state *c = ctx;
    *out = c->event;
    c->event = NULL;
    memcpy(sender, c->sender, 32);
    return *out ? 0 : VAULT_ERR_AGAIN;
}
static void grant_bad(const char *source, const char *find, const char *replace)
{
    const char *p = strstr(source, find);
    assert(p);
    size_t before = (size_t)(p - source), after = strlen(p + strlen(find));
    char *s = malloc(before + strlen(replace) + after + 1);
    assert(s);
    memcpy(s, source, before);
    strcpy(s + before, replace);
    strcpy(s + before + strlen(replace), p + strlen(find));
    vault_grant *g = (void *)1;
    assert(vault_grant_decode(s, &g) < 0 && !g);
    memset(s, 0, strlen(s));
    free(s);
}
static void probe_private(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "127.0.0.1", &a.sin_addr) == 1);
    assert(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(fd, 2) == 0);
    socklen_t size = sizeof(a);
    assert(getsockname(fd, (struct sockaddr *)&a, &size) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        signal(SIGPIPE, SIG_IGN);
        alarm(10);
        for (unsigned i = 0; i < 2; ++i) {
            int c = accept(fd, NULL, NULL);
            if (c < 0)
                _exit(2);
            char request[8192];
            size_t used = 0;
            while (used + 1 < sizeof(request)) {
                ssize_t n = read(c, request + used, sizeof(request) - used - 1);
                if (n <= 0)
                    _exit(3);
                used += (size_t)n;
                request[used] = 0;
                if (strstr(request, "\r\n\r\n"))
                    break;
            }
            const char *response =
                i == 0
                    ? "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                      "16\r\nConnection: close\r\n\r\n{\"private\":true}"
                    : "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            size_t sent = 0, len = strlen(response);
            while (sent < len) {
                ssize_t n = write(c, response + sent, len - sent);
                if (n <= 0)
                    _exit(4);
                sent += (size_t)n;
            }
            close(c);
        }
        close(fd);
        _exit(0);
    }
    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u", (unsigned)ntohs(a.sin_port));
    blossom_client_opts_t opts = {
        .endpoint = endpoint, .allow_insecure = true, .timeout_seconds = 3};
    blossom_client_t *client = NULL;
    blossom_capability_state_t state = BLOSSOM_CAP_UNKNOWN;
    blossom_error_t rc = blossom_client_new(&opts, NULL, &client);
    if (!rc)
        rc = vault_vsp_probe_blossom(client, &state);
    blossom_client_free(client);
    close(fd);
    if (rc)
        kill(child, SIGKILL);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(rc == 0 && state == BLOSSOM_CAP_YES && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(void)
{
    world w;
    world_init(&w);
    uint8_t next[32] = {0x72};
    uint64_t epoch;
    assert(vault_ring_append(&w.ring, next, &epoch) == 0);
    char *relays[] = {"wss://nos.lol"}, *blossom[] = {"https://blossom.example"};
    vault_grant g = {.ring = w.ring,
                     .cap = VAULT_CAP_WRITE,
                     .relays = relays,
                     .relays_len = 1,
                     .blossom = blossom,
                     .blossom_len = 1,
                     .issued_at = 123};
    memcpy(g.vault_id, w.vault.vault_id, 32);
    memcpy(g.node_id, w.node.node_id, 32);
    memcpy(g.owner, w.pub[0], 32);
    memcpy(g.root_manifest, w.chunks[0].sha256, 32);
    char *json = NULL, *again = NULL;
    assert(vault_grant_encode(&g, &json) == 0);
    vault_grant *opened = NULL;
    assert(vault_grant_decode(json, &opened) == 0 && opened->ring.count == 2 &&
           opened->cap == VAULT_CAP_WRITE && !memcmp(opened->ring.entries[1].node_key, next, 32));
    assert(vault_grant_encode(opened, &again) == 0 && !strcmp(json, again));
    memset(again, 0, strlen(again));
    free(again);
    vault_grant_free(opened);
    grant_bad(json, "\"cap\":\"write\"", "\"cap\":\"admin\"");
    grant_bad(json, "\"issued_at\":123", "\"issued_at\":123,\"issued_at\":123");
    grant_bad(json, "\"issued_at\":123", "\"unknown\":123");
    grant_bad(json, "\"epoch\":1", "\"epoch\":0");
    grant_bad(json, "\"epoch\":2", "\"epoch\":1");
    grant_bad(json, "wss://nos.lol", "file://nos.lol");
    grant_bad(json, "\"issued_at\":123", "\"issued_at\":18446744073709551616");
    size_t z = strlen(json);
    for (size_t i = 0; i < z; ++i) {
        char saved = json[i];
        json[i] = 0;
        assert(vault_grant_decode(json, &opened) < 0 && !opened);
        json[i] = saved;
    }
    memset(json, 0, z);
    free(json);
    carrier_state state = {0};
    memcpy(state.sender, w.pub[0], 32);
    vault_grant_carrier carrier = {&state, send_grant, receive_grant};
    assert(vault_grant_send(&carrier, w.pub[1], &g) == 0 && state.sends == 1);
    assert(vault_grant_receive(&carrier, &opened) == 0 && opened->ring.count == 2);
    vault_grant_free(opened);
    assert(vault_grant_send(&carrier, w.pub[1], &g) == 0);
    memcpy(state.sender, w.pub[2], 32);
    assert(vault_grant_receive(&carrier, &opened) == VAULT_ERR_AUTH && !opened);
    assert(vault_grant_send(&carrier, w.pub[1], &g) == 0);
    assert(nostr_event_sign(state.event, w.secret[0]) == 0);
    assert(vault_grant_rumor_open(state.event, &opened) == VAULT_ERR_AUTH && !opened);
    vault_grant_rumor_free(state.event);
    state.event = NULL;
    state.fail = 1;
    unsigned sends = state.sends;
    assert(vault_grant_send(&carrier, w.pub[1], &g) == VAULT_ERR_IO && state.sends == sends + 1);
    vault_grant_rumor_free(state.event);
    state.event = NULL;
    vault_service_list services = {relays, blossom, 1, 1}, *decoded = NULL;
    assert(vault_service_list_encode(&services, &json) == 0 &&
           vault_service_list_decode(json, &decoded) == 0 && decoded->relays_len == 1 &&
           !strcmp(decoded->relays[0], relays[0]));
    vault_service_list_free(decoded);
    free(json);
    assert(vault_service_list_decode("{\"vault-relay\":[],\"vault-relay\":[],\"vault-blossom\":[]}",
                                     &decoded) == VAULT_ERR_ARG &&
           !decoded);
    NostrEvent *e = NULL;
    assert(vault_service_list_event_new(&services, w.sk[0], 123, &e) == 0 && e->kind == 10319 &&
           nostr_tags_size(e->tags) == 0 && !e->sig);
    assert(vault_service_list_event_open(e, w.sk[0], &decoded) == VAULT_ERR_AUTH && !decoded);
    assert(nostr_event_sign(e, w.secret[0]) == 0);
    char *wire = nostr_event_serialize(e);
    assert(wire);
    NostrEvent *received = nostr_event_new();
    assert(nostr_event_deserialize(received, wire) == 0);
    free(wire);
    assert(vault_service_list_event_open(received, w.sk[0], &decoded) == 0 &&
           decoded->blossom_len == 1 && !strcmp(decoded->blossom[0], blossom[0]));
    vault_service_list_free(decoded);
    assert(vault_service_list_event_open(received, w.sk[1], &decoded) == VAULT_ERR_AUTH &&
           !decoded);
    received->content[10] = received->content[10] == 'A' ? 'B' : 'A';
    assert(nostr_event_sign(received, w.secret[0]) == 0);
    assert(vault_service_list_event_open(received, w.sk[0], &decoded) == PORTHOME_ERR_CRYPTO &&
           !decoded);
    nostr_event_free(received);
    nostr_event_free(e);
    const struct {
        const char *json;
        blossom_capability_state_t expected;
        int rc;
    } cases[] = {{"{\"vault\":true,\"limitation\":{\"auth_required\":true}}", BLOSSOM_CAP_YES, 0},
                 {"{\"vault\":true,\"limitation\":{\"auth_required\":false}}", BLOSSOM_CAP_NO, 0},
                 {"{\"vault\":false,\"limitation\":{\"auth_required\":true}}", BLOSSOM_CAP_NO, 0},
                 {"{\"limitation\":{\"auth_required\":true}}", BLOSSOM_CAP_UNKNOWN, 0},
                 {"{\"other\":{\"vault\":true,\"auth_required\":true}}", BLOSSOM_CAP_UNKNOWN, 0},
                 {"{\"vault\":true,\"vault\":true,\"limitation\":{\"auth_required\":true}}",
                  BLOSSOM_CAP_UNKNOWN, VAULT_ERR_ARG},
                 {"{\"vault\":true,\"limitation\":{\"auth_required\":\"true\"}}",
                  BLOSSOM_CAP_UNKNOWN, VAULT_ERR_ARG},
                 {"{\"vault\":true,\"limitation\":{\"auth_required\":true},\"extension\":[-1.2e3,"
                  "null,\"line\\ntext\"]}",
                  BLOSSOM_CAP_YES, 0},
                 {"{\"vault\":true,\"limitation\":{\"auth_required\":true}}junk",
                  BLOSSOM_CAP_UNKNOWN, VAULT_ERR_ARG}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        blossom_capability_state_t out;
        assert(vault_vsp_relay_classify(cases[i].json, &out) == cases[i].rc &&
               out == cases[i].expected);
    }
    blossom_server_capabilities_t c;
    blossom_server_capabilities_init(&c);
    assert(vault_vsp_blossom_classify(&c) == BLOSSOM_CAP_UNKNOWN);
    c.reachable = true;
    c.private_server = BLOSSOM_CAP_YES;
    assert(vault_vsp_blossom_classify(&c) == BLOSSOM_CAP_YES);
    c.private_server = BLOSSOM_CAP_NO;
    assert(vault_vsp_blossom_classify(&c) == BLOSSOM_CAP_NO);
    probe_private();
    world_dispose(&w);
    puts("OK grant/carrier interface/10319/VSP (local HTTP probe, no relay transport)");
    return 0;
}
