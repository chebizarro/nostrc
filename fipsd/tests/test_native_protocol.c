/* Native API line protocol: command parsing, port tiers, errno names,
 * refusal texts, the message clip and the exact byte layouts upstream's
 * serde writes (src/native/protocol.rs, mod.rs read_command/clip tests,
 * tests/replies.rs). No socket and no node. */
#include "native/protocol.h"
#include "native/registry.h"
#include "native/link.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

static fipsd_native_command_error parse(const char *text, fipsd_native_command *out, char *message, size_t cap)
{
    json_error_t e;
    json_t *request = json_loads(text, JSON_ALLOW_NUL, &e);
    if (!request) fprintf(stderr, "cannot parse %s: %s\n", text, e.text);
    CHECK(request);
    fipsd_native_command_error rc = fipsd_native_parse(request, out, message, cap);
    json_decref(request);
    return rc;
}
static void test_connect_carries_the_peer_and_both_ports(void)
{
    fipsd_native_command c; char m[256];
    CHECK(parse("{\"command\":\"connect\",\"params\":{\"peer\":\"npub1abc\",\"remote_port\":4242,\"local_port\":5000}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK);
    CHECK(c.kind == FIPSD_NATIVE_CMD_CONNECT && !strcmp(c.peer, "npub1abc") && c.remote == 4242 && c.has_local && c.local == 5000);
    fipsd_native_command_clear(&c);
    /* An absent local port, and a zero one, both ask for an ephemeral port. */
    CHECK(parse("{\"command\":\"connect\",\"params\":{\"peer\":\"npub1abc\",\"remote_port\":4242}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK);
    CHECK(!c.has_local); fipsd_native_command_clear(&c);
    CHECK(parse("{\"command\":\"connect\",\"params\":{\"peer\":\"npub1abc\",\"remote_port\":4242,\"local_port\":0}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK);
    CHECK(!c.has_local); fipsd_native_command_clear(&c);
    CHECK(parse("{\"command\":\"connect\",\"params\":{\"peer\":\"npub1abc\",\"remote_port\":4242,\"local_port\":null}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK);
    CHECK(!c.has_local); fipsd_native_command_clear(&c);
}
static void test_port_tiers(void)
{
    fipsd_native_command c; char m[256];
    CHECK(parse("{\"command\":\"listen\",\"params\":{\"local_port\":200}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_PORT_PROTOCOL);
    CHECK(!strcmp(m, "port 200 is reserved for protocol use"));
    CHECK(parse("{\"command\":\"listen\",\"params\":{\"local_port\":256}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_PORT_STANDARD);
    CHECK(!strcmp(m, "port 256 is reserved for FIPS standard services"));
    CHECK(fipsd_native_check_bindable(1023, m, sizeof m) == FIPSD_NATIVE_CMD_PORT_STANDARD);
    CHECK(fipsd_native_check_bindable(1024, m, sizeof m) == FIPSD_NATIVE_CMD_OK);
    /* The far end's shim is no more addressable than our own. */
    CHECK(parse("{\"command\":\"connect\",\"params\":{\"peer\":\"npub1abc\",\"remote_port\":256}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_PORT_STANDARD);
    CHECK(!strcmp(fipsd_native_command_errno(FIPSD_NATIVE_CMD_PORT_STANDARD), "EADDRNOTAVAIL"));
    CHECK(!strcmp(fipsd_native_command_errno(FIPSD_NATIVE_CMD_PORT_PROTOCOL), "EADDRNOTAVAIL"));
    CHECK(!strcmp(fipsd_native_command_errno(FIPSD_NATIVE_CMD_UNKNOWN), "EINVAL"));
    CHECK(!strcmp(fipsd_native_command_errno(FIPSD_NATIVE_CMD_NO_PARAMS), "EINVAL"));
    CHECK(!strcmp(fipsd_native_command_errno(FIPSD_NATIVE_CMD_BAD_PARAMS), "EINVAL"));
    CHECK(!strcmp(fipsd_native_command_errno(FIPSD_NATIVE_CMD_BAD_HEX), "EINVAL"));
    CHECK(!strcmp(fipsd_native_command_errno(FIPSD_NATIVE_CMD_BAD_REPEAT), "EINVAL"));
}
static void test_refusals_name_themselves(void)
{
    fipsd_native_command c; char m[256];
    CHECK(parse("{\"command\":\"teleport\"}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_UNKNOWN);
    CHECK(!strcmp(m, "unknown command 'teleport'"));
    CHECK(parse("{\"command\":\"stats\"}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_NO_PARAMS);
    CHECK(!strcmp(m, "command 'stats' requires params"));
    CHECK(parse("{\"command\":\"listen\",\"params\":{\"local_port\":\"not a number\"}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_BAD_PARAMS);
    CHECK(!strncmp(m, "invalid params for 'listen': ", 29));
    CHECK(parse("{\"command\":\"connect\",\"params\":{\"peer\":\"npub1abc\"}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_BAD_PARAMS);
    CHECK(strstr(m, "missing field `remote_port`"));
    CHECK(parse("{\"command\":\"connect\",\"params\":{\"peer\":\"npub1abc\",\"remote_port\":70000}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_BAD_PARAMS);
    CHECK(parse("{\"command\":\"connect\",\"params\":[1]}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_BAD_PARAMS);
    /* accept and reject went with the round trip they existed for. */
    CHECK(parse("{\"command\":\"accept\",\"params\":{\"flow_id\":7}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_UNKNOWN);
    CHECK(strstr(m, "accept"));
    CHECK(parse("{\"command\":\"reject\",\"params\":{\"flow_id\":7}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_UNKNOWN);
    /* A name with an embedded NUL names nothing. */
    CHECK(parse("{\"command\":\"connect\\u0000x\",\"params\":{}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_UNKNOWN);
}
static void test_listen_and_debug_commands(void)
{
    fipsd_native_command c; char m[256];
    CHECK(parse("{\"command\":\"listen\",\"params\":{\"local_port\":0}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK && !c.has_local);
    CHECK(parse("{\"command\":\"listen\",\"params\":{}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK && !c.has_local);
    CHECK(parse("{\"command\":\"listen\",\"params\":{\"local_port\":4242}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK && c.has_local && c.local == 4242);
    CHECK(parse("{\"command\":\"stats\",\"params\":{\"flow_id\":9}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK && c.flow == 9);
    CHECK(!strcmp(fipsd_native_debug_name(c.kind), "stats"));
    CHECK(parse("{\"command\":\"arrive\",\"params\":{\"peer\":\"npub1abc\",\"src_port\":5000,\"dst_port\":4242,\"data\":\"00ff10\"}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK);
    CHECK(c.src == 5000 && c.dst == 4242 && c.data_len == 3 && c.data[0] == 0 && c.data[1] == 0xff && c.data[2] == 0x10);
    CHECK(!strcmp(fipsd_native_debug_name(c.kind), "arrive"));
    fipsd_native_command_clear(&c);
    CHECK(parse("{\"command\":\"inject\",\"params\":{\"flow_id\":1,\"data\":\"ab\"}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK && c.repeat == 1);
    fipsd_native_command_clear(&c);
    CHECK(parse("{\"command\":\"inject\",\"params\":{\"flow_id\":1,\"data\":\"ab\",\"repeat\":3}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_OK && c.repeat == 3);
    CHECK(!strcmp(fipsd_native_debug_name(c.kind), "inject"));
    fipsd_native_command_clear(&c);
    CHECK(parse("{\"command\":\"inject\",\"params\":{\"flow_id\":1,\"data\":\"00\",\"repeat\":0}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_BAD_REPEAT);
    CHECK(!strcmp(m, "repeat must be between 1 and 64, got 0"));
    CHECK(parse("{\"command\":\"inject\",\"params\":{\"flow_id\":1,\"data\":\"00\",\"repeat\":99999}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_BAD_REPEAT);
    CHECK(strstr(m, "repeat"));
    CHECK(parse("{\"command\":\"inject\",\"params\":{\"flow_id\":1,\"data\":\"abc\"}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_BAD_HEX);
    CHECK(!strcmp(m, "invalid hex in 'data': Odd number of digits"));
    CHECK(parse("{\"command\":\"inject\",\"params\":{\"flow_id\":1,\"data\":\"zz\"}}", &c, m, sizeof m) == FIPSD_NATIVE_CMD_BAD_HEX);
    CHECK(!strcmp(m, "invalid hex in 'data': Invalid character 'z' at position 0"));
    CHECK(!fipsd_native_debug_name(FIPSD_NATIVE_CMD_CONNECT) && !fipsd_native_debug_name(FIPSD_NATIVE_CMD_LISTEN));
}
static void test_registry_errnos_and_texts(void)
{
    char m[256];
    CHECK(!strcmp(fipsd_native_registry_errno(FIPSD_NATIVE_REG_PORT_TAKEN), "EADDRINUSE"));
    CHECK(!strcmp(fipsd_native_registry_errno(FIPSD_NATIVE_REG_FLOW_TAKEN), "EADDRINUSE"));
    CHECK(!strcmp(fipsd_native_registry_errno(FIPSD_NATIVE_REG_NO_PORT), "EADDRNOTAVAIL"));
    CHECK(!strcmp(fipsd_native_registry_errno(FIPSD_NATIVE_REG_TOO_MANY_FLOWS), "EMFILE"));
    CHECK(!strcmp(fipsd_native_registry_errno(FIPSD_NATIVE_REG_PORT_STANDARD), "EADDRNOTAVAIL"));
    CHECK(!strcmp(fipsd_native_registry_errno(FIPSD_NATIVE_REG_PORT_PROTOCOL), "EADDRNOTAVAIL"));
    CHECK(!strcmp(fipsd_native_registry_errno(FIPSD_NATIVE_REG_BACKLOG_FULL), "ECONNREFUSED"));
    CHECK(!strcmp(fipsd_native_registry_errno(FIPSD_NATIVE_REG_NO_PENDING), "ECONNREFUSED"));
    fipsd_native_refusal r = {.error = FIPSD_NATIVE_REG_PORT_TAKEN, .port = 4242};
    fipsd_native_refusal_message(&r, m, sizeof m); CHECK(!strcmp(m, "port 4242 is already in use on this node"));
    r = (fipsd_native_refusal){.error = FIPSD_NATIVE_REG_FLOW_TAKEN, .local = 4242, .remote = 5000};
    fipsd_native_refusal_message(&r, m, sizeof m); CHECK(!strcmp(m, "a flow to that peer between ports 4242 and 5000 already exists"));
    r = (fipsd_native_refusal){.error = FIPSD_NATIVE_REG_NO_PORT};
    fipsd_native_refusal_message(&r, m, sizeof m); CHECK(!strcmp(m, "no ephemeral port is free"));
    r = (fipsd_native_refusal){.error = FIPSD_NATIVE_REG_TOO_MANY_FLOWS, .max = 256};
    fipsd_native_refusal_message(&r, m, sizeof m); CHECK(!strcmp(m, "this node holds its maximum of 256 flows"));
    r = (fipsd_native_refusal){.error = FIPSD_NATIVE_REG_BACKLOG_FULL, .port = 4242};
    fipsd_native_refusal_message(&r, m, sizeof m); CHECK(!strcmp(m, "listener on port 4242 has a full backlog"));
    r = (fipsd_native_refusal){.error = FIPSD_NATIVE_REG_NO_PENDING, .flow = 9};
    fipsd_native_refusal_message(&r, m, sizeof m); CHECK(!strcmp(m, "no pending flow 9"));
    /* The drop texts the debug arrive command reports, and their counters. */
    CHECK(!strcmp(fipsd_native_drop_reason_text(FIPSD_NATIVE_REASON_NO_PORT), "no listener or flow on that port"));
    CHECK(!strcmp(fipsd_native_drop_reason_text(FIPSD_NATIVE_REASON_BACKLOG_FULL), "listener backlog full"));
    CHECK(!strcmp(fipsd_native_drop_reason_text(FIPSD_NATIVE_REASON_TOO_MANY_FLOWS), "node flow ceiling reached"));
    CHECK(!strcmp(fipsd_native_drop_reason_text(FIPSD_NATIVE_REASON_PEER_SHARE), "peer flow limit reached"));
    CHECK(!strcmp(fipsd_native_drop_reason_text(FIPSD_NATIVE_REASON_PENDING_QUEUE_FULL), "queue full"));
    CHECK(!strcmp(fipsd_native_drop_reason_text(FIPSD_NATIVE_REASON_FLOW_QUEUE_FULL), "queue full"));
    CHECK(!strcmp(fipsd_native_drop_reason_text(FIPSD_NATIVE_REASON_ARRIVAL_QUEUE_FULL), "arrival queue full"));
    CHECK(!strcmp(fipsd_native_drop_reason_text(FIPSD_NATIVE_REASON_LISTENER_NOT_READING), "listener not reading arrivals"));
    CHECK(!strcmp(fipsd_native_drop_reason_text(FIPSD_NATIVE_REASON_LISTENER_GONE), "listener closed"));
    CHECK(fipsd_native_drop_reason_metric(FIPSD_NATIVE_REASON_PENDING_QUEUE_FULL) == FIPSD_METRIC_native_drop_pending_queue_full);
    CHECK(fipsd_native_drop_reason_metric(FIPSD_NATIVE_REASON_FLOW_QUEUE_FULL) == FIPSD_METRIC_native_drop_flow_queue_full);
    /* The same event spelled three ways by the platforms. */
    CHECK(fipsd_native_drop_reason_why(EPIPE) == FIPSD_NATIVE_REASON_LISTENER_GONE);
    CHECK(fipsd_native_drop_reason_why(ECONNRESET) == FIPSD_NATIVE_REASON_LISTENER_GONE);
    CHECK(fipsd_native_drop_reason_why(EDESTADDRREQ) == FIPSD_NATIVE_REASON_LISTENER_GONE);
    CHECK(fipsd_native_drop_reason_why(ENOBUFS) == FIPSD_NATIVE_REASON_LISTENER_NOT_READING);
    CHECK(fipsd_native_drop_reason_why(EAGAIN) == FIPSD_NATIVE_REASON_LISTENER_NOT_READING);
}
/* The byte layouts: serde Response order (status, data, message), nested
 * maps in lexical key order, and the arrival message with no newline. */
static void test_reply_layouts(void)
{
    json_t *ok = fipsd_native_ok(json_pack("{s:i,s:s,s:i}", "local_port", 4242,
        "node", "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d", "backlog", 16));
    char *line = fipsd_native_encode_reply(ok); json_decref(ok);
    CHECK(line && !strcmp(line, "{\"status\":\"ok\",\"data\":{\"backlog\":16,\"local_port\":4242,\"node\":\"npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d\"}}\n"));
    free(line);
    json_t *deny = fipsd_native_deny("EADDRNOTAVAIL", "port 256 is reserved for FIPS standard services");
    line = fipsd_native_encode_reply(deny); json_decref(deny);
    CHECK(line && !strcmp(line, "{\"status\":\"error\",\"data\":{\"errno\":\"EADDRNOTAVAIL\"},\"message\":\"port 256 is reserved for FIPS standard services\"}\n"));
    free(line);
    json_t *connect = fipsd_native_ok(json_pack("{s:I,s:i,s:i,s:s,s:s,s:i}", "flow_id", (json_int_t)3, "local_port", 49152, "remote_port", 4242,
        "peer", "npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m",
        "node", "npub1n9lpnv0592cc2ps6nm0ca3qls642vx7yjsv35rkxqzj2vgds52sqgpverl", "max_payload", 1362));
    line = fipsd_native_encode_reply(connect); json_decref(connect);
    CHECK(line && !strcmp(line, "{\"status\":\"ok\",\"data\":{\"flow_id\":3,\"local_port\":49152,\"max_payload\":1362,"
        "\"node\":\"npub1n9lpnv0592cc2ps6nm0ca3qls642vx7yjsv35rkxqzj2vgds52sqgpverl\","
        "\"peer\":\"npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m\",\"remote_port\":4242}}\n"));
    free(line);
    json_t *arrival = json_pack("{s:I,s:s,s:s,s:i,s:i,s:i,s:I}", "flow_id", (json_int_t)9,
        "peer", "npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m",
        "node", "npub1n9lpnv0592cc2ps6nm0ca3qls642vx7yjsv35rkxqzj2vgds52sqgpverl",
        "local_port", 4242, "remote_port", 5001, "max_payload", 1362, "held", (json_int_t)1);
    char *message = fipsd_native_encode_object(arrival); json_decref(arrival);
    CHECK(message && !strcmp(message, "{\"flow_id\":9,\"held\":1,\"local_port\":4242,\"max_payload\":1362,"
        "\"node\":\"npub1n9lpnv0592cc2ps6nm0ca3qls642vx7yjsv35rkxqzj2vgds52sqgpverl\","
        "\"peer\":\"npub1sjlh2c3x9w7kjsqg2ay080n2lff2uvt325vpan33ke34rn8l5jcqawh57m\",\"remote_port\":5001}"));
    CHECK(!strchr(message, '\n'));
    free(message);
    /* A refusal's message is JSON-escaped exactly as serde escapes it. */
    deny = fipsd_native_deny("EINVAL", "quote \" backslash \\ tab \t bell \x01 slash / e\xcc\x81");
    line = fipsd_native_encode_reply(deny); json_decref(deny);
    CHECK(line && !strcmp(line, "{\"status\":\"error\",\"data\":{\"errno\":\"EINVAL\"},\"message\":\"quote \\\" backslash \\\\ tab \\t bell \\u0001 slash / e\xcc\x81\"}\n"));
    free(line);
}
/* serde_json escape widths for every ASCII byte and multibyte text. */
static size_t serialised_width(const char *s)
{
    json_t *v = json_string(s);
    char *d = json_dumps(v, JSON_COMPACT | JSON_ENCODE_ANY);
    size_t n = strlen(d) - 2;
    free(d); json_decref(v);
    return n;
}
static void test_clip(void)
{
    char out[FIPSD_NATIVE_MAX_MESSAGE * 6 + 8];
    for (unsigned byte = 1; byte <= 0x7f; ++byte) {
        char sample[FIPSD_NATIVE_MAX_MESSAGE + 2];
        memset(sample, (int)byte, FIPSD_NATIVE_MAX_MESSAGE + 1); sample[FIPSD_NATIVE_MAX_MESSAGE + 1] = 0;
        size_t n = fipsd_native_clip(sample, out, sizeof out);
        CHECK(n == strlen(out));
        CHECK(serialised_width(out) <= FIPSD_NATIVE_MAX_MESSAGE);
        CHECK(n >= 3 && !memcmp(out + n - 3, "\xe2\x80\xa6", 3));
    }
    char multibyte[FIPSD_NATIVE_MAX_MESSAGE * 4 + 1] = "";
    for (size_t i = 0; i < FIPSD_NATIVE_MAX_MESSAGE; ++i) strcat(multibyte, "\xc3\xa9");
    CHECK(serialised_width(out) <= FIPSD_NATIVE_MAX_MESSAGE);
    (void)fipsd_native_clip(multibyte, out, sizeof out);
    CHECK(serialised_width(out) <= FIPSD_NATIVE_MAX_MESSAGE && strstr(out, "\xe2\x80\xa6"));
    multibyte[0] = 0;
    for (size_t i = 0; i < FIPSD_NATIVE_MAX_MESSAGE; ++i) strcat(multibyte, "\xf0\x9f\xa6\x80");
    (void)fipsd_native_clip(multibyte, out, sizeof out);
    CHECK(serialised_width(out) <= FIPSD_NATIVE_MAX_MESSAGE);
    char tail[FIPSD_NATIVE_MAX_MESSAGE + 1];
    memset(tail, 'x', FIPSD_NATIVE_MAX_MESSAGE - 5); tail[FIPSD_NATIVE_MAX_MESSAGE - 5] = 1; tail[FIPSD_NATIVE_MAX_MESSAGE - 4] = 0;
    (void)fipsd_native_clip(tail, out, sizeof out);
    CHECK(serialised_width(out) <= FIPSD_NATIVE_MAX_MESSAGE);
    const char *fits = "unknown command 'teleport'";
    (void)fipsd_native_clip(fits, out, sizeof out); CHECK(!strcmp(out, fits));
    char exact[FIPSD_NATIVE_MAX_MESSAGE + 1]; memset(exact, 'x', FIPSD_NATIVE_MAX_MESSAGE); exact[FIPSD_NATIVE_MAX_MESSAGE] = 0;
    (void)fipsd_native_clip(exact, out, sizeof out); CHECK(!strcmp(out, exact));
    /* The reply built from a MAX_COMMAND-byte name stays under 2048 bytes. */
    char name[FIPSD_NATIVE_MAX_COMMAND + 1]; memset(name, 1, FIPSD_NATIVE_MAX_COMMAND); name[FIPSD_NATIVE_MAX_COMMAND] = 0;
    char *message = malloc(FIPSD_NATIVE_MAX_COMMAND + 64); CHECK(message);
    snprintf(message, FIPSD_NATIVE_MAX_COMMAND + 64, "unknown command '%s'", name);
    json_t *deny = fipsd_native_deny("EINVAL", message);
    char *line = fipsd_native_encode_reply(deny); json_decref(deny); free(message);
    CHECK(line && strlen(line) < 2048);
    free(line);
}
/* The command reader's cap holds whether or not the newline is in hand. */
static void test_line_take(void)
{
    uint8_t *buf = malloc(FIPSD_NATIVE_MAX_COMMAND + 2); CHECK(buf);
    size_t line_len, consumed; bool too_large;
    memset(buf, 'x', FIPSD_NATIVE_MAX_COMMAND + 1); buf[FIPSD_NATIVE_MAX_COMMAND + 1] = '\n';
    CHECK(!fipsd_native_line_take(buf, FIPSD_NATIVE_MAX_COMMAND + 2, &line_len, &consumed, &too_large) && too_large);
    CHECK(!fipsd_native_line_take(buf, FIPSD_NATIVE_MAX_COMMAND + 1, &line_len, &consumed, &too_large) && too_large);
    /* And the healthy path: a command at exactly the cap is accepted. */
    buf[FIPSD_NATIVE_MAX_COMMAND] = '\n';
    CHECK(fipsd_native_line_take(buf, FIPSD_NATIVE_MAX_COMMAND + 1, &line_len, &consumed, &too_large));
    CHECK(line_len == FIPSD_NATIVE_MAX_COMMAND && consumed == FIPSD_NATIVE_MAX_COMMAND + 1 && !too_large);
    /* Two commands in one buffer are taken in turn. */
    const char *two = "{\"command\":\"listen\"}\n{\"command\":\"stats\"}\n";
    CHECK(fipsd_native_line_take((const uint8_t *)two, strlen(two), &line_len, &consumed, &too_large));
    CHECK(line_len == 20 && consumed == 21);
    CHECK(fipsd_native_line_take((const uint8_t *)two + consumed, strlen(two) - consumed, &line_len, &consumed, &too_large));
    CHECK(line_len == 19 && consumed == 20);
    CHECK(!fipsd_native_line_take((const uint8_t *)two, 5, &line_len, &consumed, &too_large) && !too_large);
    free(buf);
}
int main(void)
{
    test_connect_carries_the_peer_and_both_ports();
    test_port_tiers();
    test_refusals_name_themselves();
    test_listen_and_debug_commands();
    test_registry_errnos_and_texts();
    test_reply_layouts();
    test_clip();
    test_line_take();
    puts("native protocol: parse vectors, errno table, reply layouts, clip, line cap: ok");
    return 0;
}
