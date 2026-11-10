#include "gh-badge-service.h"
#include "gh-auth-policy.h"
#include "gh-relay-scope.h"
#include "gh-relay-deny-policy.h"
#include "gh-web-content.h"
#include "gh-link-policy.h"
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENT_BYTES (64u * 1024u)
#define MAX_EVENTS 512
#define MAX_PAIRS 64
#define MAX_SOURCES 16
#define DEADLINE_S 20
#define LEGACY_IDENTIFIER "profile_badges"

/* A verified event kept for the lookup: the parsed form and its exact JSON
 * (what the cache stores). */
typedef struct { NostrEvent *event; gchar *json; gchar id[65]; } Ev;
static void ev_free(gpointer data)
{
  Ev *ev = data; nostr_event_free(ev->event); g_free(ev->json); g_free(ev);
}
/* What one lookup established about pubkey, kept per account generation. */
typedef struct { GPtrArray *badges; GhBadgeListKind kind; GStrv hints; } Cached;
static void cached_free(gpointer data)
{
  Cached *c = data; g_clear_pointer(&c->badges, g_ptr_array_unref); g_strfreev(c->hints); g_free(c);
}

struct _GhBadgeService {
  GObject parent_instance;
  GhSocialLists *lists;
  GhAccountStore *account_store;
  GhStore *store; /* borrowed strictly between bound/closing */
  guint64 generation;
  GhListAdapterNetwork network;
  GSettings *settings;
  GhWebContent *web;
  const GhHttpTransport *http; gpointer http_data;
  GHashTable *cache;  /* pubkey -> Cached */
  GPtrArray *awards;  /* own verified GhBadge; NULL before any own lookup */
  GPtrArray *lookups; /* Lookup, in flight */
  GPtrArray *artwork; /* GCancellable per artwork load, cancelled on close */
};
G_DEFINE_TYPE(GhBadgeService, gh_badge_service, G_TYPE_OBJECT)
static void changed(GhBadgeService *s) { g_signal_emit_by_name(s, "changed"); }

/* ---- pure helpers ---------------------------------------------------------- */

static gboolean
hex64(const gchar *s)
{
  if (!s || strlen(s) != 64) return FALSE;
  for (guint i = 0; i < 64; i++)
    if (!g_ascii_isxdigit(s[i]) || g_ascii_isupper(s[i])) return FALSE;
  return TRUE;
}
/* "30009:<issuer>:<identifier>"; identifier may contain colons. */
static gboolean
split_address(const gchar *address, gchar **issuer, const gchar **identifier)
{
  if (!address || !g_str_has_prefix(address, "30009:")) return FALSE;
  const gchar *rest = address + 6;
  if (strlen(rest) < 65 || rest[64] != ':') return FALSE;
  g_autofree gchar *hex = g_strndup(rest, 64);
  if (!hex64(hex)) return FALSE;
  if (issuer) *issuer = g_steal_pointer(&hex);
  if (identifier) *identifier = rest + 65;
  return TRUE;
}
static NostrEvent *
verified(const gchar *json, gchar id[65])
{
  if (!json || strlen(json) > MAX_EVENT_BYTES) return NULL;
  NostrEvent *event = nostr_event_new();
  if (!event) return NULL;
  if (nostr_event_deserialize_signed(event, json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_validate(event, id) != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(event); return NULL;
  }
  return event;
}
/* NIP-01 replaceable ordering: later created_at, then the lower id. */
static gboolean
newer(const NostrEvent *a, const gchar *a_id, const NostrEvent *b, const gchar *b_id)
{
  if (!b) return TRUE;
  gint64 ta = nostr_event_get_created_at(a), tb = nostr_event_get_created_at(b);
  return ta > tb || (ta == tb && g_strcmp0(a_id, b_id) < 0);
}
static const gchar *
tag_value(const NostrEvent *event, const gchar *name)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (gsize i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!g_strcmp0(nostr_tag_get(tag, 0), name) && nostr_tag_size(tag) > 1) return nostr_tag_get(tag, 1);
  }
  return NULL;
}
static GhBadge *
badge_from(const NostrNip58ProfileBadge *pair, const NostrEvent *definition)
{
  NostrNip58BadgeDef def = { 0 };
  if (nostr_nip58_parse_definition(definition, &def) != 0) return NULL;
  GhBadge *badge = g_new0(GhBadge, 1);
  badge->address = g_strdup(pair->badge_ref);
  badge->award_id = g_strdup(pair->award_id);
  badge->award_relay = pair->award_relay && *pair->award_relay ? g_strdup(pair->award_relay) : NULL;
  badge->issuer = g_strdup(nostr_event_get_pubkey(definition));
  badge->identifier = g_strdup(def.identifier);
  badge->name = def.name && *def.name ? g_strdup(def.name) : NULL;
  badge->description = def.description && *def.description ? g_strdup(def.description) : NULL;
  badge->image_url = def.image_url && *def.image_url ? g_strdup(def.image_url) : NULL;
  badge->thumb_url = def.thumb_url && *def.thumb_url ? g_strdup(def.thumb_url) : NULL;
  badge->verified = TRUE;
  return badge;
}
/* A chosen pair whose chain is not known: kept, never attributed. */
static GhBadge *
unverified_badge(const NostrNip58ProfileBadge *pair, gchar *issuer, const gchar *identifier)
{
  GhBadge *badge = g_new0(GhBadge, 1);
  badge->address = g_strdup(pair->badge_ref); badge->award_id = g_strdup(pair->award_id);
  badge->award_relay = pair->award_relay && *pair->award_relay ? g_strdup(pair->award_relay) : NULL;
  badge->issuer = issuer; badge->identifier = g_strdup(identifier);
  return badge;
}
GhBadge *
gh_badge_verify(const NostrNip58ProfileBadge *pair, const gchar *award_json,
                const gchar *definition_json, const gchar *recipient)
{
  gchar award_id[65] = { 0 }, definition_id[65] = { 0 };
  NostrEvent *award = pair && recipient ? verified(award_json, award_id) : NULL;
  NostrEvent *definition = award ? verified(definition_json, definition_id) : NULL;
  GhBadge *badge = NULL;
  if (award && definition && nostr_event_get_kind(award) == NOSTR_KIND_BADGE_AWARD &&
      nostr_event_get_kind(definition) == NOSTR_KIND_BADGE_DEFINITION &&
      !g_strcmp0(award_id, pair->award_id) &&
      nostr_nip58_validate_relationship(pair, award, definition, recipient))
    badge = badge_from(pair, definition);
  if (award) nostr_event_free(award);
  if (definition) nostr_event_free(definition);
  return badge;
}
static GPtrArray *badges_new(void) { return g_ptr_array_new_with_free_func((GDestroyNotify)gh_badge_free); }
static GPtrArray *
badges_copy(GPtrArray *badges)
{
  GPtrArray *copy = badges_new();
  for (guint i = 0; badges && i < badges->len; i++)
    g_ptr_array_add(copy, gh_badge_copy(g_ptr_array_index(badges, i)));
  return copy;
}

/* ---- the account binding ---------------------------------------------------- */

static gboolean
current(GhBadgeService *s)
{
  return s->store && (!s->network.accounts ||
      gh_account_controller_is_current(s->network.accounts, s->generation)) &&
    s->generation == gh_social_lists_get_generation(s->lists) &&
    !g_strcmp0(gh_store_get_account_pubkey(s->store), gh_social_lists_get_account(s->lists));
}
static gboolean
closed(GError **error, const gchar *text)
{
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, text); return FALSE;
}

