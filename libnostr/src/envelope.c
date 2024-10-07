#include "envelope.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Helper function to create a new Envelope
Envelope *create_envelope(EnvelopeType type) {
    Envelope *envelope = (Envelope *)malloc(sizeof(Envelope));
    if (!envelope) return NULL;
    envelope->type = type;
    return envelope;
}

// Function to parse a message and return the appropriate Envelope struct
Envelope *parse_message(const char *message) {
    if (!message) return NULL;

    char *first_comma = strchr(message, ',');
    if (!first_comma) return NULL;

    char label[16];
    strncpy(label, message, first_comma - message);
    label[first_comma - message] = '\0';

    // Check the label and return the corresponding envelope
    if (strcmp(label, "EVENT") == 0) {
        EventEnvelope *envelope = malloc(sizeof(EventEnvelope));

        if (EventEnvelope_UnmarshalJSON(envelope, message, json_iface) == 0) {
            return envelope;
        }

        free(envelope);
    } else if (strcmp(label, "REQ") == 0) {
        ReqEnvelope *envelope = malloc(sizeof(ReqEnvelope));
        if (ReqEnvelope_UnmarshalJSON(envelope, message, json_iface) == 0) {
            return envelope;
        }
        free(envelope);
    } else if (strcmp(label, "COUNT") == 0) {
        envelope = (Envelope *)malloc(sizeof(CountEnvelope));
        envelope->type = ENVELOPE_COUNT;
        CountEnvelope *count_envelope = (CountEnvelope *)envelope;

    } else if (strcmp(label, "NOTICE") == 0) {
        envelope = (Envelope *)malloc(sizeof(NoticeEnvelope));
        envelope->type = ENVELOPE_NOTICE;
        NoticeEnvelope *notice_envelope = (NoticeEnvelope *)envelope;

    } else if (strcmp(label, "EOSE") == 0) {
        envelope = (Envelope *)malloc(sizeof(EOSEEnvelope));
        envelope->type = ENVELOPE_EOSE;
        EOSEEnvelope *eose_envelope = (EOSEEnvelope *)envelope;

    } else if (strcmp(label, "CLOSE") == 0) {
        envelope = (Envelope *)malloc(sizeof(CloseEnvelope));
        envelope->type = ENVELOPE_CLOSE;
        CloseEnvelope *close_envelope = (CloseEnvelope *)envelope;

    } else if (strcmp(label, "CLOSED") == 0) {
        envelope = (Envelope *)malloc(sizeof(ClosedEnvelope));
        envelope->type = ENVELOPE_CLOSED;
        ClosedEnvelope *closed_envelope = (ClosedEnvelope *)envelope;

    } else if (strcmp(label, "OK") == 0) {
        envelope = (Envelope *)malloc(sizeof(OKEnvelope));
        envelope->type = ENVELOPE_OK;
        OKEnvelope *ok_envelope = (OKEnvelope *)envelope;

    } else if (strcmp(label, "AUTH") == 0) {
        envelope = (Envelope *)malloc(sizeof(AuthEnvelope));
        envelope->type = ENVELOPE_AUTH;
        AuthEnvelope *auth_envelope = (AuthEnvelope *)envelope;

    }

    return envelope;
}

// Function to free an Envelope struct
void free_envelope(Envelope *envelope) {
    if (!envelope) return;

    switch (envelope->type) {
        case ENVELOPE_EVENT:
            free(((EventEnvelope *)envelope)->subscription_id);
            free_event(&((EventEnvelope *)envelope)->event);
            break;
        case ENVELOPE_REQ:
            free(((ReqEnvelope *)envelope)->subscription_id);
            free_filters(&((ReqEnvelope *)envelope)->filters);
            break;
        case ENVELOPE_COUNT:
            free(((CountEnvelope *)envelope)->subscription_id);
            free_filters(&((CountEnvelope *)envelope)->filters);
            free(((CountEnvelope *)envelope)->count);
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
            free_event(&((AuthEnvelope *)envelope)->event);
            break;
        default:
            break;
    }

    free(envelope);
}

int event_envelope_unmarshal_json(EventEnvelope *envelope, const char *json_data) {
    if (!json_data || !envelope) return -1;

    // Parse the JSON to check the number of elements in the array
    NostrEvent *event = json_iface->deserialize(json_data);
    if (!event) return -1;

    envelope->event = event;
    return 0;
}

