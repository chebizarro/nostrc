#include "control/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const queries[] = {
    "show_status", "show_acl", "show_peers", "show_links", "show_tree",
    "show_sessions", "show_bloom", "show_mmp", "show_cache", "show_connections",
    "show_transports", "show_routing", "show_identity_cache", "show_native_flows",
    "show_stats_list", "show_metrics", "show_stats_history", "show_stats_all_history",
    "show_stats_peers", "show_stats_history_all_peers"
};
static const char *const live[] = {
    "connect", "disconnect", "probe_start", "probe_poll", "probe_cancel",
    "path_show", "path_pin", "path_unpin"
};
const char *const *fipsd_control_commands(size_t *count)
{
    *count = sizeof(queries) / sizeof(queries[0]);
    return queries;
}
json_t *fipsd_control_ok(json_t *data)
{
    if (!data) return NULL;
    json_t *out = json_object();
    int a = json_object_set_new(out, "status", json_string("ok"));
    int b = json_object_set_new(out, "data", data); /* consumes even on failure */
    if (!out || a < 0 || b < 0) { json_decref(out); return NULL; }
    return out;
}
json_t *fipsd_control_error(const char *message)
{
    return json_pack("{s:s,s:s}", "status", "error", "message", message);
}
static json_t *missing(const char *field)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "missing '%s' parameter", field);
    return fipsd_control_error(msg);
}
static json_t *live_validate(const char *name, const json_t *params)
{
    if (!strcmp(name, "connect") || !strcmp(name, "disconnect") || !strcmp(name, "probe_start")) {
        if (!params || json_is_null(params)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "missing params for %s", name);
            return fipsd_control_error(msg);
        }
    }
    if (!strcmp(name, "probe_poll") || !strcmp(name, "probe_cancel")) {
        const json_t *id = json_object_get(params, "probe_id");
        if (!json_is_integer(id) || json_integer_value(id) < 0) return missing("probe_id");
    } else {
        if (!json_is_string(json_object_get(params, "npub"))) return missing("npub");
        if (!strcmp(name, "connect")) {
            if (!json_is_string(json_object_get(params, "address"))) return missing("address");
            if (!json_is_string(json_object_get(params, "transport"))) return missing("transport");
        } else if (!strcmp(name, "path_pin") && !json_is_string(json_object_get(params, "transport"))) {
            return missing("transport");
        }
    }
    return NULL;
}
static json_t *dispatch(const fipsd_control_provider *provider, const json_t *request,
                         const fipsd_control_request_context *context)
{
    if (context && context->shutdown && go_context_is_canceled(context->shutdown))
        return fipsd_control_error("node shutting down");
    const char *name = json_string_value(json_object_get(request, "command"));
    if (!json_is_object(request) || !name)
        return fipsd_control_error("invalid request: missing or non-string 'command'");
    /* JSON strings containing NUL cannot name a C command or parameter. */
    if (strlen(name) != json_string_length(json_object_get(request, "command")))
        return fipsd_control_error("invalid request: NUL in command");
    const json_t *params = json_object_get(request, "params");
    if (json_is_null(params)) params = NULL;
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
        if (strcmp(name, queries[i])) continue;
        GoRefPtr ref = {0};
        if (provider && provider->acquire) ref = provider->acquire(provider->user);
        const fipsd_control_snapshot empty = {0};
        json_t *response = fipsd_control_query(ref.ptr ? ref.ptr : &empty, name, params);
        go_refptr_release(&ref);
        return response;
    }
    for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); ++i) {
        if (strcmp(name, live[i])) continue;
        json_t *error = live_validate(name, params);
        if (error) return error;
        json_t *response = NULL;
        if (provider && provider->command)
            response = provider->command(provider->user, name, params, context);
        return response ? response : fipsd_control_error("subsystem not started");
    }
    size_t len = strlen(name) + 32;
    char *msg = malloc(len);
    if (!msg) return NULL;
    snprintf(msg, len, "unknown command: %s", name);
    json_t *result = fipsd_control_error(msg);
    free(msg);
    return result;
}
/* Bound recursion before entering Jansson on a small fiber stack. This only
 * counts containers outside strings; Jansson still validates all JSON syntax. */
static bool bounded_depth(const char *line, size_t len)
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
        else if (c == '[' || c == '{') {
            if (++depth > FIPSD_CONTROL_MAX_DEPTH) return false;
        } else if ((c == ']' || c == '}') && depth) --depth;
    }
    return true;
}
json_t *fipsd_control_parse_context(const fipsd_control_provider *provider, const char *line, size_t len,
                                    const fipsd_control_request_context *context)
{
    if (len > FIPSD_CONTROL_MAX_REQUEST) return fipsd_control_error("read error: request too large");
    if (!len) return fipsd_control_error("empty request");
    if (!bounded_depth(line, len)) return fipsd_control_error("invalid request: nesting limit exceeded");
    json_error_t error;
    json_t *request = json_loadb(line, len, JSON_DECODE_ANY | JSON_REJECT_DUPLICATES, &error);
    if (!request) {
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid request: %s at line %d column %d", error.text, error.line, error.column);
        return fipsd_control_error(msg);
    }
    json_t *response = dispatch(provider, request, context);
    json_decref(request);
    return response;
}
char *fipsd_control_encode(const json_t *response)
{
    const char *status = json_string_value(json_object_get(response, "status"));
    if (!status) return NULL;
    bool ok = !strcmp(status, "ok");
    const json_t *value = json_object_get(response, ok ? "data" : "message");
    char *body = json_dumps(value, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENCODE_ANY);
    if (!body) return NULL;
    size_t cap = strlen(body) + 48;
    char *out = malloc(cap);
    if (out) snprintf(out, cap, "{\"status\":\"%s\",\"%s\":%s}", ok ? "ok" : "error", ok ? "data" : "message", body);
    json_malloc_t allocate_json; json_free_t free_json;
    json_get_alloc_funcs(&allocate_json, &free_json);
    free_json(body);
    return out;
}

json_t *fipsd_control_dispatch(const fipsd_control_provider *provider, const json_t *request)
{
    return dispatch(provider, request, NULL);
}
json_t *fipsd_control_parse(const fipsd_control_provider *provider, const char *line, size_t len)
{
    return fipsd_control_parse_context(provider, line, len, NULL);
}
