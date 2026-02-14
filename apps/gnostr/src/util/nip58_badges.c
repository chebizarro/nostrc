/*
 * nip58_badges.c - NIP-58 Badge implementation for GNostr
 *
 * Implements badge fetching, parsing, and caching for:
 *   - Kind 30009: Badge Definition
 *   - Kind 8: Badge Award
 *   - Kind 30008: Profile Badges
 */

#define G_LOG_DOMAIN "nip58-badges"

#include "nip58_badges.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include "../ui/gnostr-avatar-cache.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include <nostr-gobject-1.0/nostr_json.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <json.h>
#include <string.h>

/* ============== Badge Definition ============== */

GnostrBadgeDefinition *
gnostr_badge_definition_new(void)
{
  return g_new0(GnostrBadgeDefinition, 1);
}

void
gnostr_badge_definition_free(GnostrBadgeDefinition *def)
{
  if (!def) return;
  g_free(def->identifier);
  g_free(def->name);
  g_free(def->description);
  g_free(def->image_url);
  g_free(def->thumb_url);
  g_free(def->issuer_pubkey);
  g_free(def->event_id);
  g_free(def);
}

GnostrBadgeDefinition *
gnostr_badge_definition_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  /* Deserialize to NostrEvent using the facade */
  NostrEvent event = {0};
  if (nostr_event_deserialize(&event, event_json) != 0) {
    g_warning("badge_definition: failed to parse JSON");
    return NULL;
  }

  /* Verify kind */
  if (event.kind != NIP58_KIND_BADGE_DEFINITION) {
    free(event.id);
    free(event.pubkey);
    free(event.content);
    free(event.sig);
    if (event.tags) nostr_tags_free(event.tags);
    return NULL;
  }

  GnostrBadgeDefinition *def = gnostr_badge_definition_new();

  /* Extract event metadata */
  def->event_id = g_strdup(event.id);
  def->issuer_pubkey = g_strdup(event.pubkey);
  def->created_at = event.created_at;

  /* Parse tags for badge metadata using NostrTags API */
  if (event.tags) {
    size_t n_tags = nostr_tags_size(event.tags);
    for (size_t i = 0; i < n_tags; i++) {
      NostrTag *tag = nostr_tags_get(event.tags, i);
      if (!tag) continue;
      size_t tag_len = nostr_tag_size(tag);
      if (tag_len < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "d") == 0) {
        def->identifier = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "name") == 0) {
        def->name = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "description") == 0) {
        def->description = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "image") == 0) {
        def->image_url = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "thumb") == 0) {
        def->thumb_url = g_strdup(tag_value);
      }
    }
  }

  /* Free internal event fields */
  free(event.id);
  free(event.pubkey);
  free(event.content);
  free(event.sig);
  if (event.tags) nostr_tags_free(event.tags);

  /* Validate: must have identifier */
  if (!def->identifier) {
    g_debug("badge_definition: missing 'd' tag identifier");
    gnostr_badge_definition_free(def);
    return NULL;
  }

  g_debug("badge_definition: parsed '%s' (id=%s) from %s",
          def->name ? def->name : def->identifier,
          def->identifier,
          def->issuer_pubkey ? def->issuer_pubkey : "unknown");

  return def;
}

gchar *
gnostr_badge_definition_get_naddr(const GnostrBadgeDefinition *def)
{
  if (!def || !def->issuer_pubkey || !def->identifier) return NULL;
  return g_strdup_printf("%d:%s:%s",
                         NIP58_KIND_BADGE_DEFINITION,
                         def->issuer_pubkey,
                         def->identifier);
}

/* ============== Badge Award ============== */

GnostrBadgeAward *
gnostr_badge_award_new(void)
{
  GnostrBadgeAward *award = g_new0(GnostrBadgeAward, 1);
  award->awardees = g_ptr_array_new_with_free_func(g_free);
  return award;
}

void
gnostr_badge_award_free(GnostrBadgeAward *award)
{
  if (!award) return;
  g_free(award->event_id);
  g_free(award->badge_ref);
  g_free(award->issuer_pubkey);
  g_ptr_array_unref(award->awardees);
  g_free(award);
}

