#include "nostr-envelope.h"
#include "nostr-event.h"
#include "json.h"
#include "security_limits_runtime.h"
#include "nostr_log.h"
#include "nostr/metrics.h"
#include <string.h>
#include <stdlib.h>

// Helper function to create a new Envelope
NostrEnvelope *create_envelope(NostrEnvelopeType type) {
    /* Zero-initialize so free paths are always safe on early-parse errors */
    NostrEnvelope *envelope = (NostrEnvelope *)calloc(1, sizeof(NostrEnvelope));
    if (!envelope)
        return NULL;
    envelope->type = type;
    return envelope;
}

/* === Compact fast-path serializer === */
static char *json_escape_string_min(const char *s) {
    if (!s) return NULL;
    // First pass: compute required size with escaping
    size_t needed = 2; // quotes
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"': case '\\': case '\b': case '\f': case '\n': case '\r': case '\t':
            needed += 2; // \x
            break;
        default:
            if (c < 0x20) {
                needed += 6; // \u00XX
            } else {
                needed += 1;
            }
        }
    }
    char *out = (char *)malloc(needed + 1);
    if (!out) return NULL;
    char *w = out;
    *w++ = '"';
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"': *w++ = '\\'; *w++ = '"'; break;
        case '\\': *w++ = '\\'; *w++ = '\\'; break;
        case '\b': *w++ = '\\'; *w++ = 'b'; break;
        case '\f': *w++ = '\\'; *w++ = 'f'; break;
        case '\n': *w++ = '\\'; *w++ = 'n'; break;
        case '\r': *w++ = '\\'; *w++ = 'r'; break;
        case '\t': *w++ = '\\'; *w++ = 't'; break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                *w++ = '\\'; *w++ = 'u'; *w++ = '0'; *w++ = '0';
                *w++ = hex[(c >> 4) & 0xF];
                *w++ = hex[c & 0xF];
            } else {
                *w++ = (char)c;
            }
        }
    }
    *w++ = '"';
    *w = '\0';
    return out;
}