/* ---- resolving a chain from events in hand and the cache ------------------- */

typedef struct { GhBadgeService *service; GHashTable *events; /* id -> Ev, nullable */ } Resolver;

/* The award of id: in hand, else cached. Owned. */
static Ev *
resolve_award(Resolver *r, const gchar *id)
{
  Ev *held = r->events ? g_hash_table_lookup(r->events, id) : NULL;
  g_autofree gchar *json = held ? g_strdup(held->json)
    : r->service->store ? gh_store_social_cache_get(r->service->store, id, NULL) : NULL;
  if (!json) return NULL;
  Ev *ev = g_new0(Ev, 1);
  ev->event = verified(json, ev->id);
  if (!ev->event || nostr_event_get_kind(ev->event) != NOSTR_KIND_BADGE_AWARD) { ev_free(ev); return NULL; }
  ev->json = g_steal_pointer(&json);
  return ev;
}
static void
consider_definition(Ev **best, const gchar *json, const gchar *issuer, const gchar *identifier)
{
  Ev *ev = g_new0(Ev, 1);
  ev->event = verified(json, ev->id);
  if (ev->event && nostr_event_get_kind(ev->event) == NOSTR_KIND_BADGE_DEFINITION &&
      !g_strcmp0(nostr_event_get_pubkey(ev->event), issuer) &&
      !g_strcmp0(tag_value(ev->event, "d"), identifier) &&
      newer(ev->event, ev->id, *best ? (*best)->event : NULL, *best ? (*best)->id : NULL)) {
    ev->json = g_strdup(json);
    if (*best) ev_free(*best);
    *best = ev;
  } else ev_free(ev);
}
/* The definition at issuer/30009/identifier by NIP-01 ordering over what is
 * in hand and cached. Owned. */
static Ev *
resolve_definition(Resolver *r, const gchar *issuer, const gchar *identifier)
{
  Ev *best = NULL;
  if (r->events) {
    GHashTableIter iter; gpointer value;
    g_hash_table_iter_init(&iter, r->events);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      Ev *ev = value;
      if (nostr_event_get_kind(ev->event) == NOSTR_KIND_BADGE_DEFINITION)
        consider_definition(&best, ev->json, issuer, identifier);
    }
  }
  g_autoptr(GPtrArray) cached = r->service->store
    ? gh_store_social_cache_find(r->service->store, NOSTR_KIND_BADGE_DEFINITION, issuer, identifier, NULL) : NULL;
  for (guint i = 0; cached && i < cached->len; i++)
    consider_definition(&best, g_ptr_array_index(cached, i), issuer, identifier);
  return best;
}
static void
cache_put(GhBadgeService *s, Ev *ev, const gchar *identifier)
{
  if (!current(s)) return;
  g_autoptr(GError) error = NULL;
  if (!gh_store_social_cache_put(s->store, ev->id, nostr_event_get_kind(ev->event),
                                 nostr_event_get_pubkey(ev->event), identifier, ev->json,
                                 gh_clock_get_unix(gh_store_get_clock(s->store)), &error))
    g_message("Groundhog could not cache a badge event: %s", error->message);
}
/* The full chain of pair for recipient, or NULL; verified events are cached
 * when persist is set. */
static GhBadge *
resolve_pair(Resolver *r, const NostrNip58ProfileBadge *pair, const gchar *recipient, gboolean persist)
{
  g_autofree gchar *issuer = NULL; const gchar *identifier = NULL;
  if (!split_address(pair->badge_ref, &issuer, &identifier) || !hex64(pair->award_id)) return NULL;
  Ev *award = resolve_award(r, pair->award_id);
  Ev *definition = award ? resolve_definition(r, issuer, identifier) : NULL;
  GhBadge *badge = definition ? gh_badge_verify(pair, award->json, definition->json, recipient) : NULL;
  if (badge && persist) { cache_put(r->service, award, recipient); cache_put(r->service, definition, identifier); }
  if (award) ev_free(award);
  if (definition) ev_free(definition);
  return badge;
}
/* The pairs of the selected profile list: canonical first, then a valid
 * legacy list (nostr_nip58_select_profile_badges()). overflow: more than
 * capacity pairs exist (the canonical parser reports the full count). */
static gsize
list_pairs(const NostrEvent *list, NostrNip58ProfileBadge *pairs, gsize capacity, gboolean *overflow)
{
  size_t count = 0;
  int rc = nostr_event_get_kind(list) == NOSTR_KIND_PROFILE_BADGE_LIST
    ? nostr_nip58_parse_profile_badge_list(list, pairs, capacity, &count)
    : nostr_nip58_parse_profile_badges(list, pairs, capacity, &count);
  if (overflow) *overflow = rc == -ENOSPC || count > capacity;
  return rc == 0 ? MIN(count, capacity) : 0;
}
static void
consider_list(Ev **best, const gchar *json, const gchar *author, gint kind)
{
  Ev *ev = g_new0(Ev, 1);
  ev->event = verified(json, ev->id);
  gboolean legacy_ok = kind != NOSTR_KIND_BADGE_SETS || nostr_nip58_validate_profile_badges(ev->event);
  if (ev->event && nostr_event_get_kind(ev->event) == kind && legacy_ok &&
      !g_strcmp0(nostr_event_get_pubkey(ev->event), author) &&
      newer(ev->event, ev->id, *best ? (*best)->event : NULL, *best ? (*best)->id : NULL)) {
    ev->json = g_strdup(json);
    if (*best) ev_free(*best);
    *best = ev;
  } else ev_free(ev);
}
/* The displayed list of author from the cache: canonical wins. kind reports
 * which; NULL when none is cached. Owned. */
static Ev *
cached_list(GhBadgeService *s, const gchar *author, GhBadgeListKind *kind)
{
  Ev *canonical = NULL, *legacy = NULL;
  if (s->store) {
    g_autoptr(GPtrArray) lists = gh_store_social_cache_find(s->store, NOSTR_KIND_PROFILE_BADGE_LIST, author, "", NULL);
    for (guint i = 0; lists && i < lists->len; i++)
      consider_list(&canonical, g_ptr_array_index(lists, i), author, NOSTR_KIND_PROFILE_BADGE_LIST);
    g_autoptr(GPtrArray) legacies = gh_store_social_cache_find(s->store, NOSTR_KIND_BADGE_SETS, author, LEGACY_IDENTIFIER, NULL);
    for (guint i = 0; legacies && i < legacies->len; i++)
      consider_list(&legacy, g_ptr_array_index(legacies, i), author, NOSTR_KIND_BADGE_SETS);
  }
  Ev *chosen = canonical ? canonical : legacy;
  if (kind) *kind = canonical ? GH_BADGE_LIST_CANONICAL : legacy ? GH_BADGE_LIST_LEGACY : GH_BADGE_LIST_NONE;
  if (chosen != canonical && canonical) ev_free(canonical);
  if (chosen != legacy && legacy) ev_free(legacy);
  return chosen;
}
/* Every pair of list whose chain verifies, in order; with keep_unverified,
 * the others too, flagged (an own list being migrated loses nothing). */
