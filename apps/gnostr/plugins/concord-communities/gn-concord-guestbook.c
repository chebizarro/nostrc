/* The Guestbook Plane fold (CORD-02 §5).
 *
 * The fold is a *coalesce*: one final state per npub, where their latest
 * Join, Leave or Kick wins. Two restrictions keep it clean, and both exist
 * because the plane is member-writable and its timestamps are author-settable:
 *
 *   An entry dated more than an hour ahead of the receiver's clock is dropped
 *   outright — ample for skew, and a deterrent against squatting "latest"
 *   with a forged future date.
 *
 *   Entries tying on time break by the lower rumor id, the *inner* event's,
 *   never the outer wrap's, which differs per re-wrap. That tie-break is
 *   author-grindable and knowingly so: the coalesce is per-npub, so an author
 *   only ever grinds ties against their own entries.
 */

#include "gn-concord-guestbook.h"

#include <nip_concord.h>
#include <nostr-event.h>
#include <nostr-tag.h>

#include <stdlib.h>
#include <string.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)

/* CORD-02 §5: ample for clock skew, and the ceiling on a forged future. */
#define CONCORD_GUESTBOOK_FUTURE_DRIFT_MS (60 * 60 * 1000)

typedef struct {
  GnConcordMemberState state;
  gint64 order_key;   /* created_at * 1000 + ms */
  gchar *rumor_id;    /* the tie-break */
  gint64 observed_at; /* the newest activity seen from this npub */
} MemberEntry;

struct _GnConcordGuestbook {
  GHashTable *members; /* pubkey -> MemberEntry* */
  GHashTable *seen;    /* rumor id -> seen */
  GnConcordKickAuthorityFunc authority;
  gpointer authority_data;
  gint64 clock_ms; /* 0 = the real clock */
};

static void member_entry_free(gpointer data) {
  MemberEntry *entry = data;
  if (!entry) return;
  g_free(entry->rumor_id);
  g_free(entry);
}

static MemberEntry *member_entry(GnConcordGuestbook *self,
                                 const char *pubkey) {
  MemberEntry *entry = g_hash_table_lookup(self->members, pubkey);
  if (!entry) {
    entry = g_new0(MemberEntry, 1);
    g_hash_table_insert(self->members, g_strdup(pubkey), entry);
  }
  return entry;
}

static gint64 guestbook_now_ms(GnConcordGuestbook *self) {
  return self->clock_ms ? self->clock_ms : g_get_real_time() / 1000;
}

static const char *tag_value(const NostrEvent *event, const char *key) {
  NostrTags *tags = nostr_event_get_tags(event);
  if (!tags) return NULL;
  for (gsize i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (tag && nostr_tag_size(tag) >= 2 &&
        g_strcmp0(nostr_tag_get(tag, 0), key) == 0)
      return nostr_tag_get(tag, 1);
  }
  return NULL;
}

static NostrEvent *parse_verified_event(const char *event_json) {
  if (!event_json) return NULL;
  NostrEvent *event = nostr_event_new();
  if (!event || !nostr_event_deserialize_compact(event, event_json, NULL) ||
      nostr_event_validate(event, NULL) != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(event);
    return NULL;
  }
  return event;
}

/* One coalesce step, shared by every rumor kind: the newest entry for an npub
 * wins, and a tie breaks by the lower rumor id. */
static gboolean coalesce(GnConcordGuestbook *self, const char *pubkey,
                         GnConcordMemberState state, gint64 order_key,
                         const char *rumor_id) {
  MemberEntry *entry = member_entry(self, pubkey);
  if (entry->rumor_id) {
    if (order_key < entry->order_key) return FALSE;
    if (order_key == entry->order_key &&
        g_strcmp0(rumor_id, entry->rumor_id) >= 0)
      return FALSE;
  }
  gboolean changed = entry->state != state;
  entry->state = state;
  entry->order_key = order_key;
  g_free(entry->rumor_id);
  entry->rumor_id = g_strdup(rumor_id);
  return changed;
}

