#include "native/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fipsd_native_command_clear(fipsd_native_command *c)
{
    free(c->peer); free(c->data);
    memset(c, 0, sizeof *c);
}
fipsd_native_command_error fipsd_native_check_bindable(uint16_t port, char *message, size_t cap)
{
    if (port <= FIPSD_NATIVE_PORT_PROTOCOL_MAX) {
        if (cap) snprintf(message, cap, "port %u is reserved for protocol use", (unsigned)port);
        return FIPSD_NATIVE_CMD_PORT_PROTOCOL;
    }
    if (port <= FIPSD_NATIVE_PORT_STANDARD_MAX) {
        if (cap) snprintf(message, cap, "port %u is reserved for FIPS standard services", (unsigned)port);
        return FIPSD_NATIVE_CMD_PORT_STANDARD;
    }
    return FIPSD_NATIVE_CMD_OK;
}
const char *fipsd_native_command_errno(fipsd_native_command_error error)
{
    switch (error) {
    case FIPSD_NATIVE_CMD_PORT_PROTOCOL: case FIPSD_NATIVE_CMD_PORT_STANDARD: return "EADDRNOTAVAIL";
    default: return "EINVAL";
    }
}
const char *fipsd_native_debug_name(fipsd_native_command_kind kind)
{
    switch (kind) {
    case FIPSD_NATIVE_CMD_INJECT: return "inject";
    case FIPSD_NATIVE_CMD_STATS: return "stats";
    case FIPSD_NATIVE_CMD_ARRIVE: return "arrive";
    default: return NULL;
    }
}
/* serde's spelling of a value's type, for "invalid type" reasons. */
static void describe(const json_t *v, char *out, size_t cap)
{
    if (!v || json_is_null(v)) snprintf(out, cap, "null");
    else if (json_is_boolean(v)) snprintf(out, cap, "boolean `%s`", json_is_true(v) ? "true" : "false");
    else if (json_is_integer(v)) snprintf(out, cap, "integer `%lld`", (long long)json_integer_value(v));
    else if (json_is_real(v)) snprintf(out, cap, "floating point `%g`", json_real_value(v));
    else if (json_is_string(v)) snprintf(out, cap, "string \"%s\"", json_string_value(v));
    else if (json_is_array(v)) snprintf(out, cap, "sequence");
    else snprintf(out, cap, "map");
}
static bool bad_params(const char *command, const char *reason, char *message, size_t cap)
{
    snprintf(message, cap, "invalid params for '%s': %s", command, reason);
    return false;
}
/* serde-shaped field readers: a missing field, a wrong type and a value
 * outside the integer's range each refuse with the reason serde would give. */
static bool take_object(const json_t *params, const char *command, const char *shape, char *message, size_t cap)
{
    if (json_is_object(params)) return true;
    char what[128], reason[256]; describe(params, what, sizeof what);
    snprintf(reason, sizeof reason, "invalid type: %s, expected struct %s", what, shape);
    return bad_params(command, reason, message, cap);
}
static bool take_uint(const json_t *params, const char *command, const char *field, uint64_t max,
                      const char *type, bool required, bool *present, uint64_t *out, char *message, size_t cap)
{
    const json_t *v = json_object_get(params, field);
    *present = false;
    if (!v || (!required && json_is_null(v))) {
        if (!required) return true;
        char reason[128]; snprintf(reason, sizeof reason, "missing field `%s`", field);
        return bad_params(command, reason, message, cap);
    }
    char what[600], reason[700];
    if (!json_is_integer(v)) {
        describe(v, what, sizeof what);
        snprintf(reason, sizeof reason, "invalid type: %s, expected %s", what, type);
        return bad_params(command, reason, message, cap);
    }
    json_int_t n = json_integer_value(v);
    if (n < 0 || (uint64_t)n > max) {
        snprintf(reason, sizeof reason, "invalid value: integer `%lld`, expected %s", (long long)n, type);
        return bad_params(command, reason, message, cap);
    }
    *present = true; *out = (uint64_t)n;
    return true;
}
static bool take_string(const json_t *params, const char *command, const char *field, char **out, char *message, size_t cap)
{
    const json_t *v = json_object_get(params, field);
    if (!v) {
        char reason[128]; snprintf(reason, sizeof reason, "missing field `%s`", field);
        return bad_params(command, reason, message, cap);
    }
    if (!json_is_string(v)) {
        char what[600], reason[700]; describe(v, what, sizeof what);
        snprintf(reason, sizeof reason, "invalid type: %s, expected a string", what);
        return bad_params(command, reason, message, cap);
    }
    *out = strdup(json_string_value(v));
    if (!*out) return bad_params(command, "allocation failed", message, cap);
    return true;
}
/* `framing`, which upstream does not know (its serde structs ignore it, so a
 * client may send it to either daemon): absent, null or "datagram" is the
 * platform's datagram pair, "stream" asks for a framed stream flow. */