static GPtrArray *
resolve_list(Resolver *r, const NostrEvent *list, const gchar *recipient, gboolean persist,
             gboolean keep_unverified)
{
  NostrNip58ProfileBadge pairs[MAX_PAIRS];
  gsize n = list_pairs(list, pairs, MAX_PAIRS, NULL);
  GPtrArray *badges = badges_new();
  for (gsize i = 0; i < n; i++) {
    GhBadge *badge = resolve_pair(r, &pairs[i], recipient, persist);
    gchar *issuer = NULL; const gchar *identifier = NULL;
    if (!badge && keep_unverified && split_address(pairs[i].badge_ref, &issuer, &identifier) && hex64(pairs[i].award_id))
      badge = unverified_badge(&pairs[i], issuer, identifier);
    if (badge) g_ptr_array_add(badges, badge);
  }
  return badges;
}
static Cached *
cached_for(GhBadgeService *s, const gchar *pubkey)
{
  Cached *c = g_hash_table_lookup(s->cache, pubkey);
  if (c || !s->store) return c;
  GhBadgeListKind kind = GH_BADGE_LIST_NONE;
  Ev *list = cached_list(s, pubkey, &kind);
  if (!list) return NULL;
  Resolver r = { s, NULL };
  c = g_new0(Cached, 1);
  c->kind = kind;
  c->badges = resolve_list(&r, list->event, pubkey, FALSE, FALSE);
  ev_free(list);
  g_hash_table_insert(s->cache, g_strdup(pubkey), c);
  return c;
}

/* ---- reads ------------------------------------------------------------------ */

GPtrArray *
gh_badge_service_get_badges(GhBadgeService *s, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_BADGE_SERVICE(s), NULL);
  if (!pubkey || !current(s)) return NULL;
  g_autofree gchar *lower = g_ascii_strdown(pubkey, -1);
  Cached *c = cached_for(s, lower);
  return c ? badges_copy(c->badges) : NULL;
}
GhBadgeListKind
gh_badge_service_get_list_kind(GhBadgeService *s, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_BADGE_SERVICE(s), GH_BADGE_LIST_UNKNOWN);
  if (!pubkey || !current(s)) return GH_BADGE_LIST_UNKNOWN;
  g_autofree gchar *lower = g_ascii_strdown(pubkey, -1);
  Cached *c = cached_for(s, lower);
  return c ? c->kind : GH_BADGE_LIST_UNKNOWN;
}
GStrv
gh_badge_service_dup_hints(GhBadgeService *s, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_BADGE_SERVICE(s), NULL);
  g_autofree gchar *lower = pubkey ? g_ascii_strdown(pubkey, -1) : NULL;
  Cached *c = lower && current(s) ? g_hash_table_lookup(s->cache, lower) : NULL;
  return g_strdupv(c && c->hints ? c->hints : (gchar *[]){ NULL });
}
static void
add_source(GhBadgeService *s, GPtrArray *urls, const gchar *url)
{
  if (urls->len < MAX_SOURCES && gh_relay_url_validate(url, NULL) &&
      gh_relay_deny_policy_check(s->generation, url, FALSE, NULL) &&
      !g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL))
    g_ptr_array_add(urls, g_strdup(url));
}
static GStrv
sources(GhBadgeService *s, const gchar *const *approved)
{
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  g_auto(GStrv) discovery = s->settings ? g_settings_get_strv(s->settings, "discovery-relays") : NULL;
  for (guint i = 0; discovery && discovery[i]; i++) add_source(s, urls, discovery[i]);
  for (guint i = 0; approved && approved[i]; i++) add_source(s, urls, approved[i]);
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
}
GStrv
gh_badge_service_dup_sources(GhBadgeService *s)
{
  g_return_val_if_fail(GH_IS_BADGE_SERVICE(s), NULL);
  return sources(s, NULL);
}
GPtrArray *
gh_badge_service_get_awards(GhBadgeService *s)
{
  g_return_val_if_fail(GH_IS_BADGE_SERVICE(s), NULL);
  if (!current(s)) return NULL;
  if (!s->awards && s->store) {
    /* Cached awards to this account, verified again against cached definitions. */
    g_autoptr(GPtrArray) cached = gh_store_social_cache_find(s->store, NOSTR_KIND_BADGE_AWARD, NULL,
                                                             gh_social_lists_get_account(s->lists), NULL);
    if (cached && cached->len) {
      s->awards = badges_new();
      Resolver r = { s, NULL };
      for (guint i = 0; i < cached->len; i++) {
        gchar id[65] = { 0 };
        NostrEvent *award = verified(g_ptr_array_index(cached, i), id);
        NostrNip58ProfileBadge pair = { award ? tag_value(award, "a") : NULL, id, NULL };
        GhBadge *badge = pair.badge_ref ? resolve_pair(&r, &pair, gh_social_lists_get_account(s->lists), FALSE) : NULL;
        if (badge) g_ptr_array_add(s->awards, badge);
        if (award) nostr_event_free(award);
      }
    }
  }
  return s->awards ? badges_copy(s->awards) : NULL;
}

/* ---- lookups ----------------------------------------------------------------- */

typedef struct {
  GhBadgeService *service; /* a reference */
  GTask *task;
  gchar *pubkey;
  gboolean own;
  GStrv sources;
  guint phase;
  GhRelayScope *scope;
  GHashTable *pending; /* url -> SOURCE_* */
  GHashTable *events;  /* id -> Ev */
  guint count;
  GSource *completion;
  guint deadline;
  gulong cancel_handler;
  Ev *list;   /* the selected profile list: one of events, or cached */
  Ev *cached; /* the already-known list, a candidate by NIP-01 ordering */
  NostrNip58ProfileBadge pairs[MAX_PAIRS];
  gsize n_pairs;
  GPtrArray *own_awards; /* Ev borrowed from events: kind 8 naming the account */
} Lookup;

enum { SOURCE_PENDING, SOURCE_ANSWERED, SOURCE_FAILED };
static void
lookup_free(Lookup *l)
{
  if (l->cached) ev_free(l->cached);
  if (l->scope) { gh_relay_scope_cancel(l->scope); gh_relay_scope_unref(l->scope); }
  if (l->completion) { g_source_destroy(l->completion); g_source_unref(l->completion); }
  if (l->deadline) g_source_remove(l->deadline);
  if (l->cancel_handler) g_cancellable_disconnect(g_task_get_cancellable(l->task), l->cancel_handler);
  g_hash_table_unref(l->pending); g_hash_table_unref(l->events);
  if (l->own_awards) g_ptr_array_unref(l->own_awards);
  g_strfreev(l->sources); g_free(l->pubkey);
  g_clear_object(&l->task); g_object_unref(l->service); g_free(l);
}
static void
lookup_end(Lookup *l, GError *error)
{
  GhBadgeService *s = l->service;
  g_ptr_array_remove(s->lookups, l);
  if (error) g_task_return_error(l->task, error);
  else g_task_return_boolean(l->task, TRUE);
  lookup_free(l);
}
static gboolean
all_settled(Lookup *l)
{
  GHashTableIter iter; gpointer value;
  g_hash_table_iter_init(&iter, l->pending);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (GPOINTER_TO_UINT(value) == SOURCE_PENDING) return FALSE;
  return TRUE;
}
/* Whether any source reached EOSE: a failed, closed or silent set of relays
 * is a failed lookup, never an authoritative empty answer. */