char *nostr_envelope_serialize_compact(const NostrEnvelope *base) {
    if (!base) return NULL;
    switch (base->type) {
    case NOSTR_ENVELOPE_EVENT: {
        const NostrEventEnvelope *env = (const NostrEventEnvelope *)base;
        if (!env->event) return NULL;
        char *ev = nostr_event_serialize(env->event);
        if (!ev) return NULL;
        char *sid = NULL;
        if (env->subscription_id) sid = json_escape_string_min(env->subscription_id);
        // Compute size: ["EVENT", ("sid",)? ev ]
        const char *prefix = "[\"EVENT\",";
        const char *prefix_no_sid = "[\"EVENT\",";
        size_t sz = 0;
        if (sid) {
            sz = strlen(prefix) + strlen(sid) + 1 /* comma */ + strlen(ev) + 1/*]*/ + 1;
        } else {
            // no sid form: ["EVENT",ev]
            sz = strlen(prefix_no_sid) + strlen(ev) + 1/*]*/ + 1;
        }
        char *out = (char *)malloc(sz);
        if (!out) { free(ev); free(sid); return NULL; }
        if (sid) {
            // ["EVENT",SID,EV]
            snprintf(out, sz, "[\"EVENT\",%s,%s]", sid, ev);
        } else {
            snprintf(out, sz, "[\"EVENT\",%s]", ev);
        }
        free(ev);
        free(sid);
        return out;
    }
    case NOSTR_ENVELOPE_OK: {
        const NostrOKEnvelope *env = (const NostrOKEnvelope *)base;
        if (!env->event_id) return NULL;
        char *eid = json_escape_string_min(env->event_id);
        if (!eid) return NULL;
        const char *booltxt = env->ok ? "true" : "false";
        char *rsn = NULL;
        size_t sz = 0;
        if (env->reason) {
            rsn = json_escape_string_min(env->reason);
            if (!rsn) { free(eid); return NULL; }
            // ["OK",eid,bool,rsn]
            sz = 6 + strlen(eid) + 1 + strlen(booltxt) + 1 + strlen(rsn) + 1; // rough
        } else {
            // ["OK",eid,bool]
            sz = 6 + strlen(eid) + 1 + strlen(booltxt) + 1; // rough
        }
        sz += 16; // padding for brackets/commas/quotes
        char *out = (char *)malloc(sz);
        if (!out) { free(eid); free(rsn); return NULL; }
        if (rsn)
            snprintf(out, sz, "[\"OK\",%s,%s,%s]", eid, booltxt, rsn);
        else
            snprintf(out, sz, "[\"OK\",%s,%s]", eid, booltxt);
        free(eid);
        free(rsn);
        return out;
    }
    case NOSTR_ENVELOPE_NOTICE: {
        const NostrNoticeEnvelope *env = (const NostrNoticeEnvelope *)base;
        if (!env->message) return NULL;
        char *msg = json_escape_string_min(env->message);
        if (!msg) return NULL;
        size_t sz = 12 + strlen(msg) + 1;
        char *out = (char *)malloc(sz);
        if (!out) { free(msg); return NULL; }
        snprintf(out, sz, "[\"NOTICE\",%s]", msg);
        free(msg);
        return out;
    }
    case NOSTR_ENVELOPE_EOSE: {
        const NostrEOSEEnvelope *env = (const NostrEOSEEnvelope *)base;
        if (!env->message) return NULL;
        char *msg = json_escape_string_min(env->message);
        if (!msg) return NULL;
        size_t sz = 10 + strlen(msg) + 1;
        char *out = (char *)malloc(sz);
        if (!out) { free(msg); return NULL; }
        snprintf(out, sz, "[\"EOSE\",%s]", msg);
        free(msg);
        return out;
    }
    case NOSTR_ENVELOPE_CLOSE: {
        const NostrCloseEnvelope *env = (const NostrCloseEnvelope *)base;
        if (!env->message) return NULL;
        char *sid = json_escape_string_min(env->message);
        if (!sid) return NULL;
        size_t sz = 10 + strlen(sid) + 1;
        char *out = (char *)malloc(sz);
        if (!out) { free(sid); return NULL; }
        snprintf(out, sz, "[\"CLOSE\",%s]", sid);
        free(sid);
        return out;
    }
    case NOSTR_ENVELOPE_CLOSED: {
        const NostrClosedEnvelope *env = (const NostrClosedEnvelope *)base;
        if (!env->subscription_id || !env->reason) return NULL;
        char *sid = json_escape_string_min(env->subscription_id);
        char *rsn = sid ? json_escape_string_min(env->reason) : NULL;
        if (!sid || !rsn) { free(sid); free(rsn); return NULL; }
        size_t sz = 12 + strlen(sid) + 1 + strlen(rsn) + 1;
        char *out = (char *)malloc(sz);
        if (!out) { free(sid); free(rsn); return NULL; }
        snprintf(out, sz, "[\"CLOSED\",%s,%s]", sid, rsn);
        free(sid); free(rsn);
        return out;
    }
    case NOSTR_ENVELOPE_AUTH: {
        const NostrAuthEnvelope *env = (const NostrAuthEnvelope *)base;
        if (env->event) {
            char *ev = nostr_event_serialize(env->event);
            if (!ev) return NULL;
            size_t sz = 10 + strlen(ev) + 1;
            char *out = (char *)malloc(sz);
            if (!out) { free(ev); return NULL; }
            snprintf(out, sz, "[\"AUTH\",%s]", ev);
            free(ev);
            return out;
        } else if (env->challenge) {
            char *ch = json_escape_string_min(env->challenge);
            if (!ch) return NULL;
            size_t sz = 10 + strlen(ch) + 1;
            char *out = (char *)malloc(sz);
            if (!out) { free(ch); return NULL; }
            snprintf(out, sz, "[\"AUTH\",%s]", ch);
            free(ch);
            return out;
        }
        return NULL;
    }
    case NOSTR_ENVELOPE_REQ:
    {
        const NostrReqEnvelope *env = (const NostrReqEnvelope *)base;
        if (!env->subscription_id || !env->filters) return NULL;
        char *sid = json_escape_string_min(env->subscription_id);
        if (!sid) return NULL;
        // Serialize each filter
        size_t total = 10 + strlen(sid) + 1; // header approx
        char **parts = NULL; size_t nparts = 0;
        size_t fcount = env->filters ? env->filters->count : 0;
        size_t maxf = (size_t)nostr_limit_max_filters_per_req();
        if (fcount > maxf) {
            nostr_rl_log(NLOG_WARN, "req", "trimming filters: %zu > %zu", fcount, maxf);
            nostr_metric_counter_add("req_filters_trimmed", 1);
            fcount = maxf;
        }
        if (env->filters && fcount > 0) {
            parts = (char **)calloc(fcount, sizeof(char *));
            if (!parts) { free(sid); return NULL; }
            for (size_t i = 0; i < fcount; ++i) {
                char *fj = nostr_filter_serialize_compact(&env->filters->filters[i]);
                if (!fj) { for (size_t j=0;j<i;++j) free(parts[j]); free(parts); free(sid); return NULL; }
                parts[nparts++] = fj;
                total += strlen(fj) + 1; // comma
            }
        }
        total += 16; // brackets and commas
        char *out = (char *)malloc(total);
        if (!out) { for (size_t j=0;j<nparts;++j) free(parts[j]); free(parts); free(sid); return NULL; }
        char *w = out;
        size_t rem = total;
        int n = snprintf(w, rem, "[\"REQ\",%s", sid);
        if (n < 0 || (size_t)n >= rem) { for (size_t j=0;j<nparts;++j) free(parts[j]); free(parts); free(sid); free(out); return NULL; }
        w += n; rem -= (size_t)n;
        for (size_t i = 0; i < nparts; ++i) {
            n = snprintf(w, rem, ",%s", parts[i]);
            if (n < 0 || (size_t)n >= rem) { for (size_t j=0;j<nparts;++j) free(parts[j]); free(parts); free(sid); free(out); return NULL; }
            w += n; rem -= (size_t)n;
        }
        n = snprintf(w, rem, "]");
        if (n < 0 || (size_t)n >= rem) { for (size_t j=0;j<nparts;++j) free(parts[j]); free(parts); free(sid); free(out); return NULL; }
        *w = '\0';
        for (size_t i=0;i<nparts;++i) free(parts[i]);
        free(parts);
        free(sid);
        return out;
    }
    case NOSTR_ENVELOPE_COUNT:
    {
        const NostrCountEnvelope *env = (const NostrCountEnvelope *)base;
        if (!env->subscription_id) return NULL;
        char *sid = json_escape_string_min(env->subscription_id);
        if (!sid) return NULL;
        // Count object
        char countbuf[64];
        snprintf(countbuf, sizeof(countbuf), "{\"count\":%d}", env->count);
        size_t total = 12 + strlen(sid) + strlen(countbuf) + 1;
        char **parts = NULL; size_t nparts = 0;
        size_t fcount = env->filters ? env->filters->count : 0;
        size_t maxf = (size_t)nostr_limit_max_filters_per_req();
        if (fcount > maxf) {
            nostr_rl_log(NLOG_WARN, "count", "trimming filters: %zu > %zu", fcount, maxf);
            nostr_metric_counter_add("count_filters_trimmed", 1);
            fcount = maxf;
        }
        if (env->filters && fcount > 0) {
            parts = (char **)calloc(fcount, sizeof(char *));
            if (!parts) { free(sid); return NULL; }
            for (size_t i = 0; i < fcount; ++i) {
                char *fj = nostr_filter_serialize_compact(&env->filters->filters[i]);
                if (!fj) { for (size_t j=0;j<i;++j) free(parts[j]); free(parts); free(sid); return NULL; }
                parts[nparts++] = fj;
                total += strlen(fj) + 1;
            }
        }
        total += 16;
        char *out = (char *)malloc(total);
        if (!out) { for (size_t j=0;j<nparts;++j) free(parts[j]); free(parts); free(sid); return NULL; }
        char *w = out;
        size_t rem = total;
        int n = snprintf(w, rem, "[\"COUNT\",%s,%s", sid, countbuf);
        if (n < 0 || (size_t)n >= rem) { for (size_t j=0;j<nparts;++j) free(parts[j]); free(parts); free(sid); free(out); return NULL; }
        w += n; rem -= (size_t)n;
        for (size_t i = 0; i < nparts; ++i) {
            n = snprintf(w, rem, ",%s", parts[i]);
            if (n < 0 || (size_t)n >= rem) { for (size_t j=0;j<nparts;++j) free(parts[j]); free(parts); free(sid); free(out); return NULL; }
            w += n; rem -= (size_t)n;
        }
        n = snprintf(w, rem, "]");
        if (n < 0 || (size_t)n >= rem) { for (size_t j=0;j<nparts;++j) free(parts[j]); free(parts); free(sid); free(out); return NULL; }
        *w = '\0';
        for (size_t i=0;i<nparts;++i) free(parts[i]);
        free(parts);
        free(sid);
        return out;
    }
    default:
        return NULL; // force backend fallback for now
    }
}

