#ifndef __NOSTR_ENVELOPE_H__
#define __NOSTR_ENVELOPE_H__

/* GLib-friendly public accessors for Envelope and subtypes. */

#include <stdbool.h>
#include "envelope.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical typedef for GLib-style naming */
typedef Envelope NostrEnvelope;

typedef EventEnvelope  NostrEventEnvelope;
typedef ReqEnvelope    NostrReqEnvelope;
typedef CountEnvelope  NostrCountEnvelope;
typedef NoticeEnvelope NostrNoticeEnvelope;
typedef EOSEEnvelope   NostrEOSEEnvelope;
typedef CloseEnvelope  NostrCloseEnvelope;
typedef ClosedEnvelope NostrClosedEnvelope;
typedef OKEnvelope     NostrOKEnvelope;
typedef AuthEnvelope   NostrAuthEnvelope;

typedef EnvelopeType   NostrEnvelopeType;

/* Generic */
NostrEnvelopeType nostr_envelope_get_type(const NostrEnvelope *env);

/* EVENT */
const char    *nostr_event_envelope_get_subscription_id(const NostrEventEnvelope *env);
NostrEvent    *nostr_event_envelope_get_event(const NostrEventEnvelope *env);

/* REQ */
const char    *nostr_req_envelope_get_subscription_id(const NostrReqEnvelope *env);
Filters       *nostr_req_envelope_get_filters(const NostrReqEnvelope *env);

/* COUNT */
const char    *nostr_count_envelope_get_subscription_id(const NostrCountEnvelope *env);
Filters       *nostr_count_envelope_get_filters(const NostrCountEnvelope *env);
int            nostr_count_envelope_get_count(const NostrCountEnvelope *env);

/* NOTICE */
const char    *nostr_notice_envelope_get_message(const NostrNoticeEnvelope *env);

/* EOSE */
const char    *nostr_eose_envelope_get_message(const NostrEOSEEnvelope *env);

/* CLOSE */
const char    *nostr_close_envelope_get_message(const NostrCloseEnvelope *env);

/* CLOSED */
const char    *nostr_closed_envelope_get_subscription_id(const NostrClosedEnvelope *env);
const char    *nostr_closed_envelope_get_reason(const NostrClosedEnvelope *env);

/* OK */
const char    *nostr_ok_envelope_get_event_id(const NostrOKEnvelope *env);
bool           nostr_ok_envelope_get_ok(const NostrOKEnvelope *env);
const char    *nostr_ok_envelope_get_reason(const NostrOKEnvelope *env);

/* AUTH */
const char    *nostr_auth_envelope_get_challenge(const NostrAuthEnvelope *env);
NostrEvent    *nostr_auth_envelope_get_event(const NostrAuthEnvelope *env);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_ENVELOPE_H__ */