static gboolean
answered(Lookup *l)
{
  GHashTableIter iter; gpointer value;
  g_hash_table_iter_init(&iter, l->pending);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (GPOINTER_TO_UINT(value) == SOURCE_ANSWERED) return TRUE;
  return FALSE;
}
static gboolean complete(gpointer data);
/* Deferred to an idle so the scope is never torn down from inside its own
 * callback. */
static void
schedule_completion(Lookup *l)
{
  if (l->completion) return;
  l->completion = g_idle_source_new();
  g_source_set_callback(l->completion, complete, l, NULL);
  g_source_attach(l->completion, g_main_context_get_thread_default());
}
static void
cancelled(GCancellable *cancellable, gpointer data)
{
  (void)cancellable; schedule_completion(data);
}
static gboolean
on_deadline(gpointer data)
{
  Lookup *l = data;
  l->deadline = 0;
  GHashTableIter iter; gpointer value;
  g_hash_table_iter_init(&iter, l->pending);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (GPOINTER_TO_UINT(value) == SOURCE_PENDING) g_hash_table_iter_replace(&iter, GUINT_TO_POINTER(SOURCE_FAILED));
  schedule_completion(l);
  return G_SOURCE_REMOVE;
}
static gboolean
names_account(const NostrEvent *award, const gchar *account)
{
  const char *awardees[64]; size_t n = 0;
  if (nostr_nip58_award_get_awardees(award, awardees, G_N_ELEMENTS(awardees), &n) != 0) return FALSE;
  for (size_t i = 0; i < n; i++) if (!g_strcmp0(awardees[i], account)) return TRUE;
  return FALSE;
}
static void
admit(Lookup *l, const gchar *json)
{
  if (++l->count > MAX_EVENTS) return;
  Ev *ev = g_new0(Ev, 1);
  ev->event = verified(json, ev->id);
  gint kind = ev->event ? nostr_event_get_kind(ev->event) : -1;
  gboolean wanted = l->phase == 1
    ? ((kind == NOSTR_KIND_PROFILE_BADGE_LIST || kind == NOSTR_KIND_BADGE_SETS) &&
       !g_strcmp0(nostr_event_get_pubkey(ev->event), l->pubkey)) ||
      (kind == NOSTR_KIND_BADGE_AWARD && l->own && names_account(ev->event, l->pubkey))
    : kind == NOSTR_KIND_BADGE_AWARD || kind == NOSTR_KIND_BADGE_DEFINITION;
  if (!wanted || g_hash_table_contains(l->events, ev->id)) { ev_free(ev); return; }
  ev->json = g_strdup(json);
  g_hash_table_insert(l->events, g_strdup(ev->id), ev);
}
static void
on_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  Lookup *l = data;
  if (scope != l->scope || l->completion || !g_hash_table_contains(l->pending, update->url)) return;
  guint status = GPOINTER_TO_UINT(g_hash_table_lookup(l->pending, update->url));
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT: admit(l, update->event_json); return;
  case GH_RELAY_NOTICE_EOSE:
    if (status == SOURCE_PENDING) status = SOURCE_ANSWERED;
    break;
  case GH_RELAY_NOTICE_CLOSED: case GH_RELAY_NOTICE_ERROR:
    if (status == SOURCE_PENDING) status = SOURCE_FAILED;
    break;
  default: return;
  }
  g_hash_table_insert(l->pending, g_strdup(update->url), GUINT_TO_POINTER(status));
  if (all_settled(l)) schedule_completion(l);
}
/* One REQ per source on a fresh scope under CONTACT_DIRECTORY (an ephemeral
 * key if a relay demands AUTH, never the account; PD-12). */
static void
start_phase(Lookup *l, guint phase, NostrFilters *filters)
{
  GhBadgeService *s = l->service;
  l->phase = phase;
  if (l->scope) { gh_relay_scope_cancel(l->scope); gh_relay_scope_unref(l->scope); }
  if (l->deadline) g_source_remove(l->deadline);
  g_hash_table_remove_all(l->pending);
  l->scope = s->network.scope_transport
    ? gh_relay_scope_new_with_transport(s->generation, filters, s->network.scope_transport,
                                        s->network.scope_data, on_update, l)
    : gh_relay_scope_new(s->generation, filters, on_update, l);
  if (s->network.scope_transport && s->network.scope_auth_transport)
    gh_relay_scope_set_auth_transport(l->scope, s->network.scope_auth_transport);
  GhAuthPolicy *policy = s->network.accounts ? gh_auth_policy_get_for_accounts(s->network.accounts) : NULL;
  for (guint i = 0; l->sources[i]; i++) {
    if (!gh_relay_scope_add_url(l->scope, l->sources[i], NULL)) continue;
    if (policy)
      gh_auth_policy_apply_scope(policy, l->scope, GH_AUTH_PURPOSE_CONTACT_DIRECTORY, l->sources[i], NULL);
    g_hash_table_insert(l->pending, g_strdup(l->sources[i]), GUINT_TO_POINTER(SOURCE_PENDING));
  }
  l->deadline = g_timeout_add_seconds(DEADLINE_S, on_deadline, l);
  gh_relay_scope_start(l->scope);
  if (all_settled(l)) schedule_completion(l);
}
static NostrFilter *
filter_for(NostrFilters *filters, const int *kinds, gsize n_kinds, const gchar *author, int limit)
{
  NostrFilter *filter = nostr_filter_new();
  nostr_filter_set_kinds(filter, kinds, n_kinds);
  if (author) nostr_filter_set_authors(filter, (const char *const[]){ author }, 1);
  if (limit) nostr_filter_set_limit(filter, limit);
  nostr_filters_add(filters, filter); /* contents moved into the vector */
  nostr_filter_free(filter);
  return &filters->filters[filters->count - 1];
}
static void
select_list(Lookup *l)
{
  Ev *canonical = NULL, *legacy = NULL;
  /* What is already known competes by NIP-01 ordering: a lookup that
   * misses a known canonical list never regresses to an older or legacy one. */
  GhBadgeListKind known = GH_BADGE_LIST_NONE;
  l->cached = cached_list(l->service, l->pubkey, &known);
  if (known == GH_BADGE_LIST_CANONICAL) canonical = l->cached;
  else if (known == GH_BADGE_LIST_LEGACY) legacy = l->cached;
  GHashTableIter iter; gpointer value;
  g_hash_table_iter_init(&iter, l->events);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Ev *ev = value;
    gint kind = nostr_event_get_kind(ev->event);
    if (kind == NOSTR_KIND_PROFILE_BADGE_LIST &&
        newer(ev->event, ev->id, canonical ? canonical->event : NULL, canonical ? canonical->id : NULL))
      canonical = ev;
    else if (kind == NOSTR_KIND_BADGE_SETS && nostr_nip58_validate_profile_badges(ev->event) &&
             newer(ev->event, ev->id, legacy ? legacy->event : NULL, legacy ? legacy->id : NULL))
      legacy = ev;
    else if (kind == NOSTR_KIND_BADGE_AWARD) {
      if (!l->own_awards) l->own_awards = g_ptr_array_new();
      g_ptr_array_add(l->own_awards, ev);
    }
  }
  /* A canonical list wins even when it holds no pairs (§3.12). */
  l->list = canonical ? canonical : legacy;
  l->n_pairs = l->list ? list_pairs(l->list->event, l->pairs, MAX_PAIRS, NULL) : 0;
}
static void
add_unique(GPtrArray *strings, const gchar *value)
{
  if (value && !g_ptr_array_find_with_equal_func(strings, value, g_str_equal, NULL))
    g_ptr_array_add(strings, g_strdup(value));
}
/* After the lists: fetch every award by exact id and every definition by
 * exact author/kind/d, from the same sources only. */
