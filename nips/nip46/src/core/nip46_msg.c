#include "nostr/nip46/nip46_msg.h"
#include <jansson.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

#define MESSAGE_MAX (64u * 1024u)
#define ID_MAX 256u
#define METHOD_MAX 128u
#define PARAM_MAX 64u

static json_t *load_object(const char *text) {
    if (!text || strlen(text) > MESSAGE_MAX) return NULL;
    json_error_t error;
    json_t *root = json_loads(text, JSON_REJECT_DUPLICATES, &error);
    if (!root || !json_is_object(root)) {
        json_decref(root);
        return NULL;
    }
    return root;
}

static char *dump_compact(json_t *value) {
    char *text = json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII | JSON_ENCODE_ANY);
    if (text && strlen(text) > MESSAGE_MAX) {
        free(text);
        return NULL;
    }
    return text;
}

char *nostr_nip46_request_id_generate(void) {
    unsigned char random[32];
    static const char hex[] = "0123456789abcdef";
    if (RAND_bytes(random, sizeof(random)) != 1) return NULL;
    char *id = malloc(65u);
    if (!id) { memset(random, 0, sizeof(random)); return NULL; }
    for (size_t i = 0; i < sizeof(random); i++) {
        id[i * 2u] = hex[random[i] >> 4];
        id[i * 2u + 1u] = hex[random[i] & 0x0fu];
    }
    id[64] = '\0';
    memset(random, 0, sizeof(random));
    return id;
}

char *nostr_nip46_request_build(const char *id, const char *method,
                                const char *const *params, size_t n_params) {
    if (!id || !*id || strlen(id) > ID_MAX || !method || !*method ||
        strlen(method) > METHOD_MAX || n_params > PARAM_MAX ||
        (n_params && !params)) return NULL;
    json_t *root = json_object();
    json_t *array = json_array();
    if (!root || !array || json_object_set_new(root, "id", json_string(id)) ||
        json_object_set_new(root, "method", json_string(method))) goto fail;
    for (size_t i = 0; i < n_params; i++) {
        if (!params[i] || strlen(params[i]) > MESSAGE_MAX ||
            json_array_append_new(array, json_string(params[i]))) goto fail;
    }
    if (json_object_set(root, "params", array)) goto fail;
    json_decref(array);
    array = NULL;
    char *text = dump_compact(root);
    json_decref(root);
    return text;
fail:
    json_decref(array);
    json_decref(root);
    return NULL;
}

int nostr_nip46_request_parse(const char *text, NostrNip46Request *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    json_t *root = load_object(text);
    if (!root) return -1;
    json_t *id = json_object_get(root, "id");
    json_t *method = json_object_get(root, "method");
    json_t *params = json_object_get(root, "params");
    if (!json_is_string(id) || !json_is_string(method) || !json_is_array(params) ||
        !*json_string_value(id) || strlen(json_string_value(id)) > ID_MAX ||
        !*json_string_value(method) || strlen(json_string_value(method)) > METHOD_MAX ||
        json_array_size(params) > PARAM_MAX) goto fail;
    out->id = strdup(json_string_value(id));
    out->method = strdup(json_string_value(method));
    out->n_params = json_array_size(params);
    if (!out->id || !out->method) goto fail;
    if (out->n_params) {
        out->params = calloc(out->n_params, sizeof(*out->params));
        if (!out->params) goto fail;
    }
    for (size_t i = 0; i < out->n_params; i++) {
        json_t *item = json_array_get(params, i);
        out->params[i] = json_is_string(item)
            ? strdup(json_string_value(item)) : dump_compact(item);
        if (!out->params[i]) goto fail;
    }
    json_decref(root);
    return 0;
fail:
    json_decref(root);
    nostr_nip46_request_free(out);
    return -1;
}

void nostr_nip46_request_free(NostrNip46Request *req) {
    if (!req) return;
    free(req->id); free(req->method);
    if (req->params)
        for (size_t i = 0; i < req->n_params; i++) free(req->params[i]);
    free(req->params);
    memset(req, 0, sizeof(*req));
}

char *nostr_nip46_response_build_ok(const char *id, const char *result_json) {
    if (!id || !*id || strlen(id) > ID_MAX || !result_json ||
        strlen(result_json) > MESSAGE_MAX) return NULL;
    json_error_t error;
    json_t *result = json_loads(result_json, JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error);
    json_t *root = json_object();
    if (!result || !root || json_object_set_new(root, "id", json_string(id)) ||
        json_object_set(root, "result", result)) {
        json_decref(result); json_decref(root); return NULL;
    }
    json_decref(result);
    char *text = dump_compact(root);
    json_decref(root);
    return text;
}

char *nostr_nip46_response_build_err(const char *id, const char *error_msg) {
    if (!id || !*id || strlen(id) > ID_MAX || !error_msg ||
        strlen(error_msg) > MESSAGE_MAX) return NULL;
    json_t *root = json_object();
    if (!root || json_object_set_new(root, "id", json_string(id)) ||
        json_object_set_new(root, "error", json_string(error_msg))) {
        json_decref(root); return NULL;
    }
    char *text = dump_compact(root);
    json_decref(root);
    return text;
}

int nostr_nip46_response_parse(const char *text, NostrNip46Response *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    json_t *root = load_object(text);
    if (!root) return -1;
    json_t *id = json_object_get(root, "id");
    json_t *result = json_object_get(root, "result");
    json_t *error = json_object_get(root, "error");
    if (!json_is_string(id) || !*json_string_value(id) ||
        strlen(json_string_value(id)) > ID_MAX || (!result && !error) ||
        (error && !json_is_string(error))) goto fail;
    out->id = strdup(json_string_value(id));
    if (result) out->result = json_is_string(result)
        ? strdup(json_string_value(result)) : dump_compact(result);
    if (error) out->error = strdup(json_string_value(error));
    if (!out->id || (result && !out->result) || (error && !out->error)) goto fail;
    json_decref(root);
    return 0;
fail:
    json_decref(root);
    nostr_nip46_response_free(out);
    return -1;
}

void nostr_nip46_response_free(NostrNip46Response *res) {
    if (!res) return;
    free(res->id); free(res->result); free(res->error);
    memset(res, 0, sizeof(*res));
}
