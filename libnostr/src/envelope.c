#include "nostr-envelope.h"
#include "nostr-event.h"
#include "json.h"
#include <string.h>
#include <stdlib.h>

// Helper function to create a new Envelope
NostrEnvelope *create_envelope(NostrEnvelopeType type) {
    NostrEnvelope *envelope = (NostrEnvelope *)malloc(sizeof(NostrEnvelope));
    if (!envelope)
        return NULL;
    envelope->type = type;
    return envelope;
}

/* === Compact fast-path serializer === */
static char *json_escape_string_min(const char *s) {
    if (!s) return NULL;
    size_t extra = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') extra++;
    }
    size_t len = strlen(s);
    char *out = (char *)malloc(len + extra + 3); // quotes + null
    if (!out) return NULL;
    char *w = out;
    *w++ = '"';
    for (const char *p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') *w++ = '\\';
        *w++ = *p;
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
        if (env->filters && env->filters->count > 0) {
            parts = (char **)calloc(env->filters->count, sizeof(char *));
            if (!parts) { free(sid); return NULL; }
            for (size_t i = 0; i < env->filters->count; ++i) {
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
        w += sprintf(w, "[\"REQ\",%s", sid);
        for (size_t i = 0; i < nparts; ++i) {
            w += sprintf(w, ",%s", parts[i]);
        }
        w += sprintf(w, "]");
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
        if (env->filters && env->filters->count > 0) {
            parts = (char **)calloc(env->filters->count, sizeof(char *));
            if (!parts) { free(sid); return NULL; }
            for (size_t i = 0; i < env->filters->count; ++i) {
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
        w += sprintf(w, "[\"COUNT\",%s,%s", sid, countbuf);
        for (size_t i = 0; i < nparts; ++i) {
            w += sprintf(w, ",%s", parts[i]);
        }
        w += sprintf(w, "]");
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

/* === Compact fast-path deserializer === */
int nostr_envelope_deserialize_compact(NostrEnvelope *base, const char *json) {
    if (!base || !json) return 0;
    const char *p = skip_ws(json);
    if (*p != '[') return 0; ++p; // skip [
    // first: label
    char *label = parse_json_string(&p);
    if (!label) return 0;
    const char *q = parse_comma(p);
    if (!q) { free(label); return 0; }
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
        if (!succ) { nostr_event_free(ev); break; }
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
        while (1) {
            q = parse_comma(p);
            if (!q) break;
            p = skip_ws(q);
            if (*p != '{') break;
            char *obj = parse_json_object(&p);
            if (!obj) break;
            NostrFilter f = {0};
            if (nostr_filter_deserialize_compact(&f, obj)) {
                (void)nostr_filters_add(filters, &f); // moves and zeros f
            } else {
                // not a filter object; ignore for REQ
                nostr_filter_clear(&f);
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
        while (1) {
            q = parse_comma(p);
            if (!q) break;
            p = skip_ws(q);
            if (*p != '{') break;
            const char *savep = p;
            char *obj = parse_json_object(&p);
            if (!obj) break;
            // Detect count object
            if (strstr(obj, "\"count\"")) {
                // naive extract: find first digit after ':'
                const char *cpos = strchr(obj, ':');
                if (cpos) {
                    while (*++cpos == ' ' || *cpos == '\t') {}
                    env->count = atoi(cpos);
                }
            } else {
                // treat as filter
                NostrFilter f = {0};
                if (nostr_filter_deserialize_compact(&f, obj)) {
                    (void)nostr_filters_add(filters, &f);
                } else {
                    nostr_filter_clear(&f);
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
        q = parse_comma(p); if (!q) { ok = 1; break; }
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
            if (!succ) { nostr_event_free(ev); break; }
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
                        if (succ) env->event = ev; else nostr_event_free(ev);
                    }
                }
            }
            ok = 1;
        }
        break;
    }
    case NOSTR_ENVELOPE_REQ: {
        // Let backend handle (filters expected). Return 0 to force fallback.
        break;
    }
    case NOSTR_ENVELOPE_COUNT: {
        if (strcmp(label, "COUNT") != 0) break;
        NostrCountEnvelope *env = (NostrCountEnvelope *)base;
        env->subscription_id = parse_json_string(&p);
        if (!env->subscription_id) break;
        // optional number after comma; skip objects if present
        q = parse_comma(p);
        if (q) {
            p = skip_ws(q);
            if (*p >= '0' && *p <= '9') {
                env->count = atoi(p);
            }
        }
        ok = 1; // filters left to backend in higher-level flows if needed
        break;
    }
    default:
        break;
    }

    free(label);
    return ok;
}

// Helpers to parse JSON array framing quickly without full JSON
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
    return p;
}

static char *parse_json_string(const char **pp) {
    const char *p = skip_ws(*pp);
    if (*p != '"') return NULL;
    ++p; // skip opening quote
    const char *start = p;
    // naive: assumes no escaped quotes; sufficient for protocol labels/ids
    while (*p && *p != '"') ++p;
    if (*p != '"') return NULL;
    size_t len = (size_t)(p - start);
    char *s = (char *)malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, start, len);
    s[len] = '\0';
    *pp = p + 1; // skip closing quote
    return s;
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
    // First element: label string
    char *label = parse_json_string(&p);
    if (!label) return NULL;
    // Next must be comma
    const char *q = parse_comma(p);
    if (!q) { free(label); return NULL; }
    p = q;

    NostrEnvelope *envelope = NULL;
    if (strcmp(label, "EVENT") == 0) {
        NostrEventEnvelope *env = (NostrEventEnvelope *)malloc(sizeof(NostrEventEnvelope));
        if (!env) { free(label); return NULL; }
        env->base.type = NOSTR_ENVELOPE_EVENT;
        // Second element: subscription id
        char *sid = parse_json_string(&p);
        if (!sid) { free(env); free(label); return NULL; }
        env->subscription_id = sid;
        // Comma then event object starts; capture the balanced object
        q = parse_comma(p);
        if (!q) { free(env->subscription_id); free(env); free(label); return NULL; }
        p = skip_ws(q);
        char *event_json = parse_json_object(&p);
        if (!event_json) { free(env->subscription_id); free(env); free(label); return NULL; }
        // hand exact object JSON to event deserializer
        NostrEvent *event = nostr_event_new();
        int ok = nostr_event_deserialize(event, event_json);
        if (!ok) {
            nostr_event_free(event);
            free(event_json);
            free(env->subscription_id);
            free(env);
            free(label);
            return NULL;
        }
        free(event_json);
        env->event = event;
        envelope = (NostrEnvelope *)env;
    } else if (strcmp(label, "EOSE") == 0) {
        NostrEOSEEnvelope *env = (NostrEOSEEnvelope *)malloc(sizeof(NostrEOSEEnvelope));
        if (!env) { free(label); return NULL; }
        env->base.type = NOSTR_ENVELOPE_EOSE;
        // Second element: subscription id (string) is required
        char *sid = parse_json_string(&p);
        if (!sid) { free(env); free(label); return NULL; }
        // We don't currently use it; store in message for debugging
        env->message = sid;
        envelope = (NostrEnvelope *)env;
    } else if (strcmp(label, "NOTICE") == 0) {
        NostrNoticeEnvelope *env = (NostrNoticeEnvelope *)malloc(sizeof(NostrNoticeEnvelope));
        if (!env) { free(label); return NULL; }
        env->base.type = NOSTR_ENVELOPE_NOTICE;
        char *msg = parse_json_string(&p);
        env->message = msg;
        envelope = (NostrEnvelope *)env;
    } else if (strcmp(label, "CLOSED") == 0) {
        NostrClosedEnvelope *env = (NostrClosedEnvelope *)malloc(sizeof(NostrClosedEnvelope));
        if (!env) { free(label); return NULL; }
        env->base.type = NOSTR_ENVELOPE_CLOSED;
        // sub id
        char *sid = parse_json_string(&p);
        if (!sid) { free(env); free(label); return NULL; }
        env->subscription_id = sid;
        q = parse_comma(p);
        if (!q) { free(env->subscription_id); free(env); free(label); return NULL; }
        p = q;
        char *reason = parse_json_string(&p);
        if (!reason) { free(env->subscription_id); free(env); free(label); return NULL; }
        env->reason = reason;
        envelope = (NostrEnvelope *)env;
    } else if (strcmp(label, "OK") == 0) {
        NostrOKEnvelope *env = (NostrOKEnvelope *)malloc(sizeof(NostrOKEnvelope));
        if (!env) { free(label); return NULL; }
        env->base.type = NOSTR_ENVELOPE_OK;
        // event id
        char *eid = parse_json_string(&p);
        env->event_id = eid;
        q = parse_comma(p);
        if (!q) { envelope = (NostrEnvelope *)env; goto done; }
        p = skip_ws(q);
        // parse boolean ok (true/false)
        if (strncmp(p, "true", 4) == 0) { env->ok = true; p += 4; }
        else if (strncmp(p, "false", 5) == 0) { env->ok = false; p += 5; }
        else { free(env->event_id); free(env); free(label); return NULL; }
        q = parse_comma(p);
        if (q) {
            p = q;
            char *rsn = parse_json_string(&p);
            if (!rsn) { free(env->event_id); free(env); free(label); return NULL; }
            env->reason = rsn;
        }
        envelope = (NostrEnvelope *)env;
    } else if (strcmp(label, "COUNT") == 0) {
        NostrCountEnvelope *env = (NostrCountEnvelope *)malloc(sizeof(NostrCountEnvelope));
        if (!env) { free(label); return NULL; }
        env->base.type = NOSTR_ENVELOPE_COUNT;
        // sub id
        char *sid = parse_json_string(&p);
        env->subscription_id = sid;
        // count may be a number or object; we only look for number
        q = parse_comma(p);
        if (!q) { envelope = (NostrEnvelope *)env; goto done; }
        p = skip_ws(q);
        if (*p >= '0' && *p <= '9') {
            env->count = atoi(p);
        }
        envelope = (NostrEnvelope *)env;
    } else if (strcmp(label, "AUTH") == 0) {
        NostrAuthEnvelope *env = (NostrAuthEnvelope *)malloc(sizeof(NostrAuthEnvelope));
        if (!env) { free(label); return NULL; }
        env->base.type = NOSTR_ENVELOPE_AUTH;
        char *challenge = parse_json_string(&p);
        env->challenge = challenge;
        // optional embedded event after comma
        const char *comma = parse_comma(p);
        if (comma) {
            p = skip_ws(comma);
            if (*p == '{') {
                char *ej = parse_json_object(&p);
                if (ej) {
                    NostrEvent *ev = nostr_event_new();
                    int ok = nostr_event_deserialize(ev, ej);
                    if (ok) {
                        env->event = ev;
                    } else {
                        nostr_event_free(ev);
                    }
                    free(ej);
                }
            }
        }
        envelope = (NostrEnvelope *)env;
    } else if (strcmp(label, "REQ") == 0) {
        NostrReqEnvelope *env = (NostrReqEnvelope *)malloc(sizeof(NostrReqEnvelope));
        if (!env) { free(label); return NULL; }
        env->base.type = NOSTR_ENVELOPE_REQ;
        env->subscription_id = parse_json_string(&p);
        envelope = (NostrEnvelope *)env;
    }

done:
    free(label);
    return envelope;
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
    if (!err)
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

// Function to convert a NostrEnvelope struct to JSON
char *envelope_to_json(NostrEnvelope *envelope) {

    switch (envelope->type) {
    case NOSTR_ENVELOPE_EVENT: {
        NostrEventEnvelope *event_envelope = (NostrEventEnvelope *)envelope;
        if (event_envelope->subscription_id) {
        }
        break;
    }
    case NOSTR_ENVELOPE_REQ: {
        // TODO: implement JSON for REQ
        break;
    }
    case NOSTR_ENVELOPE_COUNT: {
        // TODO: implement JSON for COUNT
        break;
    }
    case NOSTR_ENVELOPE_NOTICE: {
        break;
    }
    case NOSTR_ENVELOPE_EOSE: {
        break;
    }
    case NOSTR_ENVELOPE_CLOSE: {
        break;
    }
    case NOSTR_ENVELOPE_CLOSED: {
        break;
    }
    case NOSTR_ENVELOPE_OK: {
        break;
    }
    case NOSTR_ENVELOPE_AUTH: {
        NostrAuthEnvelope *auth_envelope = (NostrAuthEnvelope *)envelope;
        if (auth_envelope->challenge) {

        } else {
        }
        break;
    }
    default:
        break;
    }

    char *json_string = NULL;
    return json_string;
}
