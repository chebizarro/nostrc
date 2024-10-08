#ifndef NOSTR_ENVELOPE_H
#define NOSTR_ENVELOPE_H

#include "event.h"
#include "filter.h"

// Define the Envelope types
typedef enum {
    ENVELOPE_EVENT,
    ENVELOPE_REQ,
    ENVELOPE_COUNT,
    ENVELOPE_NOTICE,
    ENVELOPE_EOSE,
    ENVELOPE_CLOSE,
    ENVELOPE_CLOSED,
    ENVELOPE_OK,
    ENVELOPE_AUTH,
    ENVELOPE_UNKNOWN
} EnvelopeType;

// Base Envelope struct
typedef struct {
    EnvelopeType type;
} Envelope;

// EventEnvelope struct
typedef struct {
    Envelope base;
    char *subscription_id;
    NostrEvent *event;
} EventEnvelope;

// ReqEnvelope struct
typedef struct {
    Envelope base;
    char *subscription_id;
    Filters filters;
} ReqEnvelope;

// CountEnvelope struct
typedef struct {
    Envelope base;
    char *subscription_id;
    Filters filters;
    int64_t *count;
} CountEnvelope;

// NoticeEnvelope struct
typedef struct {
    Envelope base;
    char *message;
} NoticeEnvelope;

// EOSEEnvelope struct
typedef struct {
    Envelope base;
    char *message;
} EOSEEnvelope;

// CloseEnvelope struct
typedef struct {
    Envelope base;
    char *message;
} CloseEnvelope;

// ClosedEnvelope struct
typedef struct {
    Envelope base;
    char *subscription_id;
    char *reason;
} ClosedEnvelope;

// OKEnvelope struct
typedef struct {
    Envelope base;
    char *event_id;
    bool ok;
    char *reason;
} OKEnvelope;

// AuthEnvelope struct
typedef struct {
    Envelope base;
    char *challenge;
    NostrEvent event;
} AuthEnvelope;

Envelope * create_envelope(EnvelopeType type);
Envelope * parse_message(const char *message);
void free_envelope(Envelope *envelope);
int event_envelope_unmarshal_json(EventEnvelope * envelope, const char * json_data);
char * event_envelope_marshal_json(EventEnvelope * envelope);
char *envelope_to_json(Envelope *envelope);

#endif // NOSTR_ENVELOPE_H