#include "nostr/nip46/nip46_msg.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

/* Split a top-level JSON array into raw element substrings (no unquoting).
 * Assumes input starts with '[' and is a valid JSON array or at least
 * balanced enough for bracket/brace and string tracking.
 * Returns 0 on success; on success, '*out_items' is a newly allocated vector
 * of newly allocated strings for each element; '*out_n' is set to count.
 */
static int json_array_split_raw(const char *raw_array,
                                char ***out_items,
                                size_t *out_n) {
    if (out_items) *out_items = NULL; if (out_n) *out_n = 0;
    if (!raw_array || raw_array[0] != '[' || !out_items || !out_n) return -1;
    const char *p = raw_array;
    // Skip leading '['
    p++;
    // Skip whitespace
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == ']') { // empty array
        *out_items = (char**)calloc(0, sizeof(char*));
        if (!*out_items) return -1; *out_n = 0; return 0;
    }
    size_t cap = 4, count = 0;
    char **items = (char**)malloc(cap * sizeof(char*));
    if (!items) return -1;
    int depth_obj = 0, depth_arr = 0; int in_str = 0; int esc = 0;
    const char *elem_start = p;
    for (; *p; ++p) {
        char c = *p;
        if (in_str) {
            if (esc) { esc = 0; continue; }
            if (c == '\\') { esc = 1; continue; }
            if (c == '"') { in_str = 0; continue; }
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '{') { depth_obj++; continue; }
        if (c == '}') { if (depth_obj>0) depth_obj--; continue; }
        if (c == '[') { depth_arr++; continue; }
        if (c == ']') {
            if (depth_arr == 0 && depth_obj == 0) {
                // End of the top-level array; capture last element
                const char *elem_end = p; // exclusive
                // Trim trailing whitespace from elem_end back
                const char *q = elem_end; while (q>elem_start && isspace((unsigned char)q[-1])) q--;
                size_t len = (size_t)(q - elem_start);
                if (len > 0) {
                    char *s = (char*)malloc(len + 1);
                    if (!s) { for (size_t i=0;i<count;++i) free(items[i]); free(items); return -1; }
                    memcpy(s, elem_start, len); s[len] = '\0';
                    if (count == cap) { cap *= 2; char **tmp = (char**)realloc(items, cap*sizeof(char*)); if(!tmp){ for(size_t i=0;i<count;++i) free(items[i]); free(items); free(s); return -1; } items = tmp; }
                    items[count++] = s;
                }
                // Success
                *out_items = items; *out_n = count; return 0;
            } else {
                if (depth_arr>0) depth_arr--; continue;
            }
        }
        if (c == ',' && depth_obj == 0 && depth_arr == 0) {
            // End of element
            const char *elem_end = p; // exclusive
            // Trim trailing whitespace
            const char *q = elem_end; while (q>elem_start && isspace((unsigned char)q[-1])) q--;
            size_t len = (size_t)(q - elem_start);
            char *s = (char*)malloc(len + 1);
            if (!s) { for (size_t i=0;i<count;++i) free(items[i]); free(items); return -1; }
            memcpy(s, elem_start, len); s[len] = '\0';
            if (count == cap) { cap *= 2; char **tmp = (char**)realloc(items, cap*sizeof(char*)); if(!tmp){ for(size_t i=0;i<count;++i) free(items[i]); free(items); free(s); return -1; } items = tmp; }
            items[count++] = s;
            // Move start to next non-space after comma
            p++; while (*p && isspace((unsigned char)*p)) p++;
            elem_start = p; if (!*p) break; // malformed
            p--; // will be incremented by loop
            continue;
        }
    }
    // Malformed array
    for (size_t i=0;i<count;++i) free(items[i]);
    free(items);
    return -1;
}