/* Forward decls for local JSON helpers used below */
static const char *skip_ws(const char *p);
static char *parse_json_string(const char **pp);
static const char *parse_comma(const char *p);
static char *parse_json_object(const char **pp);

static int debug_enabled(void) {
    const char *e = getenv("NOSTR_DEBUG");
    return e && *e && strcmp(e, "0") != 0;
}

/* === Compact fast-path deserializer === */
int nostr_envelope_deserialize_compact(NostrEnvelope *base, const char *json) {
    if (!base || !json) return 0;
    if (debug_enabled()) {
        fprintf(stderr, "[compact] parse envelope: %s\n", json);
    }
    const char *p = skip_ws(json);
    if (*p != '[') return 0;
    ++p; // skip [
    // first: label
    char *label = parse_json_string(&p);
    if (!label) {
        if (debug_enabled()) fprintf(stderr, "[compact] failed to parse label string at: %.32s\n", p);
        return 0;
    }
    const char *q = parse_comma(p);
    if (!q) { if (debug_enabled()) fprintf(stderr, "[compact] missing comma after label '%s' at: %.32s\n", label, p); free(label); return 0; }
    p = q; p = skip_ws(p);

    int ok = 0;
    switch (base->type) {
    case NOSTR_ENVELOPE_EVENT: {
        if (strcmp(label, "EVENT") != 0) break;
        NostrEventEnvelope *env = (NostrEventEnvelope *)base;
        // Optional sub id
        if (*p == '"') {
            env->subscription_id = parse_json_string(&p);
            if (!env->subscription_id) break;
            q = parse_comma(p); if (!q) break; p = skip_ws(q);
        }
        // Next must be event object
        char *event_json = parse_json_object(&p);
        if (!event_json) break;
        NostrEvent *ev = nostr_event_new();
        int succ = nostr_event_deserialize(ev, event_json);
        free(event_json);
        if (succ != 0) { nostr_event_free(ev); break; }
        env->event = ev;
        ok = 1;
        break;
    }
    case NOSTR_ENVELOPE_REQ: {
        if (strcmp(label, "REQ") != 0) break;
        NostrReqEnvelope *env = (NostrReqEnvelope *)base;
        env->subscription_id = parse_json_string(&p);
        if (!env->subscription_id) break;
        // zero or more filter objects
        NostrFilters *filters = nostr_filters_new();
        if (!filters) break;
        size_t maxf = (size_t)nostr_limit_max_filters_per_req();
        size_t added = 0;
        while (1) {
            q = parse_comma(p);
            if (!q) break;
            p = skip_ws(q);
            if (*p != '{') break;
            char *obj = parse_json_object(&p);
            if (!obj) break;
            if (added < maxf) {
                NostrFilter f = {0};
                if (nostr_filter_deserialize_compact(&f, obj)) {
                    (void)nostr_filters_add(filters, &f); // moves and zeros f
                    added++;
                } else {
                    // not a filter object; ignore for REQ
                    nostr_filter_clear(&f);
                }
            } else {
                nostr_rl_log(NLOG_WARN, "req", "trim deserialization: exceeded max filters %zu", maxf);
                nostr_metric_counter_add("req_filters_trimmed", 1);
                // skip remaining objects silently
            }
            free(obj);
        }
        env->filters = filters;
        ok = 1;
        break;
    }
    case NOSTR_ENVELOPE_COUNT: {
        if (strcmp(label, "COUNT") != 0) break;
        NostrCountEnvelope *env = (NostrCountEnvelope *)base;
        env->subscription_id = parse_json_string(&p);
        if (!env->subscription_id) break;
        NostrFilters *filters = nostr_filters_new();
        if (!filters) break;
        env->count = 0;
        size_t maxf = (size_t)nostr_limit_max_filters_per_req();
        size_t added = 0;
        while (1) {
            q = parse_comma(p);
            if (!q) break;
            p = skip_ws(q);
            if (*p != '{') break;
            const char *savep = p;
            char *obj = parse_json_object(&p);
            if (!obj) break;
            // Detect count object strictly: first key must be "count" and value a number
            const char *op = obj;
            op = skip_ws(op);
            if (*op == '{') op++;
            op = skip_ws(op);
            int is_count_obj = 0;
            if (*op == '"') {
                // parse key string
                op++;
                const char *ks = op;
                while (*op && *op != '"') {
                    if (*op == '\\' && *(op+1)) op += 2; else op++;
                }
                size_t klen = (size_t)(op - ks);
                if (*op == '"') op++;
                op = skip_ws(op);
                if (*op == ':') { op++; op = skip_ws(op); }
                if (klen == 5 && strncmp(ks, "count", 5) == 0) {
                    // number may be negative, but count should be >=0; parse int
                    int sign = 1; if (*op == '+') { op++; } else if (*op == '-') { sign = -1; op++; }
                    long val = 0; int any = 0;
                    while (*op >= '0' && *op <= '9') { val = val * 10 + (*op - '0'); op++; any = 1; }
                    if (any) { env->count = (int)(sign * val); is_count_obj = 1; }
                }
            }
            if (!is_count_obj) {
                // treat as filter
                if (added < maxf) {
                    NostrFilter f = {0};
                    if (nostr_filter_deserialize_compact(&f, obj)) {
                        (void)nostr_filters_add(filters, &f);
                        added++;
                    } else {
                        nostr_filter_clear(&f);
                    }
                } else {
                    nostr_rl_log(NLOG_WARN, "count", "trim deserialization: exceeded max filters %zu", maxf);
                    nostr_metric_counter_add("count_filters_trimmed", 1);
                }
            }
            free(obj);
            (void)savep;
        }
        env->filters = filters;
        ok = 1;
        break;
    }
    case NOSTR_ENVELOPE_OK: {
        if (strcmp(label, "OK") != 0) break;
        NostrOKEnvelope *env = (NostrOKEnvelope *)base;
        env->event_id = parse_json_string(&p);
        if (!env->event_id) break;
        q = parse_comma(p); if (!q) { ok = 1; break; }
        p = skip_ws(q);
        if (strncmp(p, "true", 4) == 0) { env->ok = true; p += 4; }
        else if (strncmp(p, "false", 5) == 0) { env->ok = false; p += 5; }
        else break;
        q = parse_comma(p);
        if (q) { p = q; env->reason = parse_json_string(&p); if (!env->reason) break; }
        ok = 1;
        break;
    }
    case NOSTR_ENVELOPE_NOTICE: {
        if (strcmp(label, "NOTICE") != 0) break;
        NostrNoticeEnvelope *env = (NostrNoticeEnvelope *)base;
        env->message = parse_json_string(&p);
        if (!env->message) break;
        ok = 1;
        break;
    }
    case NOSTR_ENVELOPE_EOSE: {
        if (strcmp(label, "EOSE") != 0) break;
        NostrEOSEEnvelope *env = (NostrEOSEEnvelope *)base;
        env->message = parse_json_string(&p);
        if (!env->message) break;
        ok = 1;
        break;
    }
    case NOSTR_ENVELOPE_CLOSED: {
        if (strcmp(label, "CLOSED") != 0) break;
        NostrClosedEnvelope *env = (NostrClosedEnvelope *)base;
        env->subscription_id = parse_json_string(&p);
        if (!env->subscription_id) break;
        q = parse_comma(p); if (!q) break; // reason is required
        p = q;
        env->reason = parse_json_string(&p);
        if (!env->reason) break;
        ok = 1;
        break;
    }
    case NOSTR_ENVELOPE_AUTH: {
        if (strcmp(label, "AUTH") != 0) break;
        NostrAuthEnvelope *env = (NostrAuthEnvelope *)base;
        p = skip_ws(p);
        if (*p == '{') {
            char *ej = parse_json_object(&p);
            if (!ej) break;
            NostrEvent *ev = nostr_event_new();
            int succ = nostr_event_deserialize(ev, ej);
            free(ej);
            if (succ != 0) { nostr_event_free(ev); break; }
            env->event = ev;
            ok = 1;
        } else if (*p == '"') {
            env->challenge = parse_json_string(&p);
            if (!env->challenge) break;
            // optional event after comma
            q = parse_comma(p);
            if (q) {
                p = skip_ws(q);
                if (*p == '{') {
                    char *ej = parse_json_object(&p);
                    if (ej) {
                        NostrEvent *ev = nostr_event_new();
                        int succ = nostr_event_deserialize(ev, ej);
                        free(ej);
                        if (succ == 0) env->event = ev; else nostr_event_free(ev);
                    }
                }
            }
            ok = 1;
        }
        break;
    }
    default:
        if (debug_enabled()) fprintf(stderr, "[compact] unsupported type %d for label '%s'\n", base->type, label);
        break;
    }

    if (!ok && debug_enabled()) {
        // Try to show where we are
        const char *tail = skip_ws(p);
        fprintf(stderr, "[compact] parse failed for type %d after label '%s' near: %.64s\n", base->type, label, tail);
    }
    free(label);
    return ok;
}

