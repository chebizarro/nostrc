/*
 * gnostr-nostr-target.c — nostr: link / Handler1 event routing core
 * (nostrc-prqu.3). See gnostr-nostr-target.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gnostr-nostr-target.h"

#include <nostr-gtk-1.0/gn-nostr-reference.h>
#include <string.h>

G_DEFINE_QUARK(gnostr-nostr-target-error-quark, gnostr_nostr_target_error)

/* ---- kind -> view ---------------------------------------------------- */

/* Kinds GNostr has a purpose-built view for. Keep ascending and identical
 * to the numeric X-Nostr-Kinds= entries of data/org.gnostr.gnostr.desktop
 * (tests/test_nostr_target.c checks). Anything else is the `*` fallback. */
static const struct {
  int kind;
  GnostrNostrView view;
} declared_views[] = {
  { 0,     GNOSTR_NOSTR_VIEW_PROFILE  },  /* NIP-01 metadata */
  { 1,     GNOSTR_NOSTR_VIEW_THREAD   },  /* NIP-10 note thread */
  { 6,     GNOSTR_NOSTR_VIEW_THREAD   },  /* NIP-18 repost */
  { 14,    GNOSTR_NOSTR_VIEW_MESSAGES },  /* NIP-17 chat message (rumor) */
  { 16,    GNOSTR_NOSTR_VIEW_THREAD   },  /* NIP-18 generic repost */
  { 1059,  GNOSTR_NOSTR_VIEW_MESSAGES },  /* NIP-59 gift wrap (nostr-notify DM links) */
  { 1111,  GNOSTR_NOSTR_VIEW_THREAD   },  /* NIP-22 comment thread */
  { 30023, GNOSTR_NOSTR_VIEW_ARTICLE  },  /* NIP-23 long-form */
};

GnostrNostrView
gnostr_nostr_view_for_kind(int kind)
{
  for (gsize i = 0; i < G_N_ELEMENTS(declared_views); i++)
    if (declared_views[i].kind == kind)
      return declared_views[i].view;
  return GNOSTR_NOSTR_VIEW_THREAD;
}

const int *
gnostr_nostr_declared_kinds(gsize *n_kinds)
{
  static int kinds[G_N_ELEMENTS(declared_views)];
  static gsize initialized = 0;
  if (g_once_init_enter(&initialized)) {
    for (gsize i = 0; i < G_N_ELEMENTS(declared_views); i++)
      kinds[i] = declared_views[i].kind;
    g_once_init_leave(&initialized, 1);
  }
  if (n_kinds)
    *n_kinds = G_N_ELEMENTS(declared_views);
  return kinds;
}

/* ---- URI parsing ----------------------------------------------------- */

void
gnostr_nostr_target_free(GnostrNostrTarget *target)
{
  if (!target)
    return;
  g_free(target->pubkey_hex);
  g_free(target->event_id_hex);
  g_free(target->d_tag);
  g_strfreev(target->relays);
  g_free(target);
}

static GnostrNostrTargetError
target_error_code(const GError *error)
{
  if (error && error->domain == GN_NOSTR_REFERENCE_ERROR) {
    switch (error->code) {
      case GN_NOSTR_REFERENCE_ERROR_REFUSED:     return GNOSTR_NOSTR_TARGET_ERROR_REFUSED;
      case GN_NOSTR_REFERENCE_ERROR_UNSUPPORTED: return GNOSTR_NOSTR_TARGET_ERROR_UNSUPPORTED;
      default: break;
    }
  }
  return GNOSTR_NOSTR_TARGET_ERROR_INVALID;
}

/* The dispatcher / org.nostr.Handler1 boundary: accept web+nostr:, drop the
 * query string and fragment, then decode with the shared nostr-gtk parser
 * (gn_nostr_reference_parse_full), which owns the NIP-19 rules. */
