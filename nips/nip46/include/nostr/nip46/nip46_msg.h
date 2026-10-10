#ifndef NOSTR_NIP46_MSG_H
#define NOSTR_NIP46_MSG_H

#include <stddef.h>
#include "nostr/nip46/nip46_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generate a cryptographically random 256-bit lowercase-hex request ID.
 * The caller owns the returned string and frees it with free(). */
char *nostr_nip46_request_id_generate(void);

/* Build/parse JSON strings for NIP-46 requests and responses (unencrypted).
 * Parsers reject duplicate object keys, oversized messages, and invalid field
 * types. For compatibility, responses may carry both result and error.
 *
 * Response parsing semantics:
 *  - If the "result" field is a JSON string, `out->result` is that plain string (no quotes).
 *  - If the "result" field is non-string (object/array/number/bool/null), `out->result`
 *    is set to a newly-allocated compact JSON text of that value (e.g. "{\"k\":1}").
 *    This is directly suitable for functions that expect JSON text, e.g.
 *    `nostr_event_deserialize(event, resp.result)`.
 */
char *nostr_nip46_request_build(const char *id,
                                const char *method,
                                const char *const *params,
                                size_t n_params);
int   nostr_nip46_request_parse(const char *json, NostrNip46Request *out);
void  nostr_nip46_request_free(NostrNip46Request *req);

char *nostr_nip46_response_build_ok(const char *id, const char *result_json);
char *nostr_nip46_response_build_err(const char *id, const char *error_msg);
int   nostr_nip46_response_parse(const char *json, NostrNip46Response *out);
void  nostr_nip46_response_free(NostrNip46Response *res);

/* Classify a response's free-form `error` string (there is no wire-level
 * error code): PROTOCOL for a malformed or unsupported request (retrying the
 * same payload will not help), otherwise DENIED (a policy refusal, and the
 * safe default for unknown text). Case-insensitive keyword table shared by
 * the libnostr pool client and Groundhog (nostrc-8hc9, nostrc-8xfib.1). */
typedef enum {
    NOSTR_NIP46_ERROR_CLASS_DENIED = 0,
    NOSTR_NIP46_ERROR_CLASS_PROTOCOL
} NostrNip46ErrorClass;
NostrNip46ErrorClass nostr_nip46_error_classify(const char *error_text);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_MSG_H */
