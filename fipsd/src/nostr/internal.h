#ifndef FIPSD_NOSTR_INTERNAL_H
#define FIPSD_NOSTR_INTERNAL_H
#include "nostr/events.h"
#include <jansson.h>
/* Fixed token budget, depth 8, max 64 children per container; no heap. */
#define FN_TOKENS 1024
typedef struct { size_t start, len; unsigned next, count; char type; } fn_token;
typedef struct { const char *text; size_t len, pos; unsigned used; fn_token t[FN_TOKENS]; } fn_json;
bool fn_parse(fn_json *j, const char *text, size_t len, size_t max);
bool fn_event_shape(const fn_json *j, bool signed_event);
unsigned fn_field(const fn_json *j, unsigned obj, const char *name);
bool fn_string(const fn_json *j, unsigned tok, char *out, size_t cap);
bool fn_eq(const fn_json *j, unsigned tok, const char *s);
bool fn_u64(const fn_json *j, unsigned tok, uint64_t *out);
bool fn_bool(const fn_json *j, unsigned tok, bool *out);
bool fn_null(const fn_json *j, unsigned tok);
const char *fn_tag(const NostrEvent *e, const char *key);
bool fn_tags_bounded(const NostrEvent *e);
void fn_secret(const fips_identity *id, char hex[65]);
fipsd_nostr_status fn_sign(NostrEvent *event, const fips_identity *id);
uint64_t fn_add(uint64_t a, uint64_t b);
uint64_t fn_mul1000(uint64_t a);
/* Checked fixed-capacity JSON writer: strings escaped by Jansson; numbers keep
 * the full upstream u64 domain rather than rounding through double/int64. */
typedef struct { char data[FIPSD_NOSTR_BODY_MAX + 1]; size_t len; bool ok; } fn_writer;
void fn_raw(fn_writer *w, const char *s);
void fn_quote(fn_writer *w, const char *s, size_t cap);
void fn_uint(fn_writer *w, uint64_t n);
char *fn_finish(fn_writer *w);
bool fn_relay(const char *s);
bool fn_relay_normalize(const char *s, char out[FIPSD_NOSTR_URL_MAX + 1]);
#endif