GnostrBadgeAward *
gnostr_badge_award_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  /* Deserialize to NostrEvent using the facade */
  NostrEvent event = {0};
  if (nostr_event_deserialize(&event, event_json) != 0) {
    g_warning("badge_award: failed to parse JSON");
    return NULL;
  }

  /* Verify kind */
  if (event.kind != NIP58_KIND_BADGE_AWARD) {
    free(event.id);
    free(event.pubkey);
    free(event.content);
    free(event.sig);
    if (event.tags) nostr_tags_free(event.tags);
    return NULL;
  }

  GnostrBadgeAward *award = gnostr_badge_award_new();

  /* Extract event metadata */
  award->event_id = g_strdup(event.id);
  award->issuer_pubkey = g_strdup(event.pubkey);
  award->created_at = event.created_at;

  /* Parse tags using NostrTags API */
  if (event.tags) {
    size_t n_tags = nostr_tags_size(event.tags);
    for (size_t i = 0; i < n_tags; i++) {
      NostrTag *tag = nostr_tags_get(event.tags, i);
      if (!tag) continue;
      size_t tag_len = nostr_tag_size(tag);
      if (tag_len < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "a") == 0 && !award->badge_ref) {
        /* "a" tag references the badge definition */
        award->badge_ref = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "p") == 0) {
        /* "p" tags are awardees */
        g_ptr_array_add(award->awardees, g_strdup(tag_value));
      }
    }
  }

  /* Free internal event fields */
  free(event.id);
  free(event.pubkey);
  free(event.content);
  free(event.sig);
  if (event.tags) nostr_tags_free(event.tags);

  /* Validate: must have badge reference and at least one awardee */
  if (!award->badge_ref || award->awardees->len == 0) {
    g_debug("badge_award: missing 'a' tag or no awardees");
    gnostr_badge_award_free(award);
    return NULL;
  }

  g_debug("badge_award: parsed award for badge %s to %u awardees",
          award->badge_ref, award->awardees->len);

  return award;
}

/* ============== Profile Badge ============== */

GnostrProfileBadge *
gnostr_profile_badge_new(void)
{
  return g_new0(GnostrProfileBadge, 1);
}

void
gnostr_profile_badge_free(GnostrProfileBadge *badge)
{
  if (!badge) return;
  gnostr_badge_definition_free(badge->definition);
  g_free(badge->award_event_id);
  g_free(badge);
}

GPtrArray *
gnostr_profile_badges_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  /* Deserialize to NostrEvent using the facade */
  NostrEvent event = {0};
  if (nostr_event_deserialize(&event, event_json) != 0) {
    g_warning("profile_badges: failed to parse JSON");
    return NULL;
  }

  /* Verify kind */
  if (event.kind != NIP58_KIND_PROFILE_BADGES) {
    free(event.id);
    free(event.pubkey);
    free(event.content);
    free(event.sig);
    if (event.tags) nostr_tags_free(event.tags);
    return NULL;
  }

  /* Verify d tag is "profile_badges" */
  gboolean has_profile_badges_d = FALSE;
  if (event.tags) {
    size_t n_tags = nostr_tags_size(event.tags);
    for (size_t i = 0; i < n_tags; i++) {
      NostrTag *tag = nostr_tags_get(event.tags, i);
      if (!tag) continue;
      size_t tag_len = nostr_tag_size(tag);
      if (tag_len < 2) continue;
      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
      if (g_strcmp0(tag_name, "d") == 0 && g_strcmp0(tag_value, "profile_badges") == 0) {
        has_profile_badges_d = TRUE;
        break;
      }
    }
  }

  if (!has_profile_badges_d) {
    g_debug("profile_badges: missing d=profile_badges tag");
    free(event.id);
    free(event.pubkey);
    free(event.content);
    free(event.sig);
    if (event.tags) nostr_tags_free(event.tags);
    return NULL;
  }

  /* Parse badge references: alternating "a" (definition) and "e" (award) tags */
  GPtrArray *badges = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_profile_badge_free);
  gchar *pending_def_ref = NULL;
  gint64 position = 0;

  if (event.tags) {
    size_t n_tags = nostr_tags_size(event.tags);
    for (size_t i = 0; i < n_tags; i++) {
      NostrTag *tag = nostr_tags_get(event.tags, i);
      if (!tag) continue;
      size_t tag_len = nostr_tag_size(tag);
      if (tag_len < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "a") == 0) {
        /* Badge definition reference - store for next "e" tag */
        g_free(pending_def_ref);
        pending_def_ref = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "e") == 0 && pending_def_ref) {
        /* Award event ID - create profile badge entry */
        GnostrProfileBadge *badge = gnostr_profile_badge_new();
        badge->award_event_id = g_strdup(tag_value);
        badge->position = position++;

        /* Create placeholder definition with naddr reference */
        badge->definition = gnostr_badge_definition_new();
        /* Parse naddr: "30009:pubkey:identifier" */
        gchar **parts = g_strsplit(pending_def_ref, ":", 3);
        if (parts && parts[0] && parts[1] && parts[2]) {
          badge->definition->issuer_pubkey = g_strdup(parts[1]);
          badge->definition->identifier = g_strdup(parts[2]);
        }
        g_strfreev(parts);

        g_ptr_array_add(badges, badge);
        g_clear_pointer(&pending_def_ref, g_free);
      }
    }
  }

  g_free(pending_def_ref);

  /* Free internal event fields */
  free(event.id);
  free(event.pubkey);
  free(event.content);
  free(event.sig);
  if (event.tags) nostr_tags_free(event.tags);

  g_debug("profile_badges: parsed %u badges from profile", badges->len);
  return badges;
}

