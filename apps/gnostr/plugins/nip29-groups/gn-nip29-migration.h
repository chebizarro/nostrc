/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-migration.h - detect NIP-29 group migrations and forks
 *
 * NIP-29 "Detecting migrations and forks": clients SHOULD periodically - and
 * MUST when the group's relay is offline or unreachable - read the
 * kind:10009 (NIP-51 simple groups: ["group", <id>, <relay>, <name>?]) of
 * the group's admins and of trusted friends; an entry for the group that
 * points to another relay means the group may have moved or been forked.
 *
 * Pure logic (unit-tested by gnostr-test-nip29-migration): which relays
 * trusted authors now list a group on. The service supplies the events and
 * the trusted pubkeys (cached admins, the user).
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  gchar     *relay_url;  /* as the newest list of the first author gave it */
  GPtrArray *pubkeys;    /* (element-type utf8) trusted authors listing it */
} GnNip29Relocation;

void gn_nip29_relocation_free(GnNip29Relocation *relocation);

/* Same relay: scheme and host case-insensitive, a trailing '/' ignored. */
gboolean gn_nip29_relay_url_equal(const char *a, const char *b);

/* @event_jsons: kind:10009 events (any authors, any order, duplicates ok).
 * Only events whose id and signature verify, whose author is in
 * @trusted_pubkeys (64-hex) and that are each author's newest kind:10009
 * count. Returns the relays other than @current_relay on which they list
 * @group_id, most authors first (ties: first seen).
 * Returns: (transfer full) (element-type GnNip29Relocation) */
GPtrArray *gn_nip29_find_relocations(const char         *group_id,
                                     const char         *current_relay,
                                     const char * const *trusted_pubkeys,
                                     GPtrArray          *event_jsons);

G_END_DECLS
