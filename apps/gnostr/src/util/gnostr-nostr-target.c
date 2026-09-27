/*
 * gnostr-nostr-target.c — nostr: link / Handler1 event routing core
 * (nostrc-prqu.3). See gnostr-nostr-target.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gnostr-nostr-target.h"

#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-event.h>
#include <nostr-tag.h>
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

static gboolean
is_hex64(const char *s)
{
  if (!s || strlen(s) != 64)
    return FALSE;
  for (const char *p = s; *p; p++)
    if (!g_ascii_isxdigit(*p))
      return FALSE;
  return TRUE;
}

static char **
relays_dup(GNostrNip19 *n19)
{
  const gchar *const *relays = gnostr_nip19_get_relays(n19);
  GPtrArray *out = g_ptr_array_new();
  for (gsize i = 0; relays && relays[i]; i++)
    if (*relays[i])
      g_ptr_array_add(out, g_strdup(relays[i]));
  g_ptr_array_add(out, NULL);
  return (char **)g_ptr_array_free(out, FALSE);
}

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

  /* Bech32 is case-insensitive but must not be mixed-case; NIP-19
   * entities are lowercase in practice. Stop at query/fragment. */
  gsize len = strcspn(bech32, "?#");
  g_autofree char *entity = g_ascii_strdown(bech32, (gssize)len);

  if (g_str_has_prefix(entity, "nsec1") || g_str_has_prefix(entity, "ncryptsec1")) {
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_REFUSED,
                "Refusing to open a private key link");
    return NULL;
  }

  g_autoptr(GError) dec_err = NULL;
  g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(entity, &dec_err);
  if (!n19) {
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID,
                "Invalid nostr: link: %s", dec_err ? dec_err->message : "cannot decode");
    return NULL;
  }

  GnostrNostrTarget *t = g_new0(GnostrNostrTarget, 1);
  t->kind = -1;
  switch (gnostr_nip19_get_entity_type(n19)) {
    case GNOSTR_BECH32_NPUB:
    case GNOSTR_BECH32_NPROFILE:
      t->type = GNOSTR_NOSTR_TARGET_PROFILE;
      t->pubkey_hex = g_strdup(gnostr_nip19_get_pubkey(n19));
      t->kind = 0;
      break;
    case GNOSTR_BECH32_NOTE:
    case GNOSTR_BECH32_NEVENT: {
      t->type = GNOSTR_NOSTR_TARGET_EVENT;
      t->event_id_hex = g_strdup(gnostr_nip19_get_event_id(n19));
      const char *author = gnostr_nip19_get_author(n19);
      if (is_hex64(author))
        t->pubkey_hex = g_ascii_strdown(author, -1);
      /* libnostr decodes a missing kind TLV as 0; profiles are linked as
       * npub/nprofile, so a nevent "kind 0" means "kind unknown". */
      int k = gnostr_nip19_get_kind(n19);
      t->kind = k > 0 ? k : -1;
      break;
    }
    case GNOSTR_BECH32_NADDR: {
      t->type = GNOSTR_NOSTR_TARGET_ADDRESS;
      t->pubkey_hex = g_strdup(gnostr_nip19_get_pubkey(n19));
      t->kind = gnostr_nip19_get_kind(n19);
      const char *d = gnostr_nip19_get_identifier(n19);
      t->d_tag = g_strdup(d ? d : "");
      break;
    }
    case GNOSTR_BECH32_NRELAY:
    default:
      g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_UNSUPPORTED,
                  "GNostr does not open this kind of nostr: link");
      gnostr_nostr_target_free(t);
      return NULL;
  }
  t->relays = relays_dup(n19);

  gboolean ok;
  switch (t->type) {
    case GNOSTR_NOSTR_TARGET_PROFILE:
      ok = is_hex64(t->pubkey_hex);
      break;
    case GNOSTR_NOSTR_TARGET_EVENT:
      ok = is_hex64(t->event_id_hex);
      break;
    case GNOSTR_NOSTR_TARGET_ADDRESS:
    default:
      ok = is_hex64(t->pubkey_hex) && t->kind >= 0 && t->kind <= 65535;
      break;
  }
  if (!ok) {
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID,
                "Invalid nostr: link: missing id or author");
    gnostr_nostr_target_free(t);
    return NULL;
  }
  /* Normalize hex to lowercase for comparisons. */
  if (t->pubkey_hex) {
    char *lower = g_ascii_strdown(t->pubkey_hex, -1);
    g_free(t->pubkey_hex);
    t->pubkey_hex = lower;
  }
  if (t->event_id_hex) {
    char *lower = g_ascii_strdown(t->event_id_hex, -1);
    g_free(t->event_id_hex);
    t->event_id_hex = lower;
  }
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

static char *
first_d_tag(NostrEvent *ev)
{
  NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
  gsize n = tags ? nostr_tags_size(tags) : 0;
  for (gsize i = 0; i < n; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2)
      continue;
    const char *key = nostr_tag_get(tag, 0);
    if (key && strcmp(key, "d") == 0) {
      const char *v = nostr_tag_get(tag, 1);
      return g_strdup(v ? v : "");
    }
  }
  return NULL;
}

GnostrNostrEventInfo *
gnostr_nostr_event_parse(const char *event_json, GError **error)
{
  if (!event_json || !*event_json) {
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID,
                "Empty event");
    return NULL;
  }
  NostrEvent *ev = nostr_event_new();
  if (!ev) {
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID,
                "Out of memory");
    return NULL;
  }
  char id[65] = { 0 };
  NostrEventValidationStatus st = nostr_event_deserialize_signed(ev, event_json, NULL);
  if (st == NOSTR_EVENT_VALIDATION_OK)
    st = nostr_event_validate(ev, id);
  if (st != NOSTR_EVENT_VALIDATION_OK) {
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID,
                "Invalid event: %s", nostr_event_validation_status_string(st));
    nostr_event_free(ev);
    return NULL;
  }
  GnostrNostrEventInfo *info = g_new0(GnostrNostrEventInfo, 1);
  info->id_hex = g_ascii_strdown(id, -1);
  info->pubkey_hex = g_ascii_strdown(nostr_event_get_pubkey(ev), -1);
  info->kind = nostr_event_get_kind(ev);
  info->created_at = nostr_event_get_created_at(ev);
  info->d_tag = first_d_tag(ev);
  nostr_event_free(ev);
  return info;
}

gboolean
gnostr_nostr_event_matches_target(const GnostrNostrEventInfo *info,
                                  const GnostrNostrTarget *target)
{
  g_return_val_if_fail(info != NULL && target != NULL, FALSE);
  switch (target->type) {
    case GNOSTR_NOSTR_TARGET_EVENT:
      if (g_strcmp0(info->id_hex, target->event_id_hex) != 0)
        return FALSE;
      if (target->kind >= 0 && info->kind != target->kind)
        return FALSE;
      if (target->pubkey_hex && g_strcmp0(info->pubkey_hex, target->pubkey_hex) != 0)
        return FALSE;
      return TRUE;
    case GNOSTR_NOSTR_TARGET_ADDRESS:
      return info->kind == target->kind &&
             g_strcmp0(info->pubkey_hex, target->pubkey_hex) == 0 &&
             g_strcmp0(info->d_tag ? info->d_tag : "", target->d_tag ? target->d_tag : "") == 0;
    case GNOSTR_NOSTR_TARGET_PROFILE:
    default:
      return info->kind == 0 && g_strcmp0(info->pubkey_hex, target->pubkey_hex) == 0;
  }
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