static gboolean
start_fetch(Lookup *l)
{
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) issuers = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) identifiers = g_ptr_array_new_with_free_func(g_free);
  for (gsize i = 0; i < l->n_pairs; i++) {
    g_autofree gchar *issuer = NULL; const gchar *identifier = NULL;
    if (!split_address(l->pairs[i].badge_ref, &issuer, &identifier) || !hex64(l->pairs[i].award_id)) continue;
    add_unique(ids, l->pairs[i].award_id); add_unique(issuers, issuer); add_unique(identifiers, identifier);
  }
  for (guint i = 0; l->own_awards && i < l->own_awards->len; i++) {
    Ev *award = g_ptr_array_index(l->own_awards, i);
    g_autofree gchar *issuer = NULL; const gchar *identifier = NULL;
    if (split_address(tag_value(award->event, "a"), &issuer, &identifier) &&
        !g_strcmp0(issuer, nostr_event_get_pubkey(award->event))) {
      add_unique(issuers, issuer); add_unique(identifiers, identifier);
    }
  }
  if (!ids->len && !issuers->len) return FALSE;
  NostrFilters *filters = nostr_filters_new();
  if (ids->len) {
    NostrFilter *awards = nostr_filter_new();
    nostr_filter_set_ids(awards, (const char *const *)ids->pdata, ids->len);
    nostr_filters_add(filters, awards); nostr_filter_free(awards);
  }
  if (issuers->len) {
    NostrFilter *definitions = nostr_filter_new();
    nostr_filter_set_kinds(definitions, (const int[]){ NOSTR_KIND_BADGE_DEFINITION }, 1);
    nostr_filter_set_authors(definitions, (const char *const *)issuers->pdata, issuers->len);
    for (guint i = 0; i < identifiers->len; i++)
      nostr_filter_tags_append(definitions, "d", g_ptr_array_index(identifiers, i), NULL);
    nostr_filters_add(filters, definitions); nostr_filter_free(definitions);
  }
  start_phase(l, 2, filters);
  return TRUE;
}
static GStrv
hints_of(Lookup *l)
{
  g_autoptr(GPtrArray) hints = g_ptr_array_new_with_free_func(g_free);
  for (gsize i = 0; i < l->n_pairs; i++) {
    const gchar *hint = l->pairs[i].award_relay;
    if (hint && *hint && gh_relay_url_validate(hint, NULL) &&
        !g_strv_contains((const gchar *const *)l->sources, hint))
      add_unique(hints, hint);
  }
  g_ptr_array_add(hints, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&hints), FALSE);
}
static void
finish_lookup(Lookup *l)
{
  GhBadgeService *s = l->service;
  Resolver r = { s, l->events };
  Cached *c = g_new0(Cached, 1);
  c->kind = !l->list ? GH_BADGE_LIST_NONE
    : nostr_event_get_kind(l->list->event) == NOSTR_KIND_PROFILE_BADGE_LIST ? GH_BADGE_LIST_CANONICAL
    : GH_BADGE_LIST_LEGACY;
  c->hints = hints_of(l);
  c->badges = badges_new();
  for (gsize i = 0; i < l->n_pairs; i++) {
    GhBadge *badge = resolve_pair(&r, &l->pairs[i], l->pubkey, TRUE);
    if (badge) g_ptr_array_add(c->badges, badge);
  }
  if (l->list) cache_put(s, l->list, c->kind == GH_BADGE_LIST_LEGACY ? LEGACY_IDENTIFIER : "");
  g_hash_table_insert(s->cache, g_strdup(l->pubkey), c);
  if (l->own) {
    g_clear_pointer(&s->awards, g_ptr_array_unref);
    s->awards = badges_new();
    for (guint i = 0; l->own_awards && i < l->own_awards->len; i++) {
      Ev *award = g_ptr_array_index(l->own_awards, i);
      NostrNip58ProfileBadge pair = { tag_value(award->event, "a"), award->id, NULL };
      GhBadge *badge = pair.badge_ref ? resolve_pair(&r, &pair, l->pubkey, TRUE) : NULL;
      if (badge) g_ptr_array_add(s->awards, badge);
    }
  }
  lookup_end(l, NULL);
  changed(s);
}
static gboolean
complete(gpointer data)
{
  Lookup *l = data;
  g_clear_pointer(&l->completion, g_source_unref);
  GCancellable *cancellable = g_task_get_cancellable(l->task);
  if (cancellable && g_cancellable_is_cancelled(cancellable)) {
    lookup_end(l, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Badge lookup cancelled"));
    return G_SOURCE_REMOVE;
  }
  if (!current(l->service)) {
    lookup_end(l, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED, "Account storage closed during the lookup"));
    return G_SOURCE_REMOVE;
  }
  if (!answered(l)) {
    /* Nothing replaces what is cached: the relays did not answer. */
    lookup_end(l, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, "No discovery relay answered the badge lookup"));
    return G_SOURCE_REMOVE;
  }
  if (l->phase == 1) {
    select_list(l);
    if (start_fetch(l)) return G_SOURCE_REMOVE;
  }
  finish_lookup(l);
  return G_SOURCE_REMOVE;
}
void
gh_badge_service_lookup_async(GhBadgeService *s, const gchar *pubkey, const gchar *const *approved,
                              GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GH_IS_BADGE_SERVICE(s));
  GTask *task = g_task_new(s, cancellable, callback, data);
  g_autofree gchar *lower = pubkey ? g_ascii_strdown(pubkey, -1) : NULL;
  if (!current(s)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account storage is not open");
    g_object_unref(task); return;
  }
  if (!hex64(lower)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "An account needs a 64-character public key");
    g_object_unref(task); return;
  }
  g_auto(GStrv) urls = sources(s, approved);
  if (!urls[0]) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No discovery relay is configured");
    g_object_unref(task); return;
  }
  Lookup *l = g_new0(Lookup, 1);
  l->service = g_object_ref(s); l->task = task;
  l->pubkey = g_steal_pointer(&lower);
  l->own = !g_strcmp0(l->pubkey, gh_social_lists_get_account(s->lists));
  l->sources = g_steal_pointer(&urls);
  l->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  l->events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ev_free);
  if (cancellable) l->cancel_handler = g_cancellable_connect(cancellable, G_CALLBACK(cancelled), l, NULL);
  g_ptr_array_add(s->lookups, l);
  NostrFilters *filters = nostr_filters_new();
  filter_for(filters, (const int[]){ NOSTR_KIND_PROFILE_BADGE_LIST }, 1, l->pubkey, 4);
  NostrFilter *legacy = filter_for(filters, (const int[]){ NOSTR_KIND_BADGE_SETS }, 1, l->pubkey, 4);
  nostr_filter_tags_append(legacy, "d", LEGACY_IDENTIFIER, NULL);
  if (l->own) {
    NostrFilter *awards = filter_for(filters, (const int[]){ NOSTR_KIND_BADGE_AWARD }, 1, NULL, 64);
    nostr_filter_tags_append(awards, "p", l->pubkey, NULL);
  }
  start_phase(l, 1, filters);
}
gboolean
gh_badge_service_lookup_finish(GhBadgeService *s, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, s), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ---- the own selection ------------------------------------------------------- */