// Helpers to parse JSON array framing quickly without full JSON
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
    return p;
}

// Encode a Unicode code point as UTF-8 into out, return bytes written (1-4)
static int utf8_encode(uint32_t cp, char *out) {
    if (cp <= 0x7F) { out[0] = (char)cp; return 1; }
    if (cp <= 0x7FF) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp <= 0xFFFF) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static char *parse_json_string(const char **pp) {
    const char *p = skip_ws(*pp);
    if (*p != '"') return NULL;
    ++p; // after opening quote
    // Allocate a buffer pessimistically equal to remaining input segment length
    size_t cap = 64; size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    while (*p) {
        char c = *p++;
        if (c == '"') { // end of string
            break;
        } else if (c == '\\') { // escape sequence
            char e = *p++;
            char outc;
            switch (e) {
            case '"': outc = '"'; goto emit_one;
            case '\\': outc = '\\'; goto emit_one;
            case '/': outc = '/'; goto emit_one;
            case 'b': outc = '\b'; goto emit_one;
            case 'f': outc = '\f'; goto emit_one;
            case 'n': outc = '\n'; goto emit_one;
            case 'r': outc = '\r'; goto emit_one;
            case 't': outc = '\t'; goto emit_one;
            case 'u': {
                // parse 4 hex digits
                int h1 = hexval(p[0]); int h2 = hexval(p[1]); int h3 = hexval(p[2]); int h4 = hexval(p[3]);
                if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) { free(buf); return NULL; }
                uint32_t cp = (uint32_t)((h1<<12) | (h2<<8) | (h3<<4) | h4);
                p += 4;
                // Handle UTF-16 surrogate pairs \uD800-\uDBFF followed by \uDC00-\uDFFF
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    int h5 = hexval(p[2]); int h6 = hexval(p[3]); int h7 = hexval(p[4]); int h8 = hexval(p[5]);
                    if (h5 >= 0 && h6 >= 0 && h7 >= 0 && h8 >= 0) {
                        uint32_t low = (uint32_t)((h5<<12) | (h6<<8) | (h7<<4) | h8);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            p += 6;
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                        }
                    }
                }
                char tmp[4];
                int n = utf8_encode(cp, tmp);
                if (len + (size_t)n >= cap) { cap *= 2; char *nb = (char *)realloc(buf, cap); if (!nb) { free(buf); return NULL; } buf = nb; }
                for (int i=0;i<n;i++) buf[len++] = tmp[i];
                continue;
            }
            default:
                free(buf); return NULL;
            }
