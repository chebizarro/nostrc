#ifndef __NOSTR_NOSTR_ENVELOPE_H__
#define __NOSTR_NOSTR_ENVELOPE_H__

#include "nostr-export.h"

/* GLib-friendly public accessors for Envelope and subtypes. */

#include <stdbool.h>
#include "nostr-event.h"
#include "nostr-filter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Define the Envelope types
typedef enum {
    NOSTR_ENVELOPE_EVENT,
    NOSTR_ENVELOPE_REQ,
    NOSTR_ENVELOPE_COUNT,
    NOSTR_ENVELOPE_NOTICE,
    NOSTR_ENVELOPE_EOSE,
    NOSTR_ENVELOPE_CLOSE,
    NOSTR_ENVELOPE_CLOSED,
    NOSTR_ENVELOPE_OK,
    NOSTR_ENVELOPE_AUTH,
    NOSTR_ENVELOPE_UNKNOWN
} NostrEnvelopeType;

// Base Envelope struct
typedef struct {
    NostrEnvelopeType type;
} NostrEnvelope;

// EventEnvelope struct
typedef struct {
    NostrEnvelope base;
    char *subscription_id;
    NostrEvent *event;
} NostrEventEnvelope;

// ReqEnvelope struct
typedef struct {
    NostrEnvelope base;
    char *subscription_id;
    NostrFilters *filters;
} NostrReqEnvelope;

// CountEnvelope struct
typedef struct {
    NostrEnvelope base;
    char *subscription_id;
    NostrFilters *filters;
    int count;
} NostrCountEnvelope;

// NoticeEnvelope struct
typedef struct {
    NostrEnvelope base;
    char *message;
} NostrNoticeEnvelope;

// EOSEEnvelope struct
typedef struct {
    NostrEnvelope base;
    char *message;
} NostrEOSEEnvelope;

// CloseEnvelope struct
typedef struct {
    NostrEnvelope base;
    char *message;
} NostrCloseEnvelope;

// ClosedEnvelope struct
typedef struct {
    NostrEnvelope base;
    char *subscription_id;
    char *reason;
} NostrClosedEnvelope;

// OKEnvelope struct
typedef struct {
    NostrEnvelope base;
    char *event_id;
    bool ok;
    char *reason;
} NostrOKEnvelope;

// AuthEnvelope struct
typedef struct {
    NostrEnvelope base;
    char *challenge;
    NostrEvent *event;
} NostrAuthEnvelope;

/* Generic */
NOSTR_API NostrEnvelopeType nostr_envelope_get_type(const NostrEnvelope *env);
NOSTR_API NostrEnvelope *nostr_envelope_parse(const char *json);
NOSTR_API void nostr_envelope_free(NostrEnvelope *envelope);

/* Compact fast-path deserializer (used by json.c with backend fallback) */
typedef struct NostrJsonErrorInfo NostrJsonErrorInfo;
NOSTR_API int nostr_envelope_deserialize_compact(NostrEnvelope *envelope, const char *json,
                                        NostrJsonErrorInfo *err_out);
/* Compact fast-path serializer (used by json.c with backend fallback) */
NOSTR_API char *nostr_envelope_serialize_compact(const NostrEnvelope *envelope);

/* EVENT */
NOSTR_API const char    *nostr_event_envelope_get_subscription_id(const NostrEventEnvelope *env);
NOSTR_API NostrEvent    *nostr_event_envelope_get_event(const NostrEventEnvelope *env);

/* REQ */
NOSTR_API const char     *nostr_req_envelope_get_subscription_id(const NostrReqEnvelope *env);
NOSTR_API NostrFilters   *nostr_req_envelope_get_filters(const NostrReqEnvelope *env);

/* COUNT */
NOSTR_API const char     *nostr_count_envelope_get_subscription_id(const NostrCountEnvelope *env);
NOSTR_API NostrFilters   *nostr_count_envelope_get_filters(const NostrCountEnvelope *env);
NOSTR_API int             nostr_count_envelope_get_count(const NostrCountEnvelope *env);

/* NOTICE */
NOSTR_API const char    *nostr_notice_envelope_get_message(const NostrNoticeEnvelope *env);

/* EOSE */
NOSTR_API const char    *nostr_eose_envelope_get_message(const NostrEOSEEnvelope *env);

/* CLOSE */
NOSTR_API const char    *nostr_close_envelope_get_message(const NostrCloseEnvelope *env);

/* CLOSED */
NOSTR_API const char    *nostr_closed_envelope_get_subscription_id(const NostrClosedEnvelope *env);
NOSTR_API const char    *nostr_closed_envelope_get_reason(const NostrClosedEnvelope *env);

/* OK */
NOSTR_API const char    *nostr_ok_envelope_get_event_id(const NostrOKEnvelope *env);
NOSTR_API bool           nostr_ok_envelope_get_ok(const NostrOKEnvelope *env);
NOSTR_API const char    *nostr_ok_envelope_get_reason(const NostrOKEnvelope *env);

/* AUTH */
NOSTR_API const char    *nostr_auth_envelope_get_challenge(const NostrAuthEnvelope *env);
NOSTR_API NostrEvent    *nostr_auth_envelope_get_event(const NostrAuthEnvelope *env);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_NOSTR_ENVELOPE_H__ */