/* Consecutive a (30009 address) / e (award id[, relay]) public tags of the
 * local document, as pairs. */
static GPtrArray *
document_badges(GhBadgeService *s, GNostrListDocument *document)
{
  const NostrListDocument *codec = gnostr_list_document_get_codec(document);
  gsize n = nostr_nip51_document_tag_count(codec, FALSE);
  GPtrArray *badges = badges_new();
  for (gsize i = 0; i + 1 < n && badges->len < MAX_PAIRS; i++) {
    const NostrListTag *a = nostr_nip51_document_tag(codec, FALSE, i);
    const NostrListTag *e = nostr_nip51_document_tag(codec, FALSE, i + 1);
    g_autofree gchar *issuer = NULL; const gchar *identifier = NULL;
    if (g_strcmp0(nostr_nip51_tag_field(a, 0), "a") || g_strcmp0(nostr_nip51_tag_field(e, 0), "e") ||
        !split_address(nostr_nip51_tag_field(a, 1), &issuer, &identifier) || !hex64(nostr_nip51_tag_field(e, 1)))
      continue;
    NostrNip58ProfileBadge pair = { nostr_nip51_tag_field(a, 1), nostr_nip51_tag_field(e, 1),
                                    nostr_nip51_tag_size(e) > 2 ? nostr_nip51_tag_field(e, 2) : NULL };
    Resolver r = { s, NULL };
    GhBadge *badge = resolve_pair(&r, &pair, gh_social_lists_get_account(s->lists), FALSE);
    /* Kept as chosen while its chain is not cached (the list is the user's),
     * flagged so nothing attributes it. */
    if (!badge) badge = unverified_badge(&pair, g_steal_pointer(&issuer), identifier);
    g_ptr_array_add(badges, badge);
    i++;
  }
  return badges;
}
GPtrArray *
gh_badge_service_get_selection(GhBadgeService *s, GhBadgeSelectionSource *source, GError **error)
{
  g_return_val_if_fail(GH_IS_BADGE_SERVICE(s), NULL);
  if (source) *source = GH_BADGE_SELECTION_EMPTY;
  if (!current(s)) { closed(error, "Account storage is not open"); return NULL; }
  GNostrListService *service = gh_social_lists_get_service(s->lists);
  g_autoptr(GNostrListDocument) document = service
    ? gnostr_list_service_lookup(service, NOSTR_KIND_PROFILE_BADGE_LIST, "") : NULL;
  if (document && (gnostr_list_document_get_local_revision(document) || gnostr_list_document_get_source_json(document))) {
    if (source) *source = GH_BADGE_SELECTION_LOCAL;
    return document_badges(s, document);
  }
  GhBadgeListKind kind = GH_BADGE_LIST_NONE;
  Ev *list = cached_list(s, gh_social_lists_get_account(s->lists), &kind);
  if (!list) return badges_new();
  Resolver r = { s, NULL };
  GPtrArray *badges = resolve_list(&r, list->event, gh_social_lists_get_account(s->lists), FALSE, TRUE);
  ev_free(list);
  if (source) *source = kind == GH_BADGE_LIST_CANONICAL ? GH_BADGE_SELECTION_CANONICAL : GH_BADGE_SELECTION_LEGACY;
  return badges;
}
/* The looked-up own list becomes the local canonical document: every pair
 * (verified or not) through set_selection, then its badge-set references;
 * a list the editor cannot hold whole is refused rather than cut. */
static gboolean
migrate_list(GhBadgeService *s, GPtrArray *selection, GError **error)
{
  GhBadgeListKind kind = GH_BADGE_LIST_NONE;
  Ev *list = cached_list(s, gh_social_lists_get_account(s->lists), &kind);
  gboolean overflow = FALSE;
  if (list) { NostrNip58ProfileBadge pairs[MAX_PAIRS]; list_pairs(list->event, pairs, MAX_PAIRS, &overflow); }
  if (overflow) {
    ev_free(list);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "The published badge list holds more badges than this version can carry over");
    return FALSE;
  }
  gboolean ok = gh_badge_service_set_selection(s, selection, error);
  const char *sets[64]; size_t n_sets = 0;
  if (ok && list && kind == GH_BADGE_LIST_CANONICAL &&
      nostr_nip58_profile_badge_sets(list->event, sets, G_N_ELEMENTS(sets), &n_sets) == 0 && n_sets) {
    g_autoptr(GNostrListDocument) document = gh_social_lists_ensure(s->lists, NOSTR_KIND_PROFILE_BADGE_LIST, "", error);
    ok = document != NULL;
    g_autoptr(GPtrArray) operations = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
    const NostrListDocument *codec = ok ? gnostr_list_document_get_codec(document) : NULL;
    for (size_t i = 0; ok && i < n_sets; i++) {
      gboolean present = FALSE;
      for (gsize t = 0; !present && t < nostr_nip51_document_tag_count(codec, FALSE); t++) {
        const NostrListTag *tag = nostr_nip51_document_tag(codec, FALSE, t);
        present = !g_strcmp0(nostr_nip51_tag_field(tag, 0), "a") && !g_strcmp0(nostr_nip51_tag_field(tag, 1), sets[i]);
      }
      const gchar *fields[] = { "a", sets[i] };
      if (!present) g_ptr_array_add(operations, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, FALSE, fields, 2, 0, 0));
    }
    if (ok && operations->len) {
      g_autoptr(GNostrListDocument) saved = gh_social_lists_apply(s->lists, NOSTR_KIND_PROFILE_BADGE_LIST, "",
        gnostr_list_document_get_local_revision(document), operations, error);
      ok = saved != NULL;
    }
  }
  if (list) ev_free(list);
  return ok;
}
static gchar *
tag_key(const NostrListTag *tag)
{
  GString *key = g_string_new(NULL);
  for (gsize i = 0; i < nostr_nip51_tag_size(tag); i++) {
    const gchar *field = nostr_nip51_tag_field(tag, i);
    g_string_append(key, field ? field : ""); g_string_append_c(key, '\n');
  }
  return g_string_free(key, FALSE);
}
gboolean
gh_badge_service_set_selection(GhBadgeService *s, GPtrArray *badges, GError **error)
{
  g_return_val_if_fail(GH_IS_BADGE_SERVICE(s), FALSE);
  if (!current(s)) return closed(error, "Account storage is not open");
  for (guint i = 0; badges && i < badges->len; i++) {
    GhBadge *badge = g_ptr_array_index(badges, i);
    if (!split_address(badge->address, NULL, NULL) || !hex64(badge->award_id) || i >= MAX_PAIRS) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "A badge needs a definition address and an award id");
      return FALSE;
    }
  }
  g_autoptr(GNostrListDocument) document = gh_social_lists_ensure(s->lists, NOSTR_KIND_PROFILE_BADGE_LIST, "", error);
  if (!document) return FALSE;
  g_autoptr(GPtrArray) operations = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  const NostrListDocument *codec = gnostr_list_document_get_codec(document);
  /* Replace every pair; set references (a to 30008) and anything else stay. */
  for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, FALSE); i++) {
    const NostrListTag *tag = nostr_nip51_document_tag(codec, FALSE, i);
    const gchar *name = nostr_nip51_tag_field(tag, 0), *value = nostr_nip51_tag_field(tag, 1);
    gboolean pair_tag = !g_strcmp0(name, "e") || (!g_strcmp0(name, "a") && value && g_str_has_prefix(value, "30009:"));
    g_autofree gchar *key = tag_key(tag);
    if (!pair_tag || g_hash_table_contains(seen, key)) continue;
    g_hash_table_add(seen, g_steal_pointer(&key));
    g_ptr_array_add(operations, gnostr_list_operation_remove(FALSE, tag, G_MAXUINT));
  }
  for (guint i = 0; badges && i < badges->len; i++) {
    GhBadge *badge = g_ptr_array_index(badges, i);
    const gchar *a[] = { "a", badge->address };
    const gchar *e[] = { "e", badge->award_id, badge->award_relay };
    g_ptr_array_add(operations, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, FALSE, a, 2, 0, 0));
    g_ptr_array_add(operations, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, FALSE, e, badge->award_relay ? 3 : 2, 0, 0));
  }
  g_autoptr(GNostrListDocument) saved = gh_social_lists_apply(s->lists, NOSTR_KIND_PROFILE_BADGE_LIST, "",
    gnostr_list_document_get_local_revision(document), operations, error);
  if (saved) changed(s);
  return saved != NULL;
}
GStrv
gh_badge_service_dup_targets(GhBadgeService *s)
{
  g_return_val_if_fail(GH_IS_BADGE_SERVICE(s), NULL);
  return current(s) ? gh_social_lists_dup_public_targets(s->lists) : g_strdupv((gchar *[]){ NULL });
}
static void
published(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  GhBadgeService *s = g_task_get_source_object(task);
  GError *error = NULL;
  g_autoptr(GVariant) outcomes = gnostr_list_service_publish_finish(GNOSTR_LIST_SERVICE(source), result, &error);
  if (!outcomes) { g_task_return_error(task, error); changed(s); return; }
  gboolean accepted = FALSE;
  GVariantIter iter; const gchar *url; gint outcome;
  g_variant_iter_init(&iter, outcomes);
  while (g_variant_iter_next(&iter, "{&si}", &url, &outcome))
    accepted |= outcome == GNOSTR_LIST_ACCEPTED || outcome == GNOSTR_LIST_DUPLICATE;
  if (accepted) g_task_return_boolean(task, TRUE);
  else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "No relay accepted the profile badges");
  changed(s);
}
void
gh_badge_service_publish_async(GhBadgeService *s, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GH_IS_BADGE_SERVICE(s));
  GTask *task = g_task_new(s, cancellable, callback, data);
  GError *error = NULL;
  GhBadgeSelectionSource source = GH_BADGE_SELECTION_EMPTY;
  g_autoptr(GPtrArray) selection = gh_badge_service_get_selection(s, &source, &error);
  /* A list seeded from a looked-up canonical or legacy event becomes the
   * local canonical document now; the legacy 30008 is never deleted. */
  if (selection && source != GH_BADGE_SELECTION_LOCAL && !migrate_list(s, selection, &error))
    g_clear_pointer(&selection, g_ptr_array_unref);
  if (!selection) { g_task_return_error(task, error); g_object_unref(task); return; }
  gnostr_list_service_publish_async(gh_social_lists_get_service(s->lists), NOSTR_KIND_PROFILE_BADGE_LIST, "",
    GNOSTR_LIST_APPROVE_PUBLIC | GNOSTR_LIST_APPROVE_PARTIAL, cancellable, published, task);
}
gboolean
gh_badge_service_publish_finish(GhBadgeService *s, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, s), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ---- artwork ------------------------------------------------------------------- */