emit_one:
            if (len + 1 >= cap) { cap *= 2; char *nb = (char *)realloc(buf, cap); if (!nb) { free(buf); return NULL; } buf = nb; }
            buf[len++] = outc;
        } else {
            if (len + 1 >= cap) { cap *= 2; char *nb = (char *)realloc(buf, cap); if (!nb) { free(buf); return NULL; } buf = nb; }
            buf[len++] = c;
        }
    }
    // Null-terminate
    if (len + 1 >= cap) { char *nb = (char *)realloc(buf, len + 1); if (!nb) { free(buf); return NULL; } buf = nb; }
    buf[len] = '\0';
    *pp = p; // already after closing quote
    return buf;
}

static const char *parse_comma(const char *p) {
    p = skip_ws(p);
    if (*p != ',') return NULL;
    return p + 1;
}

// Extract a balanced JSON object starting at '{'. Returns malloc'ed substring
// containing exactly the object text (from '{' to matching '}'), and advances
// *pp to just after the object.
static char *parse_json_object(const char **pp) {
    const char *p = skip_ws(*pp);
    if (*p != '{') return NULL;
    int depth = 0;
    const char *start = p;
    while (*p) {
        if (*p == '"') {
            // skip string contents including escaped quotes
            ++p;
            while (*p) {
                if (*p == '\\') { // escape
                    if (*(p+1)) p += 2; else break;
                } else if (*p == '"') { ++p; break; }
                else { ++p; }
            }
            continue;
        }
        if (*p == '{') depth++;
        if (*p == '}') {
            depth--;
            if (depth == 0) {
                size_t len = (size_t)(p - start + 1);
                char *s = (char *)malloc(len + 1);
                if (!s) return NULL;
                memcpy(s, start, len);
                s[len] = '\0';
                *pp = p + 1;
                return s;
            }
        }
        ++p;
    }
    return NULL;
}

