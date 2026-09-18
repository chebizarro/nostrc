#ifndef NOSTR_NIP_CAS0010_OTEL_INTERNAL_H
#define NOSTR_NIP_CAS0010_OTEL_INTERNAL_H

#include "nostr/nip_cas0010/otel.h"

/* Returns the single tag named @name, requiring exactly one occurrence with
 * exactly two elements, and yields its value in @out_value (borrowed).
 * Returns NOSTR_OTEL_OK or NOSTR_OTEL_ERR_MALFORMED_EVENT. */
int nostr_otel_required_tag_value(const NostrTags *tags, const char *name,
                                  const char **out_value);

/* Returns the value of the first tag named @name, or NULL. */
const char *nostr_otel_optional_tag_value(const NostrTags *tags, const char *name);

#endif /* NOSTR_NIP_CAS0010_OTEL_INTERNAL_H */
