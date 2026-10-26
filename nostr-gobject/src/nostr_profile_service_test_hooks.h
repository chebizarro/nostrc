/* Private queue seams for same-package tests; never installed. */
#pragma once
#include <glib.h>
#include "nostr-gobject-export.h"

G_BEGIN_DECLS
#ifdef GNOSTR_TESTING
NOSTR_GOBJECT_API void gnostr_profile_service_private_fetch_batches_append(
    GPtrArray **fetch_batches, guint *fetch_batch_pos, GPtrArray *pubkeys);
NOSTR_GOBJECT_API GPtrArray *gnostr_profile_service_private_fetch_batches_pop(
    GPtrArray *fetch_batches, guint *fetch_batch_pos);
NOSTR_GOBJECT_API void gnostr_profile_service_private_fetch_batches_clear(
    GPtrArray **fetch_batches, guint *fetch_batch_pos);
#endif
G_END_DECLS