GnostrNostrTarget *
gnostr_nostr_target_parse(const char *uri, GError **error)
{
  const char *bech32 = NULL;
  if (uri && g_ascii_strncasecmp(uri, "nostr:", 6) == 0)
    bech32 = uri + 6;
  else if (uri && g_ascii_strncasecmp(uri, "web+nostr:", 10) == 0)
    bech32 = uri + 10;
  if (!bech32 || !*bech32) {
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID,
                "Not a nostr: link");
    return NULL;
  }
  /* A "nostr://" form is not NIP-21. */
  if (bech32[0] == '/') {
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID,
                "Not a NIP-21 nostr: link");
    return NULL;
  }
  g_autofree char *entity = g_strndup(bech32, strcspn(bech32, "?#"));

  g_autoptr(GError) ref_error = NULL;
  g_autoptr(GnNostrReference) ref = gn_nostr_reference_parse_full(entity, &ref_error);
  if (!ref) {
    GnostrNostrTargetError code = target_error_code(ref_error);
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, code, "%s",
                code == GNOSTR_NOSTR_TARGET_ERROR_REFUSED ?
                  "Refusing to open a private key link" :
                code == GNOSTR_NOSTR_TARGET_ERROR_UNSUPPORTED ?
                  "GNostr does not open this kind of nostr: link" :
                  ref_error ? ref_error->message : "Invalid nostr: link");
    return NULL;
  }

  GnostrNostrTarget *t = g_new0(GnostrNostrTarget, 1);
  switch (ref->type) {
    case GN_NOSTR_REFERENCE_PERSON:
      t->type = GNOSTR_NOSTR_TARGET_PROFILE;
      t->pubkey_hex = g_strdup(ref->author);
      t->kind = 0;
      break;
    case GN_NOSTR_REFERENCE_EVENT:
      t->type = GNOSTR_NOSTR_TARGET_EVENT;
      t->event_id_hex = g_strdup(ref->id);
      t->pubkey_hex = g_strdup(ref->author);
      t->kind = ref->kind;
      break;
    case GN_NOSTR_REFERENCE_ADDRESS:
    default:
      t->type = GNOSTR_NOSTR_TARGET_ADDRESS;
      t->pubkey_hex = g_strdup(ref->author);
      t->kind = ref->kind;
      t->d_tag = g_strdup(ref->id ? ref->id : "");
      break;
  }
  GPtrArray *relays = g_ptr_array_new();
  for (gsize i = 0; ref->relay_hints && ref->relay_hints[i]; i++)
    if (*ref->relay_hints[i])
      g_ptr_array_add(relays, g_strdup(ref->relay_hints[i]));
  g_ptr_array_add(relays, NULL);
  t->relays = (char **)g_ptr_array_free(relays, FALSE);
  return t;
}

/* ---- events ---------------------------------------------------------- */

void
gnostr_nostr_event_info_free(GnostrNostrEventInfo *info)
{
  if (!info)
    return;
  g_free(info->id_hex);
  g_free(info->pubkey_hex);
  g_free(info->d_tag);
  g_free(info);
}

GnostrNostrEventInfo *
gnostr_nostr_event_parse(const char *event_json, GError **error)
{
  g_autoptr(GError) ref_error = NULL;
  g_autoptr(GnNostrEventInfo) verified = gn_nostr_event_parse(event_json, &ref_error);
  if (!verified) {
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID,
                "%s", ref_error ? ref_error->message : "Invalid event");
    return NULL;
  }
  GnostrNostrEventInfo *info = g_new0(GnostrNostrEventInfo, 1);
  info->id_hex = g_steal_pointer(&verified->id);
  info->pubkey_hex = g_steal_pointer(&verified->pubkey);
  info->kind = verified->kind;
  info->created_at = verified->created_at;
  info->d_tag = g_steal_pointer(&verified->d_tag);
  return info;
}

gboolean
gnostr_nostr_event_matches_target(const GnostrNostrEventInfo *info,
                                  const GnostrNostrTarget *target)
{
  g_return_val_if_fail(info != NULL && target != NULL, FALSE);
  /* Matching rules live in nostr-gtk (gn_nostr_reference_matches_event_info). */
  GnNostrReference ref = {
    .type = target->type == GNOSTR_NOSTR_TARGET_EVENT ? GN_NOSTR_REFERENCE_EVENT :
            target->type == GNOSTR_NOSTR_TARGET_ADDRESS ? GN_NOSTR_REFERENCE_ADDRESS :
            GN_NOSTR_REFERENCE_PERSON,
    .id = target->type == GNOSTR_NOSTR_TARGET_ADDRESS ? target->d_tag : target->event_id_hex,
    .author = target->pubkey_hex,
    .kind = target->kind,
  };
  GnNostrEventInfo ev = {
    .id = info->id_hex, .pubkey = info->pubkey_hex, .kind = info->kind,
    .created_at = info->created_at, .d_tag = info->d_tag,
  };
  return gn_nostr_reference_matches_event_info(&ref, &ev);
}

char *
gnostr_nostr_pick_event_for_target(const GnostrNostrTarget *target,
                                   const char *const *event_jsons,
                                   gsize n_events)
{
  g_return_val_if_fail(target != NULL, NULL);
  const char *best = NULL;
  GnostrNostrEventInfo *best_info = NULL;
  for (gsize i = 0; i < n_events; i++) {
    GnostrNostrEventInfo *info = gnostr_nostr_event_parse(event_jsons[i], NULL);
    if (!info || !gnostr_nostr_event_matches_target(info, target)) {
      gnostr_nostr_event_info_free(info);
      continue;
    }
    gboolean better = !best_info ||
        info->created_at > best_info->created_at ||
        (info->created_at == best_info->created_at &&
         strcmp(info->id_hex, best_info->id_hex) < 0);
    if (better) {
      gnostr_nostr_event_info_free(best_info);
      best_info = info;
      best = event_jsons[i];
    } else {
      gnostr_nostr_event_info_free(info);
    }
  }
  gnostr_nostr_event_info_free(best_info);
  return best ? g_strdup(best) : NULL;
}
