/*
 * nostr-auto-internal.h — Auto-cleanup definitions for libnostr types.
 *
 * INTERNAL ONLY — include this in .c implementation files, never in
 * public headers.  Provides go_autoptr / go_autofree / go_auto for
 * all libnostr heap types.
 */
#ifndef NOSTR_AUTO_INTERNAL_H
#define NOSTR_AUTO_INTERNAL_H

#include "go_auto.h"

/* Forward declarations — only needed if not already included */
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "nostr-subscription.h"
#include "nostr-relay.h"
#include "nostr-connection.h"
#include "nostr-simple-pool.h"
#include "error.h"
#include "secure_buf.h"

/* ── Heap-allocated types (use go_autoptr) ──────────────────────────── */

/* NostrEvent — go_autoptr(NostrEvent) */
GO_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)

/* NostrTags — go_autoptr(NostrTags) */
GO_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrTags, nostr_tags_free)

/* NostrTag is an alias for StringArray — already defined in go_auto.h
 * via GO_DEFINE_AUTOPTR_CLEANUP_FUNC(StringArray, string_array_free).
 * For clarity, define NostrTag separately too: */
static inline void _go_autoptr_cleanup_NostrTag(NostrTag **pp) {
    if (*pp) { nostr_tag_free(*pp); *pp = NULL; }
}
#define _go_NostrTag_autoptr_cleanup _go_autoptr_cleanup_NostrTag

/* NostrFilter — go_autoptr(NostrFilter) */
/* nostr_filter_free is declared in nostr-filter.h */
GO_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrFilter, nostr_filter_free)

/* nostr_secure_buf — stack-allocated, use go_auto(nostr_secure_buf) */
GO_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(nostr_secure_buf, secure_free)

#endif /* NOSTR_AUTO_INTERNAL_H */