char *nostr_nip46_request_build(const char *id,
                                const char *method,
                                const char *const *params,
                                size_t n_params) {
    if (!id || !method) return NULL;
    char *eid = escape_json_string(id);
    char *emethod = escape_json_string(method);
    if (!eid || !emethod) { free(eid); free(emethod); return NULL; }
    /* Prepare params: embed raw JSON (objects/arrays) without quoting; otherwise JSON-stringify */
    char **eparams = NULL; /* escaped strings for non-raw */
    int *is_raw = NULL;
    size_t *plen = NULL; /* computed length contribution for each param in output */
    if (n_params > 0) {
        eparams = (char**)calloc(n_params, sizeof(char*));
        is_raw = (int*)calloc(n_params, sizeof(int));
        plen = (size_t*)calloc(n_params, sizeof(size_t));
        if (!eparams || !is_raw || !plen) { free(eparams); free(is_raw); free(plen); free(eid); free(emethod); return NULL; }
        for (size_t i = 0; i < n_params; ++i) {
            const char *p = (params && params[i]) ? params[i] : "";
            /* Trim leading whitespace to detect JSON object/array */
            const char *t = p; while (*t && isspace((unsigned char)*t)) ++t;
            if (*t == '{' || *t == '[') {
                is_raw[i] = 1;
                eparams[i] = NULL;
                plen[i] = strlen(p); /* as-is, no surrounding quotes */
            } else {
                is_raw[i] = 0;
                eparams[i] = escape_json_string(p);
                if (!eparams[i]) {
                    for (size_t j = 0; j < i; ++j) free(eparams[j]);
                    free(eparams); free(is_raw); free(plen); free(eid); free(emethod); return NULL;
                }
                plen[i] = 2 + strlen(eparams[i]); /* quotes around escaped */
            }
        }
    }
    size_t cap = 32 + strlen(eid) + strlen(emethod);
    cap += 2; /* [] */
    for (size_t i = 0; i < n_params; ++i) {
        cap += plen[i];
        if (i + 1 < n_params) cap += 1; /* comma */
    }
    char *buf = (char*)malloc(cap + 32);
    if (!buf) { if (eparams){ for (size_t i=0;i<n_params;++i) free(eparams[i]); free(eparams);} free(is_raw); free(plen); free(eid); free(emethod); return NULL; }
    char *q = buf;
    q += sprintf(q, "{\"id\":\"%s\",\"method\":\"%s\",\"params\":[", eid, emethod);
    for (size_t i = 0; i < n_params; ++i) {
        if (i) *q++ = ',';
        if (is_raw[i]) {
            size_t l = strlen(params[i]); memcpy(q, params[i], l); q += l;
        } else {
            *q++ = '"';
            size_t l = strlen(eparams[i]); memcpy(q, eparams[i], l); q += l;
            *q++ = '"';
        }
    }
    *q++ = ']'; *q++ = '}'; *q = '\0';
    for (size_t i = 0; i < n_params; ++i) free(eparams ? eparams[i] : NULL);
    free(eparams); free(is_raw); free(plen);
    free(eid); free(emethod);
    return buf;
}

int nostr_nip46_request_parse(const char *json, NostrNip46Request *out) {
    if (!json || !out) return -1; memset(out, 0, sizeof(*out));
    if (nostr_json_get_string(json, "id", &out->id) != 0) return -1;
    if (nostr_json_get_string(json, "method", &out->method) != 0) return -1;
    // First try to parse as array of strings (legacy behavior)
    if (nostr_json_get_string_array(json, "params", &out->params, &out->n_params) != 0 || out->n_params == 0) {
        // Fallback: get raw params array and split into raw elements
        char *raw = NULL;
        if (nostr_json_get_raw(json, "params", &raw) == 0 && raw) {
            // raw is the compact JSON for the array (e.g., "[ {..}, 123, \"x\" ]")
            // Split top-level elements
            char **items = NULL; size_t n = 0;
            if (json_array_split_raw(raw, &items, &n) == 0) {
                out->params = items;
                out->n_params = n;
            }
            free(raw);
        }
    }
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
    /* +1 for NUL terminator to avoid truncation by snprintf */
    size_t cap = strlen(result_json) + strlen(id) + 32 + 1;
    char *buf = (char*)malloc(cap);
    if (!buf) return NULL;
    snprintf(buf, cap, "{\"id\":\"%s\",\"result\":%s}", id, result_json);
    return buf;
}

char *nostr_nip46_response_build_err(const char *id, const char *error_msg) {
    if (!id || !error_msg) return NULL;
    /* +1 for NUL terminator to avoid truncation by snprintf */
    size_t cap = strlen(error_msg) + strlen(id) + 32 + 1;
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
