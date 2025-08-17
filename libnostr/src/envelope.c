#include "nostr-envelope.h"
#include "nostr-event.h"
#include "json.h"

// Helper function to create a new Envelope
NostrEnvelope *create_envelope(NostrEnvelopeType type) {
    NostrEnvelope *envelope = (NostrEnvelope *)malloc(sizeof(NostrEnvelope));
    if (!envelope)
        return NULL;
    envelope->type = type;
    return envelope;
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
