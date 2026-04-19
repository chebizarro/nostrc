#ifndef NIPS_NIP14_NOSTR_NIP14_NIP14_H
#define NIPS_NIP14_NOSTR_NIP14_NIP14_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdbool.h>

/*
 * NIP-14: Subject Tag in Messaging
 *
 * Adds a "subject" tag to events, similar to email subjects.
 * Typically used with kind 1 (text notes) or kind 4/14/17 (DMs).
 *
 * Tag format: ["subject", "Subject text here"]
 */

/**
 * nostr_nip14_get_subject:
 * @ev: (in) (not nullable): event to inspect
 *
 * Extracts the subject string from the event's "subject" tag.
 *
 * Returns: (transfer none) (nullable): borrowed pointer to the subject
 *          string, or NULL if no subject tag exists. Valid while the
 *          source event is alive and unmodified.
 */
const char *nostr_nip14_get_subject(const NostrEvent *ev);

/**
 * nostr_nip14_has_subject:
 * @ev: (in) (not nullable): event to inspect
 *
 * Returns: true if the event has a "subject" tag
 */
bool nostr_nip14_has_subject(const NostrEvent *ev);

/**
 * nostr_nip14_set_subject:
 * @ev: (inout) (not nullable): event to modify
 * @subject: (in) (not nullable): subject text to set
 *
 * Adds a "subject" tag to the event.
 * If a subject tag already exists, this adds another one.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip14_set_subject(NostrEvent *ev, const char *subject);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP14_NOSTR_NIP14_NIP14_H */