// Function to parse a message and return the appropriate Envelope struct
NostrEnvelope *nostr_envelope_parse(const char *message) {
    if (!message) return NULL;
    const char *p = skip_ws(message);
    if (*p != '[') return NULL;
    ++p; // skip [
    // Peek label to decide which envelope to allocate
    char *label = parse_json_string(&p);
    if (!label) return NULL;
    NostrEnvelope *env = NULL;
    if (strcmp(label, "EVENT") == 0) {
        env = (NostrEnvelope *)calloc(1, sizeof(NostrEventEnvelope));
        if (env) env->type = NOSTR_ENVELOPE_EVENT;
    } else if (strcmp(label, "OK") == 0) {
        env = (NostrEnvelope *)calloc(1, sizeof(NostrOKEnvelope));
        if (env) env->type = NOSTR_ENVELOPE_OK;
    } else if (strcmp(label, "NOTICE") == 0) {
        env = (NostrEnvelope *)calloc(1, sizeof(NostrNoticeEnvelope));
        if (env) env->type = NOSTR_ENVELOPE_NOTICE;
    } else if (strcmp(label, "EOSE") == 0) {
        env = (NostrEnvelope *)calloc(1, sizeof(NostrEOSEEnvelope));
        if (env) env->type = NOSTR_ENVELOPE_EOSE;
    } else if (strcmp(label, "CLOSED") == 0) {
        env = (NostrEnvelope *)calloc(1, sizeof(NostrClosedEnvelope));
        if (env) env->type = NOSTR_ENVELOPE_CLOSED;
    } else if (strcmp(label, "AUTH") == 0) {
        env = (NostrEnvelope *)calloc(1, sizeof(NostrAuthEnvelope));
        if (env) env->type = NOSTR_ENVELOPE_AUTH;
    } else if (strcmp(label, "REQ") == 0) {
        env = (NostrEnvelope *)calloc(1, sizeof(NostrReqEnvelope));
        if (env) env->type = NOSTR_ENVELOPE_REQ;
    } else if (strcmp(label, "COUNT") == 0) {
        env = (NostrEnvelope *)calloc(1, sizeof(NostrCountEnvelope));
        if (env) env->type = NOSTR_ENVELOPE_COUNT;
    }
    free(label);
    if (!env) return NULL;
    // Delegate parsing to unified deserializer (compact fast-path + backend fallback)
    if (nostr_envelope_deserialize(env, message) != 0) {
        nostr_envelope_free(env);
        return NULL;
    }
    return env;
}

