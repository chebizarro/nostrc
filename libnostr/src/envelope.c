#include "envelope.h"
//#include "cJSON.h"
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
    cJSON *json = cJSON_Parse(message);
    if (!json) return NULL;

    const cJSON *label = cJSON_GetArrayItem(json, 0);
    if (!cJSON_IsString(label)) {
        cJSON_Delete(json);
        return NULL;
    }

    Envelope *envelope = NULL;

    if (strcmp(label->valuestring, "EVENT") == 0) {
        envelope = (Envelope *)malloc(sizeof(EventEnvelope));
        envelope->type = ENVELOPE_EVENT;
        EventEnvelope *event_envelope = (EventEnvelope *)envelope;
        event_envelope->subscription_id = strdup(cJSON_GetArrayItem(json, 1)->valuestring);
        // Parse the event (omitting error checking for brevity)
        cJSON *event_json = cJSON_GetArrayItem(json, 2);
        event_envelope->event.pubkey = strdup(cJSON_GetObjectItem(event_json, "pubkey")->valuestring);
        // Repeat for other event fields...
    } else if (strcmp(label->valuestring, "REQ") == 0) {
        envelope = (Envelope *)malloc(sizeof(ReqEnvelope));
        envelope->type = ENVELOPE_REQ;
        ReqEnvelope *req_envelope = (ReqEnvelope *)envelope;
        req_envelope->subscription_id = strdup(cJSON_GetArrayItem(json, 1)->valuestring);
        // Parse filters (omitting error checking for brevity)
    } else if (strcmp(label->valuestring, "COUNT") == 0) {
        envelope = (Envelope *)malloc(sizeof(CountEnvelope));
        envelope->type = ENVELOPE_COUNT;
        CountEnvelope *count_envelope = (CountEnvelope *)envelope;
        count_envelope->subscription_id = strdup(cJSON_GetArrayItem(json, 1)->valuestring);
        // Parse count and filters (omitting error checking for brevity)
    } else if (strcmp(label->valuestring, "NOTICE") == 0) {
        envelope = (Envelope *)malloc(sizeof(NoticeEnvelope));
        envelope->type = ENVELOPE_NOTICE;
        NoticeEnvelope *notice_envelope = (NoticeEnvelope *)envelope;
        notice_envelope->message = strdup(cJSON_GetArrayItem(json, 1)->valuestring);
    } else if (strcmp(label->valuestring, "EOSE") == 0) {
        envelope = (Envelope *)malloc(sizeof(EOSEEnvelope));
        envelope->type = ENVELOPE_EOSE;
        EOSEEnvelope *eose_envelope = (EOSEEnvelope *)envelope;
        eose_envelope->message = strdup(cJSON_GetArrayItem(json, 1)->valuestring);
    } else if (strcmp(label->valuestring, "CLOSE") == 0) {
        envelope = (Envelope *)malloc(sizeof(CloseEnvelope));
        envelope->type = ENVELOPE_CLOSE;
        CloseEnvelope *close_envelope = (CloseEnvelope *)envelope;
        close_envelope->message = strdup(cJSON_GetArrayItem(json, 1)->valuestring);
    } else if (strcmp(label->valuestring, "CLOSED") == 0) {
        envelope = (Envelope *)malloc(sizeof(ClosedEnvelope));
        envelope->type = ENVELOPE_CLOSED;
        ClosedEnvelope *closed_envelope = (ClosedEnvelope *)envelope;
        closed_envelope->subscription_id = strdup(cJSON_GetArrayItem(json, 1)->valuestring);
        closed_envelope->reason = strdup(cJSON_GetArrayItem(json, 2)->valuestring);
    } else if (strcmp(label->valuestring, "OK") == 0) {
        envelope = (Envelope *)malloc(sizeof(OKEnvelope));
        envelope->type = ENVELOPE_OK;
        OKEnvelope *ok_envelope = (OKEnvelope *)envelope;
        ok_envelope->event_id = strdup(cJSON_GetArrayItem(json, 1)->valuestring);
        ok_envelope->ok = cJSON_IsTrue(cJSON_GetArrayItem(json, 2));
        ok_envelope->reason = strdup(cJSON_GetArrayItem(json, 3)->valuestring);
    } else if (strcmp(label->valuestring, "AUTH") == 0) {
        envelope = (Envelope *)malloc(sizeof(AuthEnvelope));
        envelope->type = ENVELOPE_AUTH;
        AuthEnvelope *auth_envelope = (AuthEnvelope *)envelope;
        auth_envelope->challenge = strdup(cJSON_GetArrayItem(json, 1)->valuestring);
        // Parse the event (omitting error checking for brevity)
        cJSON *event_json = cJSON_GetArrayItem(json, 2);
        auth_envelope->event.pubkey = strdup(cJSON_GetObjectItem(event_json, "pubkey")->valuestring);
        // Repeat for other event fields...
    }

    cJSON_Delete(json);
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