/* ============== Async Fetch Context ============== */

typedef struct _BadgeFetchCtx {
  GnostrBadgeFetchCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GNostrPool *pool;
  gchar *pubkey_hex;
  GPtrArray *badges;           /* GnostrProfileBadge* */
  guint pending_definitions;   /* Count of definitions still being fetched */
} BadgeFetchCtx;

static void
badge_fetch_ctx_free(BadgeFetchCtx *ctx)
{
  if (!ctx) return;
  g_free(ctx->pubkey_hex);
  g_clear_object(&ctx->cancellable);
  g_clear_object(&ctx->pool);
  if (ctx->badges) g_ptr_array_unref(ctx->badges);
  g_free(ctx);
}

static void
badge_fetch_complete(BadgeFetchCtx *ctx)
{
  if (ctx->callback) {
    ctx->callback(ctx->badges, ctx->user_data);
    ctx->badges = NULL; /* Transfer ownership to callback */
  }
  badge_fetch_ctx_free(ctx);
}

/* Forward declarations */
static void on_profile_badges_fetched(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_badge_definition_fetched(GObject *source, GAsyncResult *res, gpointer user_data);
static void fetch_badge_definitions(BadgeFetchCtx *ctx);

/* ============== Profile Badges Fetch ============== */

void
gnostr_fetch_profile_badges_async(const gchar *pubkey_hex,
                                   GCancellable *cancellable,
                                   GnostrBadgeFetchCallback callback,
                                   gpointer user_data)
{
  g_return_if_fail(pubkey_hex != NULL && strlen(pubkey_hex) == 64);

  BadgeFetchCtx *ctx = g_new0(BadgeFetchCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->pool = gnostr_pool_new();
  ctx->badges = NULL;
  ctx->pending_definitions = 0;

  g_debug("fetch_profile_badges: fetching badges for %s", pubkey_hex);

  /* Get relay URLs */
  GPtrArray *relay_urls = gnostr_get_read_relay_urls();
  if (!relay_urls || relay_urls->len == 0) {
    g_debug("fetch_profile_badges: no relays configured");
    if (relay_urls) g_ptr_array_unref(relay_urls);
    if (callback) callback(NULL, user_data);
    badge_fetch_ctx_free(ctx);
    return;
  }

  /* Build filter for kind 30008 with d=profile_badges */
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { NIP58_KIND_PROFILE_BADGES };
  nostr_filter_set_kinds(filter, kinds, 1);
  const char *authors[] = { pubkey_hex };
  nostr_filter_set_authors(filter, authors, 1);

  /* Convert relay URLs to array */
  const char **urls = g_new0(const char *, relay_urls->len + 1);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

    gnostr_pool_sync_relays(ctx->pool, (const gchar **)urls, relay_urls->len);
  {
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    gnostr_pool_query_async(ctx->pool, _qf, ctx->cancellable, on_profile_badges_fetched, ctx);
  }

  g_free(urls);
  nostr_filter_free(filter);
  g_ptr_array_unref(relay_urls);
}

static void
on_profile_badges_fetched(GObject *source, GAsyncResult *res, gpointer user_data)
{
  BadgeFetchCtx *ctx = user_data;
  GNostrPool *pool = GNOSTR_POOL(source);
  GError *error = NULL;

  GPtrArray *events = gnostr_pool_query_finish(pool, res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("fetch_profile_badges: query failed: %s", error->message);
    }
    g_error_free(error);
    badge_fetch_complete(ctx);
    return;
  }

  if (!events || events->len == 0) {
    g_debug("fetch_profile_badges: no profile badges event found for %s", ctx->pubkey_hex);
    if (events) g_ptr_array_unref(events);
    badge_fetch_complete(ctx);
    return;
  }

  /* Parse the first (most recent) profile badges event */
  /* Note: query_single returns array of JSON strings, not NostrEvent* */
  const gchar *event_json = g_ptr_array_index(events, 0);
  if (!event_json) {
    g_debug("fetch_profile_badges: null event JSON at index 0");
    g_ptr_array_unref(events);
    badge_fetch_complete(ctx);
    return;
  }
  ctx->badges = gnostr_profile_badges_parse(event_json);
  g_ptr_array_unref(events);

  if (!ctx->badges || ctx->badges->len == 0) {
    g_debug("fetch_profile_badges: no badges in profile_badges event");
    badge_fetch_complete(ctx);
    return;
  }

  g_debug("fetch_profile_badges: found %u badges, fetching definitions", ctx->badges->len);

  /* Fetch badge definitions */
  fetch_badge_definitions(ctx);
}

static void
fetch_badge_definitions(BadgeFetchCtx *ctx)
{
  if (!ctx->badges || ctx->badges->len == 0) {
    badge_fetch_complete(ctx);
    return;
  }

  /* Get relay URLs */
  GPtrArray *relay_urls = gnostr_get_read_relay_urls();
  if (!relay_urls || relay_urls->len == 0) {
    if (relay_urls) g_ptr_array_unref(relay_urls);
    badge_fetch_complete(ctx);
    return;
  }

  /* Convert relay URLs to array */
  const char **urls = g_new0(const char *, relay_urls->len + 1);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  ctx->pending_definitions = ctx->badges->len;

  /* Fetch each badge definition */
  for (guint i = 0; i < ctx->badges->len; i++) {
    GnostrProfileBadge *badge = g_ptr_array_index(ctx->badges, i);
    if (!badge->definition || !badge->definition->issuer_pubkey || !badge->definition->identifier) {
      ctx->pending_definitions--;
      continue;
    }

    /* Build filter for kind 30009 with specific author and d tag */
    NostrFilter *filter = nostr_filter_new();
    int kinds[] = { NIP58_KIND_BADGE_DEFINITION };
    nostr_filter_set_kinds(filter, kinds, 1);
    const char *authors[] = { badge->definition->issuer_pubkey };
    nostr_filter_set_authors(filter, authors, 1);
    /* Set #d tag filter using nostr_filter_tags_append */
    nostr_filter_tags_append(filter, "#d", badge->definition->identifier, NULL);

    /* Store badge index in user_data via pointer arithmetic */
    gpointer badge_ptr = badge;

        gnostr_pool_sync_relays(ctx->pool, (const gchar **)urls, relay_urls->len);
    {
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      gnostr_pool_query_async(ctx->pool, _qf, ctx->cancellable, on_badge_definition_fetched, badge_ptr);
    }

    nostr_filter_free(filter);
  }

  g_free(urls);
  g_ptr_array_unref(relay_urls);

  /* If no definitions to fetch, complete now */
  if (ctx->pending_definitions == 0) {
    badge_fetch_complete(ctx);
  }
}

/* Definition fetch context - we need to track the badge and main context */
typedef struct {
  BadgeFetchCtx *main_ctx;
  GnostrProfileBadge *badge;
} DefFetchCtx;

static void
on_badge_definition_fetched(GObject *source, GAsyncResult *res, gpointer user_data)
{
  /* We passed the badge pointer directly - find the main context from it */
  GnostrProfileBadge *badge = user_data;
  GNostrPool *pool = GNOSTR_POOL(source);
  GError *error = NULL;

  GPtrArray *events = gnostr_pool_query_finish(pool, res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("fetch_badge_definition: query failed: %s", error->message);
    }
    g_error_free(error);
    if (events) g_ptr_array_unref(events);
    return;
  }

  if (events && events->len > 0) {
    /* Note: query_single returns array of JSON strings, not NostrEvent* */
    const gchar *event_json = g_ptr_array_index(events, 0);
    if (event_json) {
      /* Parse and update the badge definition */
      GnostrBadgeDefinition *def = gnostr_badge_definition_parse(event_json);
      if (def) {
        /* Replace placeholder with full definition */
        gnostr_badge_definition_free(badge->definition);
        badge->definition = def;

        /* Prefetch badge image */
        if (def->thumb_url) {
          gnostr_badge_prefetch_image(def->thumb_url);
        } else if (def->image_url) {
          gnostr_badge_prefetch_image(def->image_url);
        }

        g_debug("fetch_badge_definition: loaded '%s'",
                def->name ? def->name : def->identifier);
      }
    }
  }

  if (events) g_ptr_array_unref(events);

  /* Note: We can't easily track pending_definitions here without a proper context.
   * The async completion is handled by the main context timeout or when all complete. */
}