static bool take_framing(const json_t *params, const char *command, bool *stream, char *message, size_t cap)
{
    const json_t *v = json_object_get(params, "framing");
    *stream = false;
    if (!v || json_is_null(v)) return true;
    char what[600], reason[700];
    if (!json_is_string(v)) {
        describe(v, what, sizeof what);
        snprintf(reason, sizeof reason, "invalid type: %s, expected `datagram` or `stream`", what);
        return bad_params(command, reason, message, cap);
    }
    const char *text = json_string_value(v);
    if (strlen(text) == json_string_length(v) && !strcmp(text, "stream")) { *stream = true; return true; }
    if (strlen(text) == json_string_length(v) && !strcmp(text, "datagram")) return true;
    snprintf(reason, sizeof reason, "unknown variant `%.500s`, expected `datagram` or `stream`", text);
    return bad_params(command, reason, message, cap);
}
static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* The hex crate's two refusals, spelled as it spells them. */
static bool take_hex(const char *text, uint8_t **out, size_t *len, char *message, size_t cap)
{
    size_t n = strlen(text);
    if (n % 2) { snprintf(message, cap, "invalid hex in 'data': Odd number of digits"); return false; }
    uint8_t *bytes = malloc(n / 2 + 1);
    if (!bytes) { snprintf(message, cap, "invalid hex in 'data': allocation failed"); return false; }
    for (size_t i = 0; i < n; i += 2) {
        int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) {
            size_t at = hi < 0 ? i : i + 1;
            snprintf(message, cap, "invalid hex in 'data': Invalid character '%c' at position %zu", text[at], at);
            free(bytes); return false;
        }
        bytes[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *out = bytes; *len = n / 2;
    return true;
}
fipsd_native_command_error fipsd_native_parse(const json_t *request, fipsd_native_command *out,
                                              char *message, size_t cap)
{
    memset(out, 0, sizeof *out);
    if (cap) message[0] = 0;
    const json_t *command_value = json_object_get(request, "command");
    const char *name = json_string_value(command_value);
    /* A name with an embedded NUL names nothing: serde would compare the
     * whole string and find no such command. */
    if (name && strlen(name) != json_string_length(command_value)) name = "";
    const json_t *params = json_object_get(request, "params");
    if (json_is_null(params)) params = NULL;
    const char *command = NULL;
    fipsd_native_command_kind kind = FIPSD_NATIVE_CMD_CONNECT;
    if (name && !strcmp(name, "connect")) { command = "connect"; kind = FIPSD_NATIVE_CMD_CONNECT; }
    else if (name && !strcmp(name, "listen")) { command = "listen"; kind = FIPSD_NATIVE_CMD_LISTEN; }
    else if (name && !strcmp(name, "stats")) { command = "stats"; kind = FIPSD_NATIVE_CMD_STATS; }
    else if (name && !strcmp(name, "arrive")) { command = "arrive"; kind = FIPSD_NATIVE_CMD_ARRIVE; }
    else if (name && !strcmp(name, "inject")) { command = "inject"; kind = FIPSD_NATIVE_CMD_INJECT; }
    else {
        snprintf(message, cap, "unknown command '%s'", name ? name : "");
        return FIPSD_NATIVE_CMD_UNKNOWN;
    }
    if (!params) {
        snprintf(message, cap, "command '%s' requires params", command);
        return FIPSD_NATIVE_CMD_NO_PARAMS;
    }
    out->kind = kind;
    bool present; uint64_t n;
    fipsd_native_command_error e;
    switch (kind) {
    case FIPSD_NATIVE_CMD_CONNECT:
        if (!take_object(params, command, "ConnectParams", message, cap)) return FIPSD_NATIVE_CMD_BAD_PARAMS;
        if (!take_string(params, command, "peer", &out->peer, message, cap)) goto bad;
        if (!take_uint(params, command, "remote_port", UINT16_MAX, "u16", true, &present, &n, message, cap)) goto bad;
        out->remote = (uint16_t)n;
        if (!take_uint(params, command, "local_port", UINT16_MAX, "u16", false, &present, &n, message, cap)) goto bad;
        if (!take_framing(params, command, &out->stream, message, cap)) goto bad;
        if ((e = fipsd_native_check_bindable(out->remote, message, cap)) != FIPSD_NATIVE_CMD_OK) goto refused;
        /* Port 0 is the UDP convention for "any port": the same thing as
         * omitting the field, accepted rather than refused. */
        if (present && n != 0) {
            if ((e = fipsd_native_check_bindable((uint16_t)n, message, cap)) != FIPSD_NATIVE_CMD_OK) goto refused;
            out->has_local = true; out->local = (uint16_t)n;
        }
        return FIPSD_NATIVE_CMD_OK;
    case FIPSD_NATIVE_CMD_LISTEN:
        if (!take_object(params, command, "ListenParams", message, cap)) return FIPSD_NATIVE_CMD_BAD_PARAMS;
        if (!take_uint(params, command, "local_port", UINT16_MAX, "u16", false, &present, &n, message, cap)) goto bad;
        if (!take_framing(params, command, &out->stream, message, cap)) goto bad;
        if (present && n != 0) {
            if ((e = fipsd_native_check_bindable((uint16_t)n, message, cap)) != FIPSD_NATIVE_CMD_OK) goto refused;
            out->has_local = true; out->local = (uint16_t)n;
        }
        return FIPSD_NATIVE_CMD_OK;
    case FIPSD_NATIVE_CMD_STATS:
        if (!take_object(params, command, "FlowParams", message, cap)) return FIPSD_NATIVE_CMD_BAD_PARAMS;
        if (!take_uint(params, command, "flow_id", UINT64_MAX, "u64", true, &present, &n, message, cap)) goto bad;
        out->flow = n;
        return FIPSD_NATIVE_CMD_OK;
    case FIPSD_NATIVE_CMD_ARRIVE: {
        if (!take_object(params, command, "ArriveParams", message, cap)) return FIPSD_NATIVE_CMD_BAD_PARAMS;
        if (!take_string(params, command, "peer", &out->peer, message, cap)) goto bad;
        if (!take_uint(params, command, "src_port", UINT16_MAX, "u16", true, &present, &n, message, cap)) goto bad;
        out->src = (uint16_t)n;
        if (!take_uint(params, command, "dst_port", UINT16_MAX, "u16", true, &present, &n, message, cap)) goto bad;
        out->dst = (uint16_t)n;
        char *hex = NULL;
        if (!take_string(params, command, "data", &hex, message, cap)) goto bad;
        bool ok = take_hex(hex, &out->data, &out->data_len, message, cap);
        free(hex);
        if (!ok) { e = FIPSD_NATIVE_CMD_BAD_HEX; goto refused; }
        return FIPSD_NATIVE_CMD_OK;
    }
    case FIPSD_NATIVE_CMD_INJECT: {
        if (!take_object(params, command, "InjectParams", message, cap)) return FIPSD_NATIVE_CMD_BAD_PARAMS;
        if (!take_uint(params, command, "flow_id", UINT64_MAX, "u64", true, &present, &n, message, cap)) goto bad;
        out->flow = n;
        char *hex = NULL;
        if (!take_string(params, command, "data", &hex, message, cap)) goto bad;
        uint64_t repeat = 1;
        if (!take_uint(params, command, "repeat", UINT32_MAX, "u32", false, &present, &repeat, message, cap)) { free(hex); goto bad; }
        if (!present) repeat = 1;
        if (repeat == 0 || repeat > FIPSD_NATIVE_MAX_INJECT_REPEAT) {
            free(hex);
            snprintf(message, cap, "repeat must be between 1 and %u, got %llu", FIPSD_NATIVE_MAX_INJECT_REPEAT, (unsigned long long)repeat);
            e = FIPSD_NATIVE_CMD_BAD_REPEAT; goto refused;
        }
        bool ok = take_hex(hex, &out->data, &out->data_len, message, cap);
        free(hex);
        if (!ok) { e = FIPSD_NATIVE_CMD_BAD_HEX; goto refused; }
        out->repeat = (uint32_t)repeat;
        return FIPSD_NATIVE_CMD_OK;
    }
    }
bad:
    fipsd_native_command_clear(out);
    return FIPSD_NATIVE_CMD_BAD_PARAMS;
refused:
    fipsd_native_command_clear(out);
    return e;
}

size_t fipsd_native_json_width(const unsigned char *s, size_t len, size_t *advance)
{
    unsigned char c = s[0];
    if (c < 0x80) {
        *advance = 1;
        if (c == '"' || c == '\\' || c == '\b' || c == '\t' || c == '\n' || c == '\f' || c == '\r') return 2;
        if (c < 0x20) return 6;
        return 1;
    }
    size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (n > len) n = len;
    *advance = n;
    return n;
}
size_t fipsd_native_clip(const char *message, char *out, size_t cap)
{
    static const char mark[] = "\xe2\x80\xa6"; /* U+2026 */
    const unsigned char *s = (const unsigned char *)message;
    size_t len = strlen(message), width = 0, i = 0;
    while (i < len) { size_t adv; width += fipsd_native_json_width(s + i, len - i, &adv); i += adv; }
    if (width <= FIPSD_NATIVE_MAX_MESSAGE) {
        size_t n = len < cap ? len : (cap ? cap - 1 : 0);
        memcpy(out, message, n); if (cap) out[n] = 0;
        return n;
    }
    size_t budget = FIPSD_NATIVE_MAX_MESSAGE - (sizeof mark - 1), used = 0, end = 0;
    i = 0;
    while (i < len) {
        size_t adv; used += fipsd_native_json_width(s + i, len - i, &adv);
        if (used > budget) break;
        i += adv; end = i;
    }
    if (end + sizeof mark > cap) end = cap > sizeof mark ? cap - sizeof mark : 0;
    memcpy(out, message, end); memcpy(out + end, mark, sizeof mark);
    return end + sizeof mark - 1;
}
json_t *fipsd_native_ok(json_t *data)
{
    if (!data) return NULL;
    json_t *out = json_object();
    int a = json_object_set_new(out, "status", json_string("ok"));
    int b = json_object_set_new(out, "data", data);
    if (!out || a < 0 || b < 0) { json_decref(out); return NULL; }
    return out;
}
json_t *fipsd_native_deny(const char *errno_name, const char *message)
{
    char clipped[FIPSD_NATIVE_MAX_MESSAGE * 6 + 8];
    (void)fipsd_native_clip(message, clipped, sizeof clipped);
    return json_pack("{s:s,s:{s:s},s:s}", "status", "error", "data", "errno", errno_name, "message", clipped);
}
static char *append(char *buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (*len + n + 2 > *cap) {
        size_t want = (*len + n + 2) * 2;
        char *grown = realloc(buf, want);
        if (!grown) { free(buf); return NULL; }
        buf = grown; *cap = want;
    }
    memcpy(buf + *len, s, n); *len += n; buf[*len] = 0;
    return buf;
}
static char *dump(const json_t *v)
{
    return json_dumps(v, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENCODE_ANY);
}
static void dump_free(char *s)
{
    json_malloc_t allocate; json_free_t release;
    json_get_alloc_funcs(&allocate, &release);
    release(s);
}
static char *encode(const json_t *response, bool newline)
{
    const json_t *status = json_object_get(response, "status");
    if (!json_is_string(status)) return NULL;
    size_t len = 0, cap = 256;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = 0;
    char *s = dump(status);
    if (!s) { free(out); return NULL; }
    if (!(out = append(out, &len, &cap, "{\"status\":", 10)) || !(out = append(out, &len, &cap, s, strlen(s)))) { dump_free(s); return NULL; }
    dump_free(s);
    const json_t *data = json_object_get(response, "data");
    if (data) {
        s = dump(data);
        if (!s || !(out = append(out, &len, &cap, ",\"data\":", 8)) || !(out = append(out, &len, &cap, s, strlen(s)))) { if (s) dump_free(s); return NULL; }
        dump_free(s);
    }
    const json_t *message = json_object_get(response, "message");
    if (message) {
        s = dump(message);
        if (!s || !(out = append(out, &len, &cap, ",\"message\":", 11)) || !(out = append(out, &len, &cap, s, strlen(s)))) { if (s) dump_free(s); return NULL; }
        dump_free(s);
    }
    return append(out, &len, &cap, newline ? "}\n" : "}", newline ? 2 : 1);
}
char *fipsd_native_encode_reply(const json_t *response) { return encode(response, true); }
char *fipsd_native_encode_object(const json_t *object)
{
    char *s = dump(object);
    if (!s) return NULL;
    char *out = strdup(s);
    dump_free(s);
    return out;
}
bool fipsd_native_bounded_depth(const char *line, size_t len)
{
    size_t depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < len; ++i) {
        char c = line[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (++depth > 64) return false; }
        else if ((c == ']' || c == '}') && depth) --depth;
    }
    return true;
}
bool fipsd_native_line_take(const uint8_t *buf, size_t len, size_t *line_len, size_t *consumed, bool *too_large)
{
    *too_large = false;
    const uint8_t *nl = len ? memchr(buf, '\n', len) : NULL;
    size_t take = nl ? (size_t)(nl - buf) : len;
    if (take > FIPSD_NATIVE_MAX_COMMAND) { *too_large = true; return false; }
    if (!nl) return false;
    *line_len = take; *consumed = take + 1;
    return true;
}