// Function to free an Envelope struct
void nostr_envelope_free(NostrEnvelope *envelope) {
    if (!envelope)
        return;

    switch (envelope->type) {
    case NOSTR_ENVELOPE_EVENT:
        free(((NostrEventEnvelope *)envelope)->subscription_id);
        nostr_event_free(((NostrEventEnvelope *)envelope)->event);
        break;
    case NOSTR_ENVELOPE_REQ:
        free(((NostrReqEnvelope *)envelope)->subscription_id);
        nostr_filters_free(((NostrReqEnvelope *)envelope)->filters);
        break;
    case NOSTR_ENVELOPE_COUNT:
        free(((NostrCountEnvelope *)envelope)->subscription_id);
        nostr_filters_free(((NostrCountEnvelope *)envelope)->filters);
        break;
    case NOSTR_ENVELOPE_NOTICE:
        free(((NostrNoticeEnvelope *)envelope)->message);
        break;
    case NOSTR_ENVELOPE_EOSE:
        free(((NostrEOSEEnvelope *)envelope)->message);
        break;
    case NOSTR_ENVELOPE_CLOSE:
        free(((NostrCloseEnvelope *)envelope)->message);
        break;
    case NOSTR_ENVELOPE_CLOSED:
        free(((NostrClosedEnvelope *)envelope)->subscription_id);
        free(((NostrClosedEnvelope *)envelope)->reason);
        break;
    case NOSTR_ENVELOPE_OK:
        free(((NostrOKEnvelope *)envelope)->event_id);
        free(((NostrOKEnvelope *)envelope)->reason);
        break;
    case NOSTR_ENVELOPE_AUTH:
        free(((NostrAuthEnvelope *)envelope)->challenge);
        nostr_event_free(((NostrAuthEnvelope *)envelope)->event);
        break;
    default:
        break;
    }

    free(envelope);
}