/* ============== Single Definition Fetch ============== */

typedef struct _DefSingleFetchCtx {
  GnostrBadgeDefinitionCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GNostrPool *pool;
  gchar *naddr;
} DefSingleFetchCtx;

static void
def_single_fetch_ctx_free(DefSingleFetchCtx *ctx)
{
  if (!ctx) return;
  g_free(ctx->naddr);
  g_clear_object(&ctx->cancellable);
  g_clear_object(&ctx->pool);
  g_free(ctx);
}

static void
on_single_definition_fetched(GObject *source, GAsyncResult *res, gpointer user_data)
{
  DefSingleFetchCtx *ctx = user_data;
  GNostrPool *pool = GNOSTR_POOL(source);
  GError *error = NULL;

  GPtrArray *events = gnostr_pool_query_finish(pool, res, &error);
  GnostrBadgeDefinition *def = NULL;

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("fetch_badge_definition: query failed: %s", error->message);
    }
    g_error_free(error);
  } else if (events && events->len > 0) {
    /* Note: query_single returns array of JSON strings, not NostrEvent* */
    const gchar *event_json = g_ptr_array_index(events, 0);
    if (event_json) {
      def = gnostr_badge_definition_parse(event_json);
    }
  }

  if (events) g_ptr_array_unref(events);

  if (ctx->callback) {
    ctx->callback(def, ctx->user_data);
  }

  if (def) gnostr_badge_definition_free(def);
  def_single_fetch_ctx_free(ctx);
}

