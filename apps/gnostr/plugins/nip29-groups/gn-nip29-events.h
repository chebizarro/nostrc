/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-events.h - unsigned NIP-29 moderation events the plugin sends
 *
 * Pure JSON builders (no service state, unit-tested by
 * gnostr-test-nip29-events). Moderation events carry the group in an `h`
 * tag and their arguments as tags, per the table in docs/nips/29.md.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* The *group-metadata* fields a kind:9002 edit-metadata carries. NULL or
 * "" strings are left out. */
typedef struct
{
  const char *name;
  const char *about;
  const char *picture;
  const char *banner;
  const char *parent;      /* subgroup: the parent's group id */
  gboolean    is_private;
  gboolean    is_restricted;
  gboolean    is_hidden;
  gboolean    is_closed;
} GnNip29Metadata;

/* TRUE when @md sets nothing (no follow-up kind:9002 is needed). */
gboolean gn_nip29_metadata_is_empty(const GnNip29Metadata *md);

/* kind:9007 create-group: the table gives it no tags besides `h`
 * (nostrc-4gf4); metadata follows in a kind:9002 once the relay has
 * accepted this one. */
gchar *gn_nip29_build_create_group_json(const char *group_id, gint64 created_at);

/* kind:9002 edit-metadata with all of @md's fields, in NIP-29 order:
 * h, name, about, picture, banner, private, restricted, hidden, closed,
 * parent. */
gchar *gn_nip29_build_edit_metadata_json(const char            *group_id,
                                         const GnNip29Metadata *md,
                                         gint64                 created_at);

/* kind:9010 update-pin-list: the full ordered list, each entry an event id
 * (64-hex, `e` tag) or an event address ("<kind>:<pubkey>:<d>", `a` tag).
 * An empty list clears the pins. */
gchar *gn_nip29_build_update_pin_list_json(const char         *group_id,
                                           const char * const *refs,
                                           gint64              created_at);

G_END_DECLS
