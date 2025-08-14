#include "nostr/nip46/nip46_msg.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>

/* Minimal JSON string escaper: returns newly allocated string without quotes.
 * Escapes: \ " \b \f \n \r \t and any char < 0x20 as \u00XX. */
static char *escape_json_string(const char *s) {
    if (!s) return strdup("");
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char*)s; *p; ++p) {
        unsigned char c = *p;
        switch (c) {
            case '"': case '\\': len += 2; break;
            case '\b': case '\f': case '\n': case '\r': case '\t': len += 2; break;
            default:
                if (c < 0x20) len += 6; /* \u00XX */
                else len += 1;
        }
    }
    char *out = (char*)malloc(len + 1);
    if (!out) return NULL;
    char *q = out;
    for (const unsigned char *p = (const unsigned char*)s; *p; ++p) {
        unsigned char c = *p;
        switch (c) {
            case '"': *q++ = '\\'; *q++ = '"'; break;
            case '\\': *q++ = '\\'; *q++ = '\\'; break;
            case '\b': *q++ = '\\'; *q++ = 'b'; break;
            case '\f': *q++ = '\\'; *q++ = 'f'; break;
            case '\n': *q++ = '\\'; *q++ = 'n'; break;
            case '\r': *q++ = '\\'; *q++ = 'r'; break;
            case '\t': *q++ = '\\'; *q++ = 't'; break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    *q++ = '\\'; *q++ = 'u'; *q++ = '0'; *q++ = '0';
                    *q++ = hex[(c >> 4) & 0xF];
                    *q++ = hex[c & 0xF];
                } else {
                    *q++ = (char)c;
                }
        }
    }
    *q = '\0';
    return out;
}

char *nostr_nip46_request_build(const char *id,
                                const char *method,
                                const char *const *params,
                                size_t n_params) {
    if (!id || !method) return NULL;
    char *eid = escape_json_string(id);
    char *emethod = escape_json_string(method);
    if (!eid || !emethod) { free(eid); free(emethod); return NULL; }
    /* Pre-escape params and compute total size */
    char **eparams = NULL;
    if (n_params > 0) {
        eparams = (char**)calloc(n_params, sizeof(char*));
        if (!eparams) { free(eid); free(emethod); return NULL; }
        for (size_t i = 0; i < n_params; ++i) {
            const char *p = (params && params[i]) ? params[i] : "";
            eparams[i] = escape_json_string(p);
            if (!eparams[i]) {
                for (size_t j = 0; j < i; ++j) free(eparams[j]);
                free(eparams); free(eid); free(emethod); return NULL;
            }
        }
    }
    size_t cap = 32 + strlen(eid) + strlen(emethod);
    cap += 2; /* [] */
    for (size_t i = 0; i < n_params; ++i) {
        cap += 2 + strlen(eparams[i]); /* quotes around each param */
        if (i + 1 < n_params) cap += 1; /* comma */
    }
    char *buf = (char*)malloc(cap + 32);
    if (!buf) { if (eparams){ for (size_t i=0;i<n_params;++i) free(eparams[i]); free(eparams);} free(eid); free(emethod); return NULL; }
    char *q = buf;
    q += sprintf(q, "{\"id\":\"%s\",\"method\":\"%s\",\"params\":[", eid, emethod);
    for (size_t i = 0; i < n_params; ++i) {
        if (i) *q++ = ',';
        *q++ = '"';
        size_t l = strlen(eparams[i]); memcpy(q, eparams[i], l); q += l;
        *q++ = '"';
    }
    *q++ = ']'; *q++ = '}'; *q = '\0';
    for (size_t i = 0; i < n_params; ++i) free(eparams ? eparams[i] : NULL);
    free(eparams);
    free(eid); free(emethod);
    return buf;
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
    /* Prefer string forms; if not string, capture compact raw JSON for result. */
    if (nostr_json_get_string(json, "result", &out->result) != 0) {
        (void)nostr_json_get_raw(json, "result", &out->result);
    }
    (void)nostr_json_get_string(json, "error", &out->error);
    return 0;
}

void nostr_nip46_response_free(NostrNip46Response *res) {
    if (!res) return; free(res->id); free(res->result); free(res->error); memset(res,0,sizeof(*res));
}