char *event_envelope_marshal_json(EventEnvelope *envelope) {
    if (!envelope || !envelope->event) return NULL;

    // Serialize the event
    char *serialized_event = json_iface->serialize(envelope->event);
    if (!serialized_event) return NULL;

    // Construct the final JSON array string
    char *json_str = malloc(1024);  // Allocating enough space for the JSON
    snprintf(json_str, 1024, "[\\"EVENT\\",\\"%s\\",%s]", envelope->subscription_id ? envelope->subscription_id : "", serialized_event);

    free(serialized_event);
    return json_str;
}


// Function to convert an Envelope struct to JSON
char *envelope_to_json(Envelope *envelope) {
    cJSON *json = cJSON_CreateArray();

    switch (envelope->type) {
        case ENVELOPE_EVENT: {
            cJSON_AddItemToArray(json, cJSON_CreateString("EVENT"));
            EventEnvelope *event_envelope = (EventEnvelope *)envelope;
            if (event_envelope->subscription_id) {
                cJSON_AddItemToArray(json, cJSON_CreateString(event_envelope->subscription_id));
            }
            cJSON *event_json = cJSON_CreateObject();
            cJSON_AddStringToObject(event_json, "pubkey", event_envelope->event.pubkey);
            // Add other event fields...
            cJSON_AddItemToArray(json, event_json);
            break;
        }
        case ENVELOPE_REQ: {
            cJSON_AddItemToArray(json, cJSON_CreateString("REQ"));
            ReqEnvelope *req_envelope = (ReqEnvelope *)envelope;
            cJSON_AddItemToArray(json, cJSON_CreateString(req_envelope->subscription_id));
            // Add filters...
            break;
        }
        case ENVELOPE_COUNT: {
            cJSON_AddItemToArray(json, cJSON_CreateString("COUNT"));
            CountEnvelope *count_envelope = (CountEnvelope *)envelope;
            cJSON_AddItemToArray(json, cJSON_CreateString(count_envelope->subscription_id));
            if (count_envelope->count) {
                cJSON *count_json = cJSON_CreateObject();
                cJSON_AddNumberToObject(count_json, "count", *count_envelope->count);
                cJSON_AddItemToArray(json, count_json);
            } else {
                // Add filters...
            }
            break;
        }
        case ENVELOPE_NOTICE: {
            cJSON_AddItemToArray(json, cJSON_CreateString("NOTICE"));
            NoticeEnvelope *notice_envelope = (NoticeEnvelope *)envelope;
            cJSON_AddItemToArray(json, cJSON_CreateString(notice_envelope->message));
            break;
        }
        case ENVELOPE_EOSE: {
            cJSON_AddItemToArray(json, cJSON_CreateString("EOSE"));
            EOSEEnvelope *eose_envelope = (EOSEEnvelope *)envelope;
            cJSON_AddItemToArray(json, cJSON_CreateString(eose_envelope->message));
            break;
        }
        case ENVELOPE_CLOSE: {
            cJSON_AddItemToArray(json, cJSON_CreateString("CLOSE"));
            CloseEnvelope *close_envelope = (CloseEnvelope *)envelope;
            cJSON_AddItemToArray(json, cJSON_CreateString(close_envelope->message));
            break;
        }
        case ENVELOPE_CLOSED: {
            cJSON_AddItemToArray(json, cJSON_CreateString("CLOSED"));
            ClosedEnvelope *closed_envelope = (ClosedEnvelope *)envelope;
            cJSON_AddItemToArray(json, cJSON_CreateString(closed_envelope->subscription_id));
            cJSON_AddItemToArray(json, cJSON_CreateString(closed_envelope->reason));
            break;
        }
        case ENVELOPE_OK: {
            cJSON_AddItemToArray(json, cJSON_CreateString("OK"));
            OKEnvelope *ok_envelope = (OKEnvelope *)envelope;
            cJSON_AddItemToArray(json, cJSON_CreateString(ok_envelope->event_id));
            cJSON_AddItemToArray(json, cJSON_CreateBool(ok_envelope->ok));
            cJSON_AddItemToArray(json, cJSON_CreateString(ok_envelope->reason));
            break;
        }
        case ENVELOPE_AUTH: {
            cJSON_AddItemToArray(json, cJSON_CreateString("AUTH"));
            AuthEnvelope *auth_envelope = (AuthEnvelope *)envelope;
            if (auth_envelope->challenge) {
                cJSON_AddItemToArray(json, cJSON_CreateString(auth_envelope->challenge));
            } else {
                cJSON *event_json = cJSON_CreateObject();
                cJSON_AddStringToObject(event_json, "pubkey", auth_envelope->event.pubkey);
                // Add other event fields...
                cJSON_AddItemToArray(json, event_json);
            }
            break;
        }
        default:
            break;
    }

    char *json_string = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return json_string;
}