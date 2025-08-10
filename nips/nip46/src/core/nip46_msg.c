#include "nostr/nip46/nip46_msg.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <jansson.h>

char *nostr_nip46_request_build(const char *id,
                                const char *method,
                                const char *const *params,
                                size_t n_params) {
    if (!id || !method) return NULL;
    json_t *root = json_object();
    if (!root) return NULL;
    if (json_object_set_new(root, "id", json_string(id)) != 0) { json_decref(root); return NULL; }
    if (json_object_set_new(root, "method", json_string(method)) != 0) { json_decref(root); return NULL; }
    json_t *arr = json_array();
    if (!arr) { json_decref(root); return NULL; }
    for (size_t i = 0; i < n_params; ++i) {
        const char *p = (params && params[i]) ? params[i] : "";
        if (json_array_append_new(arr, json_string(p)) != 0) { json_decref(arr); json_decref(root); return NULL; }
    }
    if (json_object_set_new(root, "params", arr) != 0) { json_decref(arr); json_decref(root); return NULL; }
    char *dump = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return dump; /* malloc-compatible */
}

int nostr_nip46_request_parse(const char *json, NostrNip46Request *out) {
    if (!json || !out) return -1; memset(out, 0, sizeof(*out));
    if (nostr_json_get_string(json, "id", &out->id) != 0) return -1;
    if (nostr_json_get_string(json, "method", &out->method) != 0) return -1;
    (void)nostr_json_get_string_array(json, "params", &out->params, &out->n_params);
    return 0;
}

void nostr_nip46_request_free(NostrNip46Request *req) {
    if (!req) return; free(req->id); free(req->method);
    if (req->params) {
        for (size_t i = 0; i < req->n_params; ++i) free(req->params[i]);
        free(req->params);
    }
    memset(req, 0, sizeof(*req));
}

char *nostr_nip46_response_build_ok(const char *id, const char *result_json) {
    if (!id || !result_json) return NULL;
    size_t cap = strlen(result_json) + strlen(id) + 32;
    char *buf = (char*)malloc(cap);
    if (!buf) return NULL;
    snprintf(buf, cap, "{\"id\":\"%s\",\"result\":%s}", id, result_json);
    return buf;
}

char *nostr_nip46_response_build_err(const char *id, const char *error_msg) {
    if (!id || !error_msg) return NULL;
    size_t cap = strlen(error_msg) + strlen(id) + 32;
    char *buf = (char*)malloc(cap);
    if (!buf) return NULL;
    snprintf(buf, cap, "{\"id\":\"%s\",\"error\":\"%s\"}", id, error_msg);
    return buf;
}

int nostr_nip46_response_parse(const char *json, NostrNip46Response *out) {
    if (!json || !out) return -1; memset(out, 0, sizeof(*out));
    if (nostr_json_get_string(json, "id", &out->id) != 0) return -1;
    json_error_t jerr;
    json_t *root = json_loads(json, 0, &jerr);
    if (!root || !json_is_object(root)) { if (root) json_decref(root); return -1; }
    json_t *jres = json_object_get(root, "result");
    if (jres) {
        if (json_is_string(jres)) {
            const char *s = json_string_value(jres);
            if (s) out->result = strdup(s);
        } else {
            char *dump = json_dumps(jres, JSON_COMPACT);
            if (dump) out->result = dump; /* json_dumps allocates with malloc-compatible */
        }
    }
    json_t *jerrv = json_object_get(root, "error");
    if (jerrv && json_is_string(jerrv)) {
        const char *es = json_string_value(jerrv);
        if (es) out->error = strdup(es);
    }
    json_decref(root);
    return 0;
}

void nostr_nip46_response_free(NostrNip46Response *res) {
    if (!res) return; free(res->id); free(res->result); free(res->error); memset(res,0,sizeof(*res));
}