gboolean
gh_badge_service_get_artwork_enabled(GhBadgeService *s)
{
  g_return_val_if_fail(GH_IS_BADGE_SERVICE(s), FALSE);
  return s->settings && g_settings_get_boolean(s->settings, gh_web_content_setting(GH_WEB_BADGE));
}
/* One artwork load: the account generation it was asked under, and the
 * caller's cancellable forwarded into the load's own (cancelled on close). */
typedef struct { guint64 generation; GCancellable *caller; gulong handler; } ArtworkLoad;
static void
artwork_load_free(gpointer data)
{
  ArtworkLoad *load = data;
  if (load->caller && load->handler) g_cancellable_disconnect(load->caller, load->handler);
  g_clear_object(&load->caller); g_free(load);
}
static void forward_cancel(GCancellable *caller, gpointer bound) { (void)caller; g_cancellable_cancel(bound); }
static void
artwork_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  GhBadgeService *s = g_task_get_source_object(task);
  ArtworkLoad *load = g_task_get_task_data(task);
  g_ptr_array_remove(s->artwork, g_task_get_cancellable(task));
  GError *error = NULL;
  g_autoptr(GhWebResult) loaded = gh_web_content_load_finish(GH_WEB_CONTENT(source), result, &error);
  if (!loaded) { g_task_return_error(task, error); return; }
  /* The account closed or changed meanwhile: its artwork is not shown. */
  if (load->generation != s->generation || !current(s)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Account storage closed during the load");
    return;
  }
  if (!loaded->texture) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The badge artwork is not an image");
    return;
  }
  g_task_return_pointer(task, g_object_ref(loaded->texture), g_object_unref);
}
void
gh_badge_service_load_artwork_async(GhBadgeService *s, const gchar *url, GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GH_IS_BADGE_SERVICE(s));
  /* The load's own cancellable follows the caller's and the account binding. */
  g_autoptr(GCancellable) bound = g_cancellable_new();
  GTask *task = g_task_new(s, bound, callback, data);
  ArtworkLoad *load = g_new0(ArtworkLoad, 1);
  load->generation = s->generation;
  g_task_set_task_data(task, load, artwork_load_free);
  /* Never without the Load Remote Images consent (PD-2): no request leaves. */
  if (!current(s)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account storage is not open");
    g_object_unref(task); return;
  }
  if (!gh_badge_service_get_artwork_enabled(s)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Loading remote images is off");
    g_object_unref(task); return;
  }
  if (!url || !gh_link_policy_can_preview(url)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Badge artwork needs an https address");
    g_object_unref(task); return;
  }
  if (!s->web) s->web = gh_web_content_new(s->settings, s->http, s->http_data);
  if (cancellable) {
    load->caller = g_object_ref(cancellable);
    load->handler = g_cancellable_connect(cancellable, G_CALLBACK(forward_cancel), bound, NULL);
  }
  g_ptr_array_add(s->artwork, g_object_ref(bound));
  gh_web_content_load_async(s->web, url, GH_WEB_BADGE, bound, artwork_loaded, task);
}
static void
cancel_artwork(GhBadgeService *s)
{
  for (guint i = 0; i < s->artwork->len; i++) g_cancellable_cancel(g_ptr_array_index(s->artwork, i));
  g_ptr_array_set_size(s->artwork, 0);
}
GdkTexture *
gh_badge_service_load_artwork_finish(GhBadgeService *s, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, s), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
void
gh_badge_service_set_http_transport(GhBadgeService *s, const GhHttpTransport *transport, gpointer data)
{
  g_return_if_fail(GH_IS_BADGE_SERVICE(s));
  s->http = transport; s->http_data = data;
  g_clear_object(&s->web);
}

/* ---- binding ------------------------------------------------------------------- */