void
gnostr_fetch_badge_definition_async(const gchar *naddr,
                                     GCancellable *cancellable,
                                     GnostrBadgeDefinitionCallback callback,
                                     gpointer user_data)
{
  g_return_if_fail(naddr != NULL);

  /* Parse naddr: "30009:pubkey:identifier" */
  gchar **parts = g_strsplit(naddr, ":", 3);
  if (!parts || !parts[0] || !parts[1] || !parts[2]) {
    g_warning("fetch_badge_definition: invalid naddr format: %s", naddr);
    g_strfreev(parts);
    if (callback) callback(NULL, user_data);
    return;
  }

  DefSingleFetchCtx *ctx = g_new0(DefSingleFetchCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->pool = gnostr_pool_new();
  ctx->naddr = g_strdup(naddr);

  /* Get relay URLs */
  GPtrArray *relay_urls = gnostr_get_read_relay_urls();
  if (!relay_urls || relay_urls->len == 0) {
    g_strfreev(parts);
    if (relay_urls) g_ptr_array_unref(relay_urls);
    if (callback) callback(NULL, user_data);
    def_single_fetch_ctx_free(ctx);
    return;
  }

  /* Build filter */
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { NIP58_KIND_BADGE_DEFINITION };
  nostr_filter_set_kinds(filter, kinds, 1);
  const char *authors[] = { parts[1] };
  nostr_filter_set_authors(filter, authors, 1);
  /* Set #d tag filter using nostr_filter_tags_append */
  nostr_filter_tags_append(filter, "#d", parts[2], NULL);

  /* Convert relay URLs to array */
  const char **urls = g_new0(const char *, relay_urls->len + 1);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

    gnostr_pool_sync_relays(ctx->pool, (const gchar **)urls, relay_urls->len);
  {
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    gnostr_pool_query_async(ctx->pool, _qf, ctx->cancellable, on_single_definition_fetched, ctx);
  }

  g_free(urls);
  nostr_filter_free(filter);
  g_strfreev(parts);
  g_ptr_array_unref(relay_urls);
}

/* ============== Badge Image Cache ============== */

void
gnostr_badge_prefetch_image(const gchar *url)
{
  if (!url || !*url) return;
  /* Use the existing avatar cache infrastructure for badge images */
  gnostr_avatar_prefetch(url);
}

GdkTexture *
gnostr_badge_get_cached_image(const gchar *url)
{
  if (!url || !*url) return NULL;
  /* Use the existing avatar cache infrastructure */
  return gnostr_avatar_try_load_cached(url);
}