int event_envelope_unmarshal_json(NostrEventEnvelope *envelope, const char *json_data) {
    if (!json_data || !envelope)
        return -1;

    // Parse the JSON to check the number of elements in the array
    NostrEvent *event = nostr_event_new();
    int err = nostr_event_deserialize(event, json_data);
    if (err != 0)
        return -1;

    envelope->event = event;
    return 0;
}

char *event_envelope_marshal_json(NostrEventEnvelope *envelope) {
    if (!envelope || !envelope->event)
        return NULL;

    // Serialize the event
    char *serialized_event = nostr_event_serialize(envelope->event);
    if (!serialized_event)
        return NULL;

    // Get the length of the subscription ID (handle NULL)
    size_t subscription_id_len = envelope->subscription_id ? strlen(envelope->subscription_id) : 0;

    // Calculate the total length of the final JSON string
    size_t total_len = subscription_id_len + strlen(serialized_event) + 20; // 20 for fixed parts of the string

    // Allocate sufficient space for the final JSON string
    char *json_str = malloc(total_len + 1); // +1 for the null terminator
    if (!json_str) {
        free(serialized_event);
        return NULL;
    }

    // Construct the final JSON array string
    snprintf(json_str, total_len + 1, "[\"EVENT\",\"%s\",%s]",
             envelope->subscription_id ? envelope->subscription_id : "",
             serialized_event);

    // Free the serialized event after usage
    free(serialized_event);

    return json_str;
}

/* GLib-style accessors (header: nostr-envelope.h) */
NostrEnvelopeType nostr_envelope_get_type(const NostrEnvelope *env) {
    if (!env) return NOSTR_ENVELOPE_UNKNOWN;
    return env->type;
}

const char *nostr_event_envelope_get_subscription_id(const NostrEventEnvelope *env) {
    if (!env) return NULL;
    return env->subscription_id;
}

NostrEvent *nostr_event_envelope_get_event(const NostrEventEnvelope *env) {
    if (!env) return NULL;
    return env->event;
}

const char *nostr_req_envelope_get_subscription_id(const NostrReqEnvelope *env) {
    if (!env) return NULL;
    return env->subscription_id;
}

NostrFilters *nostr_req_envelope_get_filters(const NostrReqEnvelope *env) {
    if (!env) return NULL;
    return env->filters;
}

const char *nostr_count_envelope_get_subscription_id(const NostrCountEnvelope *env) {
    if (!env) return NULL;
    return env->subscription_id;
}

NostrFilters *nostr_count_envelope_get_filters(const NostrCountEnvelope *env) {
    if (!env) return NULL;
    return env->filters;
}

int nostr_count_envelope_get_count(const NostrCountEnvelope *env) {
    if (!env) return 0;
    return env->count;
}

const char *nostr_notice_envelope_get_message(const NostrNoticeEnvelope *env) {
    if (!env) return NULL;
    return env->message;
}

const char *nostr_eose_envelope_get_message(const NostrEOSEEnvelope *env) {
    if (!env) return NULL;
    return env->message;
}

const char *nostr_close_envelope_get_message(const NostrCloseEnvelope *env) {
    if (!env) return NULL;
    return env->message;
}

const char *nostr_closed_envelope_get_subscription_id(const NostrClosedEnvelope *env) {
    if (!env) return NULL;
    return env->subscription_id;
}

const char *nostr_closed_envelope_get_reason(const NostrClosedEnvelope *env) {
    if (!env) return NULL;
    return env->reason;
}

const char *nostr_ok_envelope_get_event_id(const NostrOKEnvelope *env) {
    if (!env) return NULL;
    return env->event_id;
}

bool nostr_ok_envelope_get_ok(const NostrOKEnvelope *env) {
    if (!env) return false;
    return env->ok;
}

const char *nostr_ok_envelope_get_reason(const NostrOKEnvelope *env) {
    if (!env) return NULL;
    return env->reason;
}

const char *nostr_auth_envelope_get_challenge(const NostrAuthEnvelope *env) {
    if (!env) return NULL;
    return env->challenge;
}

NostrEvent *nostr_auth_envelope_get_event(const NostrAuthEnvelope *env) {
    if (!env) return NULL;
    return env->event;
}