void
gh_badge_service_close(GhBadgeService *s)
{
  g_return_if_fail(GH_IS_BADGE_SERVICE(s));
  while (s->lookups->len) {
    Lookup *l = g_ptr_array_index(s->lookups, s->lookups->len - 1);
    lookup_end(l, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED, "Account storage closed"));
  }
  cancel_artwork(s);
  g_hash_table_remove_all(s->cache);
  g_clear_pointer(&s->awards, g_ptr_array_unref);
  s->store = NULL; s->generation = 0;
  changed(s);
}
void
gh_badge_service_bind(GhBadgeService *s, GhStore *store, guint64 generation)
{
  g_return_if_fail(GH_IS_BADGE_SERVICE(s));
  gh_badge_service_close(s);
  s->store = store; s->generation = generation;
  changed(s);
}
static void
bound(GhAccountStore *store, GhBadgeService *s)
{
  gh_badge_service_bind(s, gh_account_store_get_store(store), gh_account_store_get_generation(store));
}
static void closing(GhAccountStore *store, GhBadgeService *s) { (void)store; gh_badge_service_close(s); }
static void
accounts_changed(GhAccountController *accounts, GhBadgeService *s)
{
  if (s->store && !gh_account_controller_is_current(accounts, s->generation)) gh_badge_service_close(s);
}
static void
lists_changed(GhSocialLists *lists, GNostrListDocument *document, GhBadgeService *s)
{
  (void)lists;
  if (s->store && gnostr_list_document_get_kind(document) == NOSTR_KIND_PROFILE_BADGE_LIST) changed(s);
}
static void
dispose(GObject *object)
{
  GhBadgeService *s = GH_BADGE_SERVICE(object);
  while (s->lookups->len)
    lookup_end(g_ptr_array_index(s->lookups, s->lookups->len - 1),
               g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED, "Badge service released"));
  cancel_artwork(s);
  s->store = NULL; s->generation = 0;
  g_hash_table_remove_all(s->cache);
  g_clear_pointer(&s->awards, g_ptr_array_unref);
  if (s->account_store) g_signal_handlers_disconnect_by_data(s->account_store, s);
  g_clear_object(&s->account_store);
  if (s->lists) g_signal_handlers_disconnect_by_data(s->lists, s);
  g_clear_object(&s->lists);
  if (s->network.accounts) g_signal_handlers_disconnect_by_data(s->network.accounts, s);
  gh_list_adapter_network_clear(&s->network);
  g_clear_object(&s->settings); g_clear_object(&s->web);
  G_OBJECT_CLASS(gh_badge_service_parent_class)->dispose(object);
}
static void
finalize(GObject *object)
{
  GhBadgeService *s = GH_BADGE_SERVICE(object);
  g_hash_table_unref(s->cache); g_ptr_array_unref(s->lookups); g_ptr_array_unref(s->artwork);
  G_OBJECT_CLASS(gh_badge_service_parent_class)->finalize(object);
}
static void
gh_badge_service_init(GhBadgeService *s)
{
  s->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cached_free);
  s->lookups = g_ptr_array_new();
  s->artwork = g_ptr_array_new_with_free_func(g_object_unref);
}
static void
gh_badge_service_class_init(GhBadgeServiceClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose;
  G_OBJECT_CLASS(klass)->finalize = finalize;
  g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
GhBadgeService *
gh_badge_service_new(GhSocialLists *lists, GhAccountStore *store, const GhListAdapterNetwork *network,
                     GSettings *settings)
{
  g_return_val_if_fail(GH_IS_SOCIAL_LISTS(lists), NULL);
  GhBadgeService *s = g_object_new(GH_TYPE_BADGE_SERVICE, NULL);
  s->lists = g_object_ref(lists);
  g_signal_connect(lists, "document-changed", G_CALLBACK(lists_changed), s);
  s->settings = settings ? g_object_ref(settings) : NULL;
  if (network) gh_list_adapter_network_copy(&s->network, network);
  if (s->network.accounts)
    g_signal_connect(s->network.accounts, "changed", G_CALLBACK(accounts_changed), s);
  if (store) {
    s->account_store = g_object_ref(store);
    g_signal_connect(store, "store-bound", G_CALLBACK(bound), s);
    g_signal_connect(store, "store-closing", G_CALLBACK(closing), s);
    if (gh_account_store_get_store(store)) bound(store, s);
  }
  return s;
}

/* ---- the UI backend ------------------------------------------------------------- */

#define SELF(owner) GH_BADGE_SERVICE(owner)
static guint64 b_generation(GObject *o) { return SELF(o)->generation; }
static const gchar *b_account(GObject *o) { return current(SELF(o)) ? gh_social_lists_get_account(SELF(o)->lists) : NULL; }
static GPtrArray *b_badges(GObject *o, const gchar *p) { return gh_badge_service_get_badges(SELF(o), p); }
static GhBadgeListKind b_kind(GObject *o, const gchar *p) { return gh_badge_service_get_list_kind(SELF(o), p); }
static GStrv b_hints(GObject *o, const gchar *p) { return gh_badge_service_dup_hints(SELF(o), p); }
static GStrv b_sources(GObject *o) { return gh_badge_service_dup_sources(SELF(o)); }
static void b_lookup(GObject *o, const gchar *p, const gchar *const *a, GCancellable *c, GAsyncReadyCallback cb, gpointer d)
{ gh_badge_service_lookup_async(SELF(o), p, a, c, cb, d); }
static gboolean b_lookup_finish(GObject *o, GAsyncResult *r, GError **e) { return gh_badge_service_lookup_finish(SELF(o), r, e); }
static GPtrArray *b_awards(GObject *o) { return gh_badge_service_get_awards(SELF(o)); }
static GPtrArray *b_selection(GObject *o, GhBadgeSelectionSource *s, GError **e) { return gh_badge_service_get_selection(SELF(o), s, e); }
static gboolean b_set_selection(GObject *o, GPtrArray *b, GError **e) { return gh_badge_service_set_selection(SELF(o), b, e); }
static GStrv b_targets(GObject *o) { return gh_badge_service_dup_targets(SELF(o)); }
static void b_publish(GObject *o, GCancellable *c, GAsyncReadyCallback cb, gpointer d) { gh_badge_service_publish_async(SELF(o), c, cb, d); }
static gboolean b_publish_finish(GObject *o, GAsyncResult *r, GError **e) { return gh_badge_service_publish_finish(SELF(o), r, e); }
static gboolean b_artwork_enabled(GObject *o) { return gh_badge_service_get_artwork_enabled(SELF(o)); }
static void b_artwork(GObject *o, const gchar *u, GCancellable *c, GAsyncReadyCallback cb, gpointer d)
{ gh_badge_service_load_artwork_async(SELF(o), u, c, cb, d); }
static GdkTexture *b_artwork_finish(GObject *o, GAsyncResult *r, GError **e) { return gh_badge_service_load_artwork_finish(SELF(o), r, e); }
GhBadgesBackend
gh_badge_service_backend(GhBadgeService *s)
{
  return (GhBadgesBackend){
    .owner = G_OBJECT(s), .generation = b_generation, .account = b_account,
    .badges = b_badges, .list_kind = b_kind, .dup_hints = b_hints, .dup_sources = b_sources,
    .lookup_async = b_lookup, .lookup_finish = b_lookup_finish,
    .awards = b_awards, .selection = b_selection, .set_selection = b_set_selection,
    .dup_targets = b_targets, .publish_async = b_publish, .publish_finish = b_publish_finish,
    .artwork_enabled = b_artwork_enabled, .artwork_async = b_artwork, .artwork_finish = b_artwork_finish,
  };
}