gboolean gn_concord_guestbook_ingest_wrap(GnConcordGuestbook *self,
                                          const uint8_t conv_key[32],
                                          const char *address_hex,
                                          const char *wrap_json) {
  g_return_val_if_fail(self != NULL, FALSE);
  if (!conv_key || !address_hex || !wrap_json) return FALSE;

  /* Every cleanup-attributed local precedes the first `goto`. */
  gboolean changed = FALSE;
  char *seal_json = NULL;
  char *rumor_json = NULL;
  g_autoptr(NostrEvent) wrap = NULL;
  g_autoptr(NostrEvent) seal = NULL;
  g_autoptr(NostrEvent) rumor = NULL;
  g_autofree gchar *rumor_id = NULL;
  const char *actor = NULL;
  const char *content = NULL;
  const char *ms_tag = NULL;
  const char *target = NULL;
  gint64 order_key = 0;
  int ms = 0;
  int kind = 0;

  wrap = parse_verified_event(wrap_json);
  if (!wrap || nostr_event_get_kind(wrap) != CONCORD_STREAM_WRAP) goto done;
  if (g_strcmp0(nostr_event_get_pubkey(wrap), address_hex) != 0) goto done;

  if (nostr_concord_stream_open(conv_key, nostr_event_get_content(wrap),
                                &seal_json) != NOSTR_CONCORD_OK)
    goto done;

  /* The Guestbook's seals MUST be encrypted (kind 20013): a plaintext one
   * would leave a private roster liftable as a standalone public artifact
   * (CORD-02 §5). */
  seal = parse_verified_event(seal_json);
  if (!seal || nostr_event_get_kind(seal) != CONCORD_SEAL_ENCRYPTED) goto done;

  if (nostr_concord_stream_open(conv_key, nostr_event_get_content(seal),
                                &rumor_json) != NOSTR_CONCORD_OK)
    goto done;

  rumor = nostr_event_new();
  if (!rumor || !nostr_event_deserialize_compact(rumor, rumor_json, NULL))
    goto done;

  /* The seal's signature is the author proof. A Join is a member's own word,
   * so a rumor claiming an author other than the seal that carried it is
   * exactly the forgery this check exists for. */
  actor = nostr_event_get_pubkey(seal);
  if (g_strcmp0(nostr_event_get_pubkey(rumor), actor) != 0) goto done;

  kind = nostr_event_get_kind(rumor);
  /* Snapshots (kind 3312) are the refounder's secondhand attestation and are
   * honored only from the npub whose Refounding minted the epoch — which this
   * client does not yet track, so it folds none. A member entering a new
   * epoch and finding their own state absent simply publishes a fresh Join. */
  if (kind != CONCORD_KIND_JOIN_LEAVE && kind != CONCORD_KIND_KICK) goto done;

  /* An `ms` outside 0..999 is malformed and its entry is dropped, not
   * interpreted, or the excess would smuggle arbitrary "future" past the
   * clock check below. */
  ms_tag = tag_value(rumor, "ms");
  if (ms_tag && !nostr_concord_parse_ms(ms_tag, &ms)) goto done;
  if (!nostr_concord_order_key(nostr_event_get_created_at(rumor), ms,
                               &order_key))
    goto done;

  if (order_key > guestbook_now_ms(self) + CONCORD_GUESTBOOK_FUTURE_DRIFT_MS)
    goto done;

  rumor_id = nostr_event_get_id(rumor);
  if (!rumor_id || g_hash_table_contains(self->seen, rumor_id)) goto done;
  g_hash_table_add(self->seen, g_strdup(rumor_id));

  if (kind == CONCORD_KIND_JOIN_LEAVE) {
    content = nostr_event_get_content(rumor);
    if (g_strcmp0(content, "join") == 0)
      changed = coalesce(self, actor, GN_CONCORD_MEMBER_PRESENT, order_key,
                         rumor_id);
    else if (g_strcmp0(content, "leave") == 0)
      changed = coalesce(self, actor, GN_CONCORD_MEMBER_DEPARTED, order_key,
                         rumor_id);
    goto done;
  }

  /* A Kick is admin-signed and names its target. The Guestbook holds no
   * authority of its own, so the caller resolves rank against the Control
   * Plane Roster; without one, no Kick is honored. */
  target = tag_value(rumor, "p");
  if (!nostr_concord_is_lower_hex_32(target)) goto done;
  if (!self->authority ||
      !self->authority(actor, target, self->authority_data))
    goto done;
  changed =
    coalesce(self, target, GN_CONCORD_MEMBER_DEPARTED, order_key, rumor_id);

done:
  if (seal_json) {
    memset(seal_json, 0, strlen(seal_json));
    free(seal_json);
  }
  if (rumor_json) {
    memset(rumor_json, 0, strlen(rumor_json));
    free(rumor_json);
  }
  return changed;
}

void gn_concord_guestbook_observe(GnConcordGuestbook *self,
                                  const char *pubkey_hex, gint64 order_key) {
  g_return_if_fail(self != NULL);
  if (!nostr_concord_is_lower_hex_32(pubkey_hex)) return;
  MemberEntry *entry = member_entry(self, pubkey_hex);
  if (order_key > entry->observed_at) entry->observed_at = order_key;
}

GnConcordMemberState gn_concord_guestbook_get_state(GnConcordGuestbook *self,
                                                    const char *pubkey_hex) {
  g_return_val_if_fail(self != NULL, GN_CONCORD_MEMBER_ABSENT);
  MemberEntry *entry =
    pubkey_hex ? g_hash_table_lookup(self->members, pubkey_hex) : NULL;
  if (!entry) return GN_CONCORD_MEMBER_ABSENT;
  /* Observation counts *forward* only: an author re-enters the list on
   * activity newer than their latest Leave or Kick, so a departed member's
   * old history can never resurrect them. */
  if (entry->observed_at > entry->order_key) return GN_CONCORD_MEMBER_PRESENT;
  return entry->state;
}

GPtrArray *gn_concord_guestbook_get_members(GnConcordGuestbook *self) {
  g_return_val_if_fail(self != NULL, NULL);
  GPtrArray *members = g_ptr_array_new();
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, self->members);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    if (gn_concord_guestbook_get_state(self, key) ==
        GN_CONCORD_MEMBER_PRESENT)
      g_ptr_array_add(members, key);
  g_ptr_array_sort_values(members, (GCompareFunc)g_strcmp0);
  return members;
}

void gn_concord_guestbook_set_kick_authority(
    GnConcordGuestbook *self, GnConcordKickAuthorityFunc authority,
    gpointer user_data) {
  g_return_if_fail(self != NULL);
  self->authority = authority;
  self->authority_data = user_data;
}

void gn_concord_guestbook_set_clock(GnConcordGuestbook *self, gint64 now_ms) {
  g_return_if_fail(self != NULL);
  self->clock_ms = now_ms;
}

GnConcordGuestbook *gn_concord_guestbook_new(void) {
  GnConcordGuestbook *self = g_new0(GnConcordGuestbook, 1);
  self->members = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        member_entry_free);
  self->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  return self;
}

void gn_concord_guestbook_free(GnConcordGuestbook *self) {
  if (!self) return;
  g_hash_table_unref(self->members);
  g_hash_table_unref(self->seen);
  g_free(self);
}
