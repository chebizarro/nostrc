#include "envelope.h"
#include "json.h"

// Helper function to create a new Envelope
Envelope *create_envelope(EnvelopeType type) {
    Envelope *envelope = (Envelope *)malloc(sizeof(Envelope));
    if (!envelope)
        return NULL;
    envelope->type = type;
    return envelope;
}

// Function to parse a message and return the appropriate Envelope struct
Envelope *parse_message(const char *message) {
    if (!message)
        return NULL;

    char *first_comma = strchr(message, ',');
    if (!first_comma)
        return NULL;

    char label[16];
    strncpy(label, message, first_comma - message);
    label[first_comma - message] = '\0';

    Envelope *envelope = NULL;

    // Check the label and return the corresponding envelope
    if (strcmp(label, "EVENT") == 0) {
        envelope = malloc(sizeof(EventEnvelope));
        envelope->type = ENVELOPE_EVENT;
    } else if (strcmp(label, "REQ") == 0) {
        envelope = malloc(sizeof(ReqEnvelope));
        envelope->type = ENVELOPE_REQ;
    } else if (strcmp(label, "COUNT") == 0) {
        envelope = (Envelope *)malloc(sizeof(CountEnvelope));
        envelope->type = ENVELOPE_COUNT;
    } else if (strcmp(label, "NOTICE") == 0) {
        envelope = (Envelope *)malloc(sizeof(NoticeEnvelope));
        envelope->type = ENVELOPE_NOTICE;
    } else if (strcmp(label, "EOSE") == 0) {
        envelope = (Envelope *)malloc(sizeof(EOSEEnvelope));
        envelope->type = ENVELOPE_EOSE;
    } else if (strcmp(label, "CLOSE") == 0) {
        envelope = (Envelope *)malloc(sizeof(CloseEnvelope));
        envelope->type = ENVELOPE_CLOSE;
    } else if (strcmp(label, "CLOSED") == 0) {
        envelope = (Envelope *)malloc(sizeof(ClosedEnvelope));
        envelope->type = ENVELOPE_CLOSED;
    } else if (strcmp(label, "OK") == 0) {
        envelope = (Envelope *)malloc(sizeof(OKEnvelope));
        envelope->type = ENVELOPE_OK;
    } else if (strcmp(label, "AUTH") == 0) {
        envelope = (Envelope *)malloc(sizeof(AuthEnvelope));
        envelope->type = ENVELOPE_AUTH;
    }

    return envelope;
}

// Function to free an Envelope struct
void free_envelope(Envelope *envelope) {
    if (!envelope)
        return;

    switch (envelope->type) {
    case ENVELOPE_EVENT:
        free(((EventEnvelope *)envelope)->subscription_id);
        free_event(((EventEnvelope *)envelope)->event);
        break;
    case ENVELOPE_REQ:
        free(((ReqEnvelope *)envelope)->subscription_id);
        free_filters(((ReqEnvelope *)envelope)->filters);
        break;
    case ENVELOPE_COUNT:
        free(((CountEnvelope *)envelope)->subscription_id);
        free_filters(((CountEnvelope *)envelope)->filters);
        break;
    case ENVELOPE_NOTICE:
        free(((NoticeEnvelope *)envelope)->message);
        break;
    case ENVELOPE_EOSE:
        free(((EOSEEnvelope *)envelope)->message);
        break;
    case ENVELOPE_CLOSE:
        free(((CloseEnvelope *)envelope)->message);
        break;
    case ENVELOPE_CLOSED:
        free(((ClosedEnvelope *)envelope)->subscription_id);
        free(((ClosedEnvelope *)envelope)->reason);
        break;
    case ENVELOPE_OK:
        free(((OKEnvelope *)envelope)->event_id);
        free(((OKEnvelope *)envelope)->reason);
        break;
    case ENVELOPE_AUTH:
        free(((AuthEnvelope *)envelope)->challenge);
        free_event(((AuthEnvelope *)envelope)->event);
        break;
    default:
        break;
    }

    free(envelope);
}

int event_envelope_unmarshal_json(EventEnvelope *envelope, const char *json_data) {
    if (!json_data || !envelope)
        return -1;

    // Parse the JSON to check the number of elements in the array
    NostrEvent *event = create_event();
    int err = nostr_event_deserialize(event, json_data);
    if (!err)
        return -1;

    envelope->event = event;
    return 0;
}

char *event_envelope_marshal_json(EventEnvelope *envelope) {
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

// Function to convert an Envelope struct to JSON
char *envelope_to_json(Envelope *envelope) {

    switch (envelope->type) {
    case ENVELOPE_EVENT: {
        EventEnvelope *event_envelope = (EventEnvelope *)envelope;
        if (event_envelope->subscription_id) {
        }
        break;
    }
    case ENVELOPE_REQ: {
        ReqEnvelope *req_envelope = (ReqEnvelope *)envelope;
        // Add filters...
        break;
    }
    case ENVELOPE_COUNT: {
        CountEnvelope *count_envelope = (CountEnvelope *)envelope;
        if (count_envelope->count) {
        } else {
            // Add filters...
        }
        break;
    }
    case ENVELOPE_NOTICE: {
        NoticeEnvelope *notice_envelope = (NoticeEnvelope *)envelope;
        break;
    }
    case ENVELOPE_EOSE: {
        EOSEEnvelope *eose_envelope = (EOSEEnvelope *)envelope;
        break;
    }
    case ENVELOPE_CLOSE: {
        CloseEnvelope *close_envelope = (CloseEnvelope *)envelope;
        break;
    }
    case ENVELOPE_CLOSED: {
        ClosedEnvelope *closed_envelope = (ClosedEnvelope *)envelope;
        break;
    }
    case ENVELOPE_OK: {
        OKEnvelope *ok_envelope = (OKEnvelope *)envelope;
        break;
    }
    case ENVELOPE_AUTH: {
        AuthEnvelope *auth_envelope = (AuthEnvelope *)envelope;
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