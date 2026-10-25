/* SPDX-License-Identifier: MIT */
#include "internal.h"
/* Unknown NIP-11 extensions are skipped lexically, not found by substring.
 * Critical field names remain unescaped and unique; ambiguous docs fail closed. */
static int hex(char c)
{
    return (c >= '0' && c <= '9')
               ? c - '0'
               : ((c >= 'a' && c <= 'f') ? c - 'a' + 10
                                         : ((c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1));
}
static int unicode(vj_reader *r, unsigned *out)
{
    if (r->end - r->p < 4)
        return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
        int x = hex(*r->p++);
        if (x < 0)
            return -1;
        v = v * 16 + (unsigned)x;
    }
    *out = v;
    return 0;
}
static int string_skip(vj_reader *r)
{
    if (vj_take(r, '"'))
        return -1;
    while (r->p < r->end) {
        unsigned c = (unsigned char)*r->p++;
        if (c == '"')
            return 0;
        if (c < 0x20)
            return -1;
        if (c == '\\') {
            if (r->p == r->end)
                return -1;
            c = (unsigned char)*r->p++;
            if (c == 'u') {
                unsigned u;
                if (unicode(r, &u))
                    return -1;
                if (u >= 0xdc00 && u <= 0xdfff)
                    return -1;
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (r->end - r->p < 6 || r->p[0] != '\\' || r->p[1] != 'u')
                        return -1;
                    r->p += 2;
                    if (unicode(r, &u) || u < 0xdc00 || u > 0xdfff)
                        return -1;
                }
            } else if (!strchr("\"\\/bfnrt", (int)c))
                return -1;
        } else if (c >= 0x80) {
            unsigned count, v, min;
            if (c >= 0xc2 && c <= 0xdf) {
                count = 1;
                v = c & 31;
                min = 0x80;
            } else if (c >= 0xe0 && c <= 0xef) {
                count = 2;
                v = c & 15;
                min = 0x800;
            } else if (c >= 0xf0 && c <= 0xf4) {
                count = 3;
                v = c & 7;
                min = 0x10000;
            } else
                return -1;
            if ((size_t)(r->end - r->p) < count)
                return -1;
            while (count--) {
                unsigned x = (unsigned char)*r->p++;
                if ((x & 0xc0) != 0x80)
                    return -1;
                v = (v << 6) | (x & 63);
            }
            if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
                return -1;
        }
    }
    return -1;
}
static int skip(vj_reader *r, unsigned depth)
{
    if (depth > 32)
        return -1;
    vj_ws(r);
    if (r->p == r->end)
        return -1;
    char c = *r->p;
    if (c == '"')
        return string_skip(r);
    if (c == '{' || c == '[') {
        ++r->p;
        char end = c == '{' ? '}' : ']';
        vj_ws(r);
        if (r->p < r->end && *r->p == end) {
            ++r->p;
            return 0;
        }
        for (;;) {
            if (c == '{' && (string_skip(r) || vj_take(r, ':')))
                return -1;
            if (skip(r, depth + 1))
                return -1;
            vj_ws(r);
            if (r->p < r->end && *r->p == end) {
                ++r->p;
                return 0;
            }
            if (vj_take(r, ','))
                return -1;
        }
    }
    const char *literals[] = {"true", "false", "null"};
    for (size_t i = 0; i < 3; ++i) {
        size_t n = strlen(literals[i]);
        if ((size_t)(r->end - r->p) >= n && !memcmp(r->p, literals[i], n)) {
            r->p += n;
            return 0;
        }
    }
    if (c == '-')
        ++r->p;
    if (r->p == r->end)
        return -1;
    if (*r->p == '0')
        ++r->p;
    else {
        if (*r->p < '1' || *r->p > '9')
            return -1;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9')
            ++r->p;
    }
    if (r->p < r->end && *r->p == '.') {
        ++r->p;
        const char *start = r->p;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9')
            ++r->p;
        if (start == r->p)
            return -1;
    }
    if (r->p < r->end && (*r->p == 'e' || *r->p == 'E')) {
        ++r->p;
        if (r->p < r->end && (*r->p == '+' || *r->p == '-'))
            ++r->p;
        const char *start = r->p;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9')
            ++r->p;
        if (start == r->p)
            return -1;
    }
    return 0;
}
static int boolean(vj_reader *r, blossom_capability_state_t *out)
{
    vj_ws(r);
    if (r->end - r->p >= 4 && !memcmp(r->p, "true", 4)) {
        r->p += 4;
        *out = BLOSSOM_CAP_YES;
        return 0;
    }
    if (r->end - r->p >= 5 && !memcmp(r->p, "false", 5)) {
        r->p += 5;
        *out = BLOSSOM_CAP_NO;
        return 0;
    }
    return -1;
}
static int object(vj_reader *r, int limitation, blossom_capability_state_t *vault,
                  blossom_capability_state_t *auth)
{
    if (vj_take(r, '{'))
        return -1;
    unsigned seen = 0;
    vj_ws(r);
    if (r->p < r->end && *r->p == '}') {
        ++r->p;
        return 0;
    }
    for (;;) {
        char name[128];
        if (vj_string(r, name, sizeof(name)) || vj_take(r, ':'))
            return -1;
        unsigned bit = 0;
        if (limitation && !strcmp(name, "auth_required")) {
            bit = 1;
            if (boolean(r, auth))
                return -1;
        } else if (!limitation && !strcmp(name, "vault")) {
            bit = 1;
            if (boolean(r, vault))
                return -1;
        } else if (!limitation && !strcmp(name, "limitation")) {
            bit = 2;
            if (object(r, 1, vault, auth))
                return -1;
        } else if (skip(r, 0))
            return -1;
        if (bit && (seen & bit))
            return -1;
        seen |= bit;
        vj_ws(r);
        if (r->p < r->end && *r->p == '}') {
            ++r->p;
            return 0;
        }
        if (vj_take(r, ','))
            return -1;
    }
}
int vault_vsp_relay_classify(const char *json, blossom_capability_state_t *out)
{
    if (out)
        *out = BLOSSOM_CAP_UNKNOWN;
    if (!out)
        return VAULT_ERR_ARG;
    vj_reader r;
    int rc = vj_init(json, &r);
    if (rc)
        return rc;
    blossom_capability_state_t vault = BLOSSOM_CAP_UNKNOWN, auth = BLOSSOM_CAP_UNKNOWN;
    if (object(&r, 0, &vault, &auth) || vj_end(&r))
        return VAULT_ERR_ARG;
    if (vault == BLOSSOM_CAP_YES && auth == BLOSSOM_CAP_YES)
        *out = BLOSSOM_CAP_YES;
    else if (vault == BLOSSOM_CAP_NO || auth == BLOSSOM_CAP_NO)
        *out = BLOSSOM_CAP_NO;
    return 0;
}
blossom_capability_state_t vault_vsp_blossom_classify(const blossom_server_capabilities_t *c)
{
    return c && c->reachable ? c->private_server : BLOSSOM_CAP_UNKNOWN;
}
blossom_error_t vault_vsp_probe_blossom(blossom_client_t *client, blossom_capability_state_t *out)
{
    if (out)
        *out = BLOSSOM_CAP_UNKNOWN;
    if (!client || !out)
        return BLOSSOM_ERR_INVALID_ARG;
    blossom_error_t rc = blossom_probe_capabilities(client);
    if (!rc)
        *out = vault_vsp_blossom_classify(blossom_client_get_capabilities(client));
    return rc;
}
