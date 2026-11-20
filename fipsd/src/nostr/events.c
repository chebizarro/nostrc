#include "nostr/internal.h"
#include <nostr-json-parse.h>
#include <glib.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool emit(char *out, size_t cap, size_t *used, const char *s, size_t n)
{
    if (n >= cap || *used >= cap - n) return false;
    if (out) memcpy(out + *used, s, n);
    *used += n;
    return true;
}
static bool hex4(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int d = nostr_json_hexval(s[i]);
        if (d < 0) return false;
        v = (v << 4) | (unsigned)d;
    }
    *out = v;
    return true;
}
static bool string_at(const char *text, size_t len, size_t *pos, char *out, size_t cap)
{
    if (!cap || (*pos) >= len || text[(*pos)++] != '"') return false;
    size_t used = 0;
    while ((*pos) < len) {
        char c = text[(*pos)++];
        if (c == '"') { if (out) out[used] = 0; return true; }
        if ((unsigned char)c < 0x20) return false;
        if (c != '\\') { if (!emit(out, cap, &used, &c, 1)) return false; continue; }
        if ((*pos) == len) return false;
        c = text[(*pos)++];
        const char *esc = "\"\\/bfnrt", *val = "\"\\/\b\f\n\r\t";
        const char *found = strchr(esc, c);
        if (found) { c = val[found - esc]; if (!emit(out, cap, &used, &c, 1)) return false; continue; }
        if (c != 'u' || len - (*pos) < 4) return false;
        uint32_t cp;
        if (!hex4(text + (*pos), &cp)) return false;
        (*pos) += 4;
        if (cp >= 0xd800 && cp <= 0xdbff) {
            uint32_t low;
            if (len - (*pos) < 6 || text[(*pos)] != '\\' || text[(*pos) + 1] != 'u' ||
                !hex4(text + (*pos) + 2, &low) || low < 0xdc00 || low > 0xdfff) return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            (*pos) += 6;
        } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
        if (!cp) return false; /* NUL never aliases a C field or key. */
        char utf8[4];
        int n = nostr_json_utf8_encode(cp, utf8);
        if (n <= 0 || !emit(out, cap, &used, utf8, (size_t)n)) return false;
    }
    return false;
}
static void ws(fn_json *j)
{
    while (j->pos < j->len && strchr(" \n\r\t", j->text[j->pos])) ++j->pos;
}
static unsigned value(fn_json *j, unsigned depth)
{
    ws(j);
    if (depth > 8 || j->used + 1 >= FN_TOKENS || j->pos == j->len) return 0;
    unsigned i = ++j->used;
    fn_token *t = &j->t[i];
    t->start = j->pos; t->type = j->text[j->pos]; t->count = 0;
    if (t->type == '"') {
        if (!string_at(j->text, j->len, &j->pos, NULL, FIPSD_NOSTR_EVENT_MAX + 1)) return 0;
    } else if (t->type == '{' || t->type == '[') {
        char end = t->type == '{' ? '}' : ']';
        ++j->pos; ws(j);
        if (j->pos == j->len) return 0;
        if (j->text[j->pos] != end) for (;;) {
            if (++t->count > 64) return 0;
            if (t->type == '{') {
                if (j->text[j->pos] != '"') return 0;
                unsigned key = value(j, depth + 1);
                if (!key) return 0;
                char name[129];
                if (!fn_string(j, key, name, sizeof(name))) return 0;
                /* Reject duplicate keys, including escaped aliases. */
                for (unsigned k = i + 1; k < key; k = j->t[j->t[k].next].next)
                    if (fn_eq(j, k, name)) return 0;
                ws(j);
                if (j->pos == j->len || j->text[j->pos++] != ':') return 0;
            }
            if (!value(j, depth + 1)) return 0;
            ws(j);
            if (j->pos == j->len) return 0;
            if (j->text[j->pos] == end) break;
            if (j->text[j->pos++] != ',') return 0;
            ws(j);
            if (j->pos == j->len) return 0;
        }
        ++j->pos;
    } else {
        size_t start = j->pos;
        while (j->pos < j->len && !strchr(",]} \n\t\r", j->text[j->pos])) ++j->pos;
        size_t n = j->pos - start;
        const char *s = j->text + start;
        if ((n == 4 && (!memcmp(s, "true", 4) || !memcmp(s, "null", 4))) ||
            (n == 5 && !memcmp(s, "false", 5))) { /* literal */ }
        else {
            size_t k = 0;
            if (k < n && s[k] == '-') ++k;
            if (k == n) return 0;
            if (s[k] == '0') ++k;
            else { if (s[k] < '1' || s[k] > '9') return 0;
                while (k < n && s[k] >= '0' && s[k] <= '9') ++k; }
            if (k < n && s[k] == '.') { size_t a = ++k;
                while (k < n && s[k] >= '0' && s[k] <= '9') ++k;
                if (a == k) return 0; }
            if (k < n && (s[k] == 'e' || s[k] == 'E')) { ++k;
                if (k < n && (s[k] == '+' || s[k] == '-')) ++k;
                size_t a = k; while (k < n && s[k] >= '0' && s[k] <= '9') ++k;
                if (a == k) return 0; }
            if (k != n) return 0;
        }
    }
    t->len = j->pos - t->start; t->next = j->used + 1;
    return i;
}
bool fn_parse(fn_json *j, const char *text, size_t len, size_t max)
{
    if (!text || !len || len > max || memchr(text, 0, len) || !g_utf8_validate(text, (gssize)len, NULL)) return false;
    j->text = text; j->len = len; j->pos = 0; j->used = 0;
    memset(&j->t[0], 0, sizeof(j->t[0]));
    if (!value(j, 0)) return false;
    ws(j);
    return j->pos == len && j->t[1].type == '{';
}
bool fn_string(const fn_json *j, unsigned tok, char *out, size_t cap)
{
    if (!tok || tok > j->used || j->t[tok].type != '"') return false;
    size_t pos = j->t[tok].start;
    return string_at(j->text, j->len, &pos, out, cap);
}
bool fn_eq(const fn_json *j, unsigned tok, const char *s)
{
    char text[129];
    return fn_string(j, tok, text, sizeof(text)) && !strcmp(text, s);
}
unsigned fn_field(const fn_json *j, unsigned obj, const char *name)
{
    if (!obj || j->t[obj].type != '{') return 0;
    for (unsigned k = obj + 1; k < j->t[obj].next; k = j->t[j->t[k].next].next)
        if (fn_eq(j, k, name)) return k + 1;
    return 0;
}
bool fn_u64(const fn_json *j, unsigned tok, uint64_t *out)
{
    if (!tok || j->t[tok].type < '0' || j->t[tok].type > '9') return false;
    uint64_t v = 0;
    for (size_t k = 0; k < j->t[tok].len; ++k) {
        unsigned d = (unsigned)(j->text[j->t[tok].start + k] - '0');
        if (d > 9 || v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v; return true;
}
bool fn_bool(const fn_json *j, unsigned tok, bool *out)
{
    if (!tok || (j->t[tok].type != 't' && j->t[tok].type != 'f')) return false;
    *out = j->t[tok].type == 't'; return true;
}
bool fn_null(const fn_json *j, unsigned tok) { return !tok || j->t[tok].type == 'n'; }
uint64_t fn_add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
uint64_t fn_mul1000(uint64_t a) { return a > UINT64_MAX / 1000 ? UINT64_MAX : a * 1000; }
const char *fn_tag(const NostrEvent *e, const char *key)
{
    for (size_t i = 0; e->tags && i < e->tags->count; ++i) {
        const NostrTag *t = e->tags->data[i];
        const char *k = nostr_tag_get(t, 0);
        if (k && !strcmp(k, key)) return nostr_tag_get(t, 1);
    }
    return NULL;
}
bool fn_tags_bounded(const NostrEvent *e)
{
    if (!e || !e->tags || e->tags->count > 64 || !e->content ||
        strnlen(e->content, FIPSD_NOSTR_EVENT_MAX + 1) > FIPSD_NOSTR_EVENT_MAX) return false;
    for (size_t i = 0; i < e->tags->count; ++i) {
        const NostrTag *t = e->tags->data[i];
        if (!t || nostr_tag_size(t) > 8) return false;
        for (size_t k = 0; k < nostr_tag_size(t); ++k) {
            const char *s = nostr_tag_get(t, k);
            if (!s || strnlen(s, FIPSD_NOSTR_URL_MAX + 1) > FIPSD_NOSTR_URL_MAX) return false;
        }
    }
    return true;
}
void fn_secret(const fips_identity *id, char hex[65])
{
    uint8_t secret[32]; fips_identity_export_secret(id, secret);
    sodium_bin2hex(hex, 65, secret, sizeof(secret)); sodium_memzero(secret, sizeof(secret));
}
fipsd_nostr_status fn_sign(NostrEvent *e, const fips_identity *id)
{
    char sk[65]; fn_secret(id, sk); int rc = nostr_event_sign(e, sk); sodium_memzero(sk, sizeof(sk));
    return rc ? FIPSD_NOSTR_CRYPTO : FIPSD_NOSTR_OK;
}
static bool hex_field(const fn_json *j, const char *field, size_t digits)
{
    char text[129];
    if (!fn_string(j, fn_field(j, 1, field), text, sizeof(text)) || strlen(text) != digits) return false;
    for (size_t i = 0; i < digits; ++i) if (nostr_json_hexval(text[i]) < 0) return false;
    return true;
}
bool fn_event_shape(const fn_json *j, bool signed_event)
{
    uint64_t at, kind;
    unsigned tags = fn_field(j, 1, "tags");
    if (!hex_field(j, "id", 64) || !hex_field(j, "pubkey", 64) ||
        (signed_event ? !hex_field(j, "sig", 128) : fn_field(j, 1, "sig") != 0) ||
        !fn_u64(j, fn_field(j, 1, "created_at"), &at) || at > INT64_MAX ||
        !fn_u64(j, fn_field(j, 1, "kind"), &kind) || kind > UINT16_MAX ||
        !tags || j->t[tags].type != '[' || j->t[tags].count > 64 ||
        !fn_string(j, fn_field(j, 1, "content"), NULL, 32769)) return false;
    for (unsigned i = tags + 1; i < j->t[tags].next; i = j->t[i].next) {
        if (j->t[i].type != '[' || j->t[i].count > 8) return false;
        for (unsigned k = i + 1; k < j->t[i].next; k = j->t[k].next)
            if (!fn_string(j, k, NULL, FIPSD_NOSTR_URL_MAX + 1)) return false;
    }
    return true;
}
fipsd_nostr_status fipsd_nostr_event_parse(const char *text, size_t len, NostrEvent **out)
{
    if (!out || len > FIPSD_NOSTR_EVENT_MAX) return FIPSD_NOSTR_LIMIT;
    fn_json j;
    if (!fn_parse(&j, text, len, FIPSD_NOSTR_EVENT_MAX) || !fn_event_shape(&j, true)) return FIPSD_NOSTR_INVALID;
    uint64_t kind;
    if (!fn_u64(&j, fn_field(&j, 1, "kind"), &kind) ||
        (kind != NOSTR_KIND_FIPS_OVERLAY_ADVERT && kind != NOSTR_KIND_FIPS_TRAVERSAL && kind != NOSTR_KIND_DM_RELAY_LIST)) return FIPSD_NOSTR_INVALID;
    if (kind == NOSTR_KIND_FIPS_OVERLAY_ADVERT &&
        !fn_string(&j, fn_field(&j, 1, "content"), NULL, FIPSD_NOSTR_BODY_MAX + 1)) return FIPSD_NOSTR_LIMIT;
    char *copy = malloc(len + 1);
    if (!copy) return FIPSD_NOSTR_LIMIT;
    memcpy(copy, text, len); copy[len] = 0;
    NostrEvent *e = nostr_event_new();
    bool valid = e && nostr_event_deserialize_signed(e, copy, NULL) == NOSTR_EVENT_VALIDATION_OK;
    free(copy);
    if (!valid || !fn_tags_bounded(e) || nostr_event_validate(e, NULL) != NOSTR_EVENT_VALIDATION_OK) {
        nostr_event_free(e); return FIPSD_NOSTR_CRYPTO;
    }
    *out = e; return FIPSD_NOSTR_OK;
}
bool fipsd_nostr_event_id(const NostrEvent *e, uint8_t id[32])
{
    uint8_t tmp[32]; size_t n;
    if (!fn_tags_bounded(e) || nostr_event_validate(e, NULL) != NOSTR_EVENT_VALIDATION_OK ||
        sodium_hex2bin(tmp, 32, e->id, 64, NULL, &n, NULL) || n != 32) return false;
    memcpy(id, tmp, 32); return true;
}
bool fipsd_nostr_seen_contains(const fipsd_nostr_seen *s, const uint8_t id[32])
{
    if (!s || !id || s->count > s->capacity || (s->count && !s->ids)) return false;
    for (size_t i = 0; i < s->count; ++i) if (!memcmp(s->ids[i], id, 32)) return true;
    return false;
}
bool fipsd_nostr_seen_insert(fipsd_nostr_seen *s, const uint8_t id[32])
{
    if (!s || !s->ids || !id || s->count >= s->capacity || fipsd_nostr_seen_contains(s, id)) return false;
    memcpy(s->ids[s->count++], id, 32); return true;
}
void fn_raw(fn_writer *w, const char *s)
{
    if (!w->ok || !emit(w->data, sizeof(w->data), &w->len, s, strlen(s))) w->ok = false;
}
void fn_quote(fn_writer *w, const char *s, size_t cap)
{
    if (!s || strnlen(s, cap) == cap) { w->ok = false; return; }
    json_t *v = json_string(s);
    char *q = v ? json_dumps(v, JSON_ENCODE_ANY | JSON_COMPACT) : NULL;
    json_decref(v);
    if (!q) w->ok = false; else { fn_raw(w, q); free(q); }
}
void fn_uint(fn_writer *w, uint64_t n)
{
    char buf[21]; snprintf(buf, sizeof(buf), "%llu", (unsigned long long)n); fn_raw(w, buf);
}
char *fn_finish(fn_writer *w)
{
    if (!w->ok) return NULL;
    w->data[w->len] = 0;
    return strdup(w->data);
}
char *fipsd_nostr_event_json(const NostrEvent *e, bool signature)
{
    if (!fn_tags_bounded(e)) return NULL;
    char *tags = nostr_tags_to_json(e->tags);
    json_t *a = tags ? json_loads(tags, 0, NULL) : NULL;
    free(tags);
    if (!a) return NULL;
    char *id = nostr_event_get_id((NostrEvent *)e);
    json_t *v = json_object();
    int rc = json_object_set_new(v, "id", json_string(id ? id : ""));
    rc |= json_object_set_new(v, "pubkey", json_string(e->pubkey));
    rc |= json_object_set_new(v, "created_at", json_integer(e->created_at));
    rc |= json_object_set_new(v, "kind", json_integer(e->kind));
    rc |= json_object_set_new(v, "tags", a);
    rc |= json_object_set_new(v, "content", json_string(e->content));
    if (signature) rc |= json_object_set_new(v, "sig", json_string(e->sig));
    free(id);
    char *s = rc ? NULL : json_dumps(v, JSON_COMPACT | JSON_ENCODE_ANY);
    json_decref(v); return s;
}
bool fn_relay_normalize(const char *s, char out[FIPSD_NOSTR_URL_MAX + 1])
{
    if (!s || strnlen(s, FIPSD_NOSTR_URL_MAX + 1) > FIPSD_NOSTR_URL_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (*p <= 0x20 || *p == 0x7f) return false;
    GError *error = NULL;
    GUri *uri = g_uri_parse(s, G_URI_FLAGS_ENCODED, &error);
    bool ok = uri && g_uri_get_scheme(uri) && g_uri_get_host(uri) && *g_uri_get_host(uri) &&
        (!g_ascii_strcasecmp(g_uri_get_scheme(uri), "wss") || !g_ascii_strcasecmp(g_uri_get_scheme(uri), "ws")) &&
        !g_uri_get_userinfo(uri) && !g_uri_get_fragment(uri);
    char *canonical = NULL;
    if (ok) {
        char *scheme = g_ascii_strdown(g_uri_get_scheme(uri), -1);
        char *host = g_ascii_strdown(g_uri_get_host(uri), -1);
        int port = g_uri_get_port(uri);
        if ((port == 443 && !strcmp(scheme, "wss")) || (port == 80 && !strcmp(scheme, "ws"))) port = -1;
        const char *path = g_uri_get_path(uri);
        canonical = g_uri_join(G_URI_FLAGS_ENCODED, scheme, NULL, host, port,
            path ? path : "", g_uri_get_query(uri), NULL);
        g_free(scheme); g_free(host);
        ok = canonical && strlen(canonical) <= FIPSD_NOSTR_URL_MAX;
        if (ok) memcpy(out, canonical, strlen(canonical) + 1);
    }
    g_free(canonical);
    if (uri) g_uri_unref(uri);
    g_clear_error(&error); return ok;
}
bool fn_relay(const char *s)
{
    char normalized[FIPSD_NOSTR_URL_MAX + 1];
    return fn_relay_normalize(s, normalized);
}
