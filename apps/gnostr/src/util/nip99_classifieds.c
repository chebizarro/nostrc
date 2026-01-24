/*
 * nip99_classifieds.c - NIP-99 Classified Listings implementation for GNostr
 *
 * Implements classified listing fetching, parsing, and caching for:
 *   - Kind 30402: Classified Listing events
 */

#define G_LOG_DOMAIN "nip99-classifieds"

#include "nip99_classifieds.h"
#include "relays.h"
#include "../storage_ndb.h"
#include "../ui/gnostr-avatar-cache.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr_simple_pool.h"
#include <jansson.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ============== Price API ============== */

GnostrClassifiedPrice *
gnostr_classified_price_new(void)
{
  return g_new0(GnostrClassifiedPrice, 1);
}

void
gnostr_classified_price_free(GnostrClassifiedPrice *price)
{
  if (!price) return;
  g_free(price->amount);
  g_free(price->currency);
  g_free(price);
}

GnostrClassifiedPrice *
gnostr_classified_price_parse(const gchar *amount, const gchar *currency)
{
  if (!amount || !*amount) return NULL;

  GnostrClassifiedPrice *price = gnostr_classified_price_new();
  price->amount = g_strdup(amount);
  price->currency = g_strdup(currency && *currency ? currency : "USD");

  return price;
}

gchar *
gnostr_classified_price_format(const GnostrClassifiedPrice *price)
{
  if (!price || !price->amount) return g_strdup("Price not set");

  /* Special formatting for crypto currencies */
  if (g_ascii_strcasecmp(price->currency, "sats") == 0 ||
      g_ascii_strcasecmp(price->currency, "sat") == 0) {
    return g_strdup_printf("%s sats", price->amount);
  }

  if (g_ascii_strcasecmp(price->currency, "btc") == 0) {
    return g_strdup_printf("%s BTC", price->amount);
  }

  /* Common fiat currency symbols */
  if (g_ascii_strcasecmp(price->currency, "USD") == 0) {
    return g_strdup_printf("$%s", price->amount);
  }
  if (g_ascii_strcasecmp(price->currency, "EUR") == 0) {
    return g_strdup_printf("%s EUR", price->amount);
  }
  if (g_ascii_strcasecmp(price->currency, "GBP") == 0) {
    return g_strdup_printf("%s GBP", price->amount);
  }

  /* Default: amount + currency code */
  return g_strdup_printf("%s %s", price->amount, price->currency);
}

gint64
gnostr_classified_price_to_sats(const GnostrClassifiedPrice *price)
{
  if (!price || !price->amount || !price->currency) return -1;

  /* Already in sats */
  if (g_ascii_strcasecmp(price->currency, "sats") == 0 ||
      g_ascii_strcasecmp(price->currency, "sat") == 0) {
    return g_ascii_strtoll(price->amount, NULL, 10);
  }

  /* BTC to sats */
  if (g_ascii_strcasecmp(price->currency, "btc") == 0) {
    gdouble btc = g_strtod(price->amount, NULL);
    return (gint64)(btc * 100000000.0);
  }

  /* Not convertible */
  return -1;
}

/* ============== Classified Listing API ============== */

GnostrClassified *
gnostr_classified_new(void)
{
  GnostrClassified *classified = g_new0(GnostrClassified, 1);
  classified->categories = g_ptr_array_new_with_free_func(g_free);
  classified->images = g_ptr_array_new_with_free_func(g_free);
  return classified;
}

void
gnostr_classified_free(GnostrClassified *classified)
{
  if (!classified) return;

  g_free(classified->event_id);
  g_free(classified->d_tag);
  g_free(classified->pubkey);
  g_free(classified->title);
  g_free(classified->summary);
  g_free(classified->description);
  g_free(classified->location);
  g_free(classified->seller_name);
  g_free(classified->seller_avatar);
  g_free(classified->seller_nip05);
  g_free(classified->seller_lud16);

  gnostr_classified_price_free(classified->price);
  g_ptr_array_unref(classified->categories);
  g_ptr_array_unref(classified->images);

  g_free(classified);
}

GnostrClassified *
gnostr_classified_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  json_error_t error;
  json_t *root = json_loads(event_json, 0, &error);
  if (!root) {
    g_warning("classified: failed to parse JSON: %s", error.text);
    return NULL;
  }

  /* Verify kind */
  json_t *kind_val = json_object_get(root, "kind");
  if (!kind_val || json_integer_value(kind_val) != NIP99_KIND_CLASSIFIED_LISTING) {
    json_decref(root);
    return NULL;
  }

  GnostrClassified *classified = gnostr_classified_new();

  /* Extract event ID */
  json_t *id_val = json_object_get(root, "id");
  if (id_val && json_is_string(id_val)) {
    classified->event_id = g_strdup(json_string_value(id_val));
  }

  /* Extract pubkey (seller) */
  json_t *pubkey_val = json_object_get(root, "pubkey");
  if (pubkey_val && json_is_string(pubkey_val)) {
    classified->pubkey = g_strdup(json_string_value(pubkey_val));
  }

  /* Extract created_at */
  json_t *created_val = json_object_get(root, "created_at");
  if (created_val && json_is_integer(created_val)) {
    classified->created_at = json_integer_value(created_val);
  }

  /* Extract content (full description) */
  json_t *content_val = json_object_get(root, "content");
  if (content_val && json_is_string(content_val)) {
    classified->description = g_strdup(json_string_value(content_val));
  }

  /* Parse tags for classified metadata */
  json_t *tags = json_object_get(root, "tags");
  if (tags && json_is_array(tags)) {
    size_t i;
    json_t *tag;
    json_array_foreach(tags, i, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

      const char *tag_name = json_string_value(json_array_get(tag, 0));
      const char *tag_value = json_string_value(json_array_get(tag, 1));
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "d") == 0) {
        g_free(classified->d_tag);
        classified->d_tag = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "title") == 0) {
        g_free(classified->title);
        classified->title = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "summary") == 0) {
        g_free(classified->summary);
        classified->summary = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "location") == 0) {
        g_free(classified->location);
        classified->location = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "published_at") == 0) {
        classified->published_at = g_ascii_strtoll(tag_value, NULL, 10);
      } else if (g_strcmp0(tag_name, "price") == 0) {
        /* Price tag: ["price", "amount", "currency"] */
        const char *currency = NULL;
        if (json_array_size(tag) >= 3) {
          currency = json_string_value(json_array_get(tag, 2));
        }
        gnostr_classified_price_free(classified->price);
        classified->price = gnostr_classified_price_parse(tag_value, currency);
      } else if (g_strcmp0(tag_name, "t") == 0) {
        /* Category tag */
        g_ptr_array_add(classified->categories, g_strdup(tag_value));
      } else if (g_strcmp0(tag_name, "image") == 0) {
        /* Image URL */
        g_ptr_array_add(classified->images, g_strdup(tag_value));
      }
    }
  }

  json_decref(root);

  /* Validate: must have d tag */
  if (!classified->d_tag) {
    g_debug("classified: missing 'd' tag identifier");
    gnostr_classified_free(classified);
    return NULL;
  }

  /* Use published_at if not set, fall back to created_at */
  if (classified->published_at <= 0) {
    classified->published_at = classified->created_at;
  }

  g_debug("classified: parsed '%s' (d=%s) from %s with %u images",
          classified->title ? classified->title : "(untitled)",
          classified->d_tag,
          classified->pubkey ? classified->pubkey : "unknown",
          classified->images->len);

  return classified;
}

gchar *
gnostr_classified_get_naddr(const GnostrClassified *classified)
{
  if (!classified || !classified->pubkey || !classified->d_tag) return NULL;
  return g_strdup_printf("%d:%s:%s",
                         NIP99_KIND_CLASSIFIED_LISTING,
                         classified->pubkey,
                         classified->d_tag);
}

const gchar *
gnostr_classified_get_primary_image(const GnostrClassified *classified)
{
  if (!classified || !classified->images || classified->images->len == 0) {
    return NULL;
  }
  return g_ptr_array_index(classified->images, 0);
}

gchar *
gnostr_classified_get_category_string(const GnostrClassified *classified)
{
  if (!classified || !classified->categories || classified->categories->len == 0) {
    return NULL;
  }

  GString *result = g_string_new(NULL);
  for (guint i = 0; i < classified->categories->len; i++) {
    if (i > 0) g_string_append(result, ", ");
    g_string_append(result, (gchar *)g_ptr_array_index(classified->categories, i));
  }

  return g_string_free(result, FALSE);
}

/* ============== Event Creation ============== */

gchar *
gnostr_classified_create_event_json(const GnostrClassified *classified)
{
  if (!classified || !classified->d_tag) return NULL;

  json_t *event = json_object();
  json_object_set_new(event, "kind", json_integer(NIP99_KIND_CLASSIFIED_LISTING));

  /* Content is the full description */
  json_object_set_new(event, "content",
    json_string(classified->description ? classified->description : ""));

  /* Build tags array */
  json_t *tags = json_array();

  /* d tag (required) */
  json_t *d_tag = json_array();
  json_array_append_new(d_tag, json_string("d"));
  json_array_append_new(d_tag, json_string(classified->d_tag));
  json_array_append_new(tags, d_tag);

  /* title tag */
  if (classified->title && *classified->title) {
    json_t *title_tag = json_array();
    json_array_append_new(title_tag, json_string("title"));
    json_array_append_new(title_tag, json_string(classified->title));
    json_array_append_new(tags, title_tag);
  }

  /* summary tag */
  if (classified->summary && *classified->summary) {
    json_t *summary_tag = json_array();
    json_array_append_new(summary_tag, json_string("summary"));
    json_array_append_new(summary_tag, json_string(classified->summary));
    json_array_append_new(tags, summary_tag);
  }

  /* published_at tag */
  if (classified->published_at > 0) {
    json_t *pub_tag = json_array();
    json_array_append_new(pub_tag, json_string("published_at"));
    gchar *ts_str = g_strdup_printf("%" G_GINT64_FORMAT, classified->published_at);
    json_array_append_new(pub_tag, json_string(ts_str));
    g_free(ts_str);
    json_array_append_new(tags, pub_tag);
  }

  /* location tag */
  if (classified->location && *classified->location) {
    json_t *loc_tag = json_array();
    json_array_append_new(loc_tag, json_string("location"));
    json_array_append_new(loc_tag, json_string(classified->location));
    json_array_append_new(tags, loc_tag);
  }

  /* price tag */
  if (classified->price && classified->price->amount) {
    json_t *price_tag = json_array();
    json_array_append_new(price_tag, json_string("price"));
    json_array_append_new(price_tag, json_string(classified->price->amount));
    if (classified->price->currency && *classified->price->currency) {
      json_array_append_new(price_tag, json_string(classified->price->currency));
    }
    json_array_append_new(tags, price_tag);
  }

  /* category (t) tags */
  if (classified->categories) {
    for (guint i = 0; i < classified->categories->len; i++) {
      const gchar *cat = g_ptr_array_index(classified->categories, i);
      if (cat && *cat) {
        json_t *t_tag = json_array();
        json_array_append_new(t_tag, json_string("t"));
        json_array_append_new(t_tag, json_string(cat));
        json_array_append_new(tags, t_tag);
      }
    }
  }

  /* image tags */
  if (classified->images) {
    for (guint i = 0; i < classified->images->len; i++) {
      const gchar *img = g_ptr_array_index(classified->images, i);
      if (img && *img) {
        json_t *img_tag = json_array();
        json_array_append_new(img_tag, json_string("image"));
        json_array_append_new(img_tag, json_string(img));
        json_array_append_new(tags, img_tag);
      }
    }
  }

  json_object_set_new(event, "tags", tags);

  gchar *result = json_dumps(event, JSON_COMPACT);
  json_decref(event);

  return result;
}

/* ============== Async Fetch Context ============== */

typedef struct _ClassifiedFetchCtx {
  GnostrClassifiedFetchCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GnostrSimplePool *pool;
  GPtrArray *classifieds;
} ClassifiedFetchCtx;

static void
classified_fetch_ctx_free(ClassifiedFetchCtx *ctx)
{
  if (!ctx) return;
  g_clear_object(&ctx->cancellable);
  g_clear_object(&ctx->pool);
  if (ctx->classifieds) g_ptr_array_unref(ctx->classifieds);
  g_free(ctx);
}

static void
on_classifieds_fetched(GObject *source, GAsyncResult *res, gpointer user_data)
{
  ClassifiedFetchCtx *ctx = user_data;
  GnostrSimplePool *pool = GNOSTR_SIMPLE_POOL(source);
  GError *error = NULL;

  GPtrArray *events = gnostr_simple_pool_query_single_finish(pool, res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("fetch_classifieds: query failed: %s", error->message);
    }
    g_error_free(error);
    if (ctx->callback) ctx->callback(NULL, ctx->user_data);
    classified_fetch_ctx_free(ctx);
    return;
  }

  if (!events || events->len == 0) {
    g_debug("fetch_classifieds: no listings found");
    if (events) g_ptr_array_unref(events);
    if (ctx->callback) ctx->callback(NULL, ctx->user_data);
    classified_fetch_ctx_free(ctx);
    return;
  }

  /* Parse all events */
  ctx->classifieds = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_classified_free);

  for (guint i = 0; i < events->len; i++) {
    NostrEvent *event = g_ptr_array_index(events, i);
    gchar *event_json = nostr_event_serialize_compact(event);
    if (event_json) {
      GnostrClassified *classified = gnostr_classified_parse(event_json);
      if (classified) {
        /* Prefetch primary image */
        const gchar *img = gnostr_classified_get_primary_image(classified);
        if (img) gnostr_avatar_prefetch(img);

        g_ptr_array_add(ctx->classifieds, classified);
      }
      g_free(event_json);
    }
  }

  g_ptr_array_unref(events);

  g_debug("fetch_classifieds: parsed %u listings", ctx->classifieds->len);

  if (ctx->callback) {
    ctx->callback(ctx->classifieds, ctx->user_data);
    ctx->classifieds = NULL; /* Transfer ownership */
  }

  classified_fetch_ctx_free(ctx);
}

/* ============== Public Fetch Functions ============== */

void
gnostr_fetch_classifieds_async(const gchar *filter_category,
                                const gchar *filter_location,
                                guint limit,
                                GCancellable *cancellable,
                                GnostrClassifiedFetchCallback callback,
                                gpointer user_data)
{
  ClassifiedFetchCtx *ctx = g_new0(ClassifiedFetchCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->pool = gnostr_simple_pool_new();

  /* Get relay URLs */
  GPtrArray *relay_urls = gnostr_get_read_relay_urls();
  if (!relay_urls || relay_urls->len == 0) {
    g_debug("fetch_classifieds: no relays configured");
    if (relay_urls) g_ptr_array_unref(relay_urls);
    if (callback) callback(NULL, user_data);
    classified_fetch_ctx_free(ctx);
    return;
  }

  /* Build filter for kind 30402 */
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { NIP99_KIND_CLASSIFIED_LISTING };
  nostr_filter_set_kinds(filter, kinds, 1);

  /* Apply category filter if specified */
  if (filter_category && *filter_category) {
    nostr_filter_tags_append(filter, "#t", filter_category, NULL);
  }

  /* Note: location filtering would need custom handling as NIP-99 doesn't define it as an indexed tag */
  (void)filter_location;

  /* Set limit */
  nostr_filter_set_limit(filter, limit > 0 ? limit : 50);

  /* Convert relay URLs to array */
  const char **urls = g_new0(const char *, relay_urls->len + 1);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  gnostr_simple_pool_query_single_async(ctx->pool, urls, relay_urls->len,
                                         filter, ctx->cancellable,
                                         on_classifieds_fetched, ctx);

  g_free(urls);
  nostr_filter_free(filter);
  g_ptr_array_unref(relay_urls);
}

void
gnostr_fetch_user_classifieds_async(const gchar *pubkey_hex,
                                     GCancellable *cancellable,
                                     GnostrClassifiedFetchCallback callback,
                                     gpointer user_data)
{
  g_return_if_fail(pubkey_hex != NULL && strlen(pubkey_hex) == 64);

  ClassifiedFetchCtx *ctx = g_new0(ClassifiedFetchCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->pool = gnostr_simple_pool_new();

  /* Get relay URLs */
  GPtrArray *relay_urls = gnostr_get_read_relay_urls();
  if (!relay_urls || relay_urls->len == 0) {
    g_debug("fetch_user_classifieds: no relays configured");
    if (relay_urls) g_ptr_array_unref(relay_urls);
    if (callback) callback(NULL, user_data);
    classified_fetch_ctx_free(ctx);
    return;
  }

  /* Build filter for kind 30402 from specific author */
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { NIP99_KIND_CLASSIFIED_LISTING };
  nostr_filter_set_kinds(filter, kinds, 1);
  const char *authors[] = { pubkey_hex };
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filter_set_limit(filter, 100);

  /* Convert relay URLs to array */
  const char **urls = g_new0(const char *, relay_urls->len + 1);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  gnostr_simple_pool_query_single_async(ctx->pool, urls, relay_urls->len,
                                         filter, ctx->cancellable,
                                         on_classifieds_fetched, ctx);

  g_free(urls);
  nostr_filter_free(filter);
  g_ptr_array_unref(relay_urls);
}

/* ============== Single Fetch ============== */

typedef struct _ClassifiedSingleCtx {
  GnostrClassifiedSingleCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GnostrSimplePool *pool;
  gchar *naddr;
} ClassifiedSingleCtx;

static void
classified_single_ctx_free(ClassifiedSingleCtx *ctx)
{
  if (!ctx) return;
  g_free(ctx->naddr);
  g_clear_object(&ctx->cancellable);
  g_clear_object(&ctx->pool);
  g_free(ctx);
}

static void
on_single_classified_fetched(GObject *source, GAsyncResult *res, gpointer user_data)
{
  ClassifiedSingleCtx *ctx = user_data;
  GnostrSimplePool *pool = GNOSTR_SIMPLE_POOL(source);
  GError *error = NULL;

  GPtrArray *events = gnostr_simple_pool_query_single_finish(pool, res, &error);
  GnostrClassified *classified = NULL;

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("fetch_classified_by_naddr: query failed: %s", error->message);
    }
    g_error_free(error);
  } else if (events && events->len > 0) {
    NostrEvent *event = g_ptr_array_index(events, 0);
    gchar *event_json = nostr_event_serialize_compact(event);
    if (event_json) {
      classified = gnostr_classified_parse(event_json);
      g_free(event_json);
    }
  }

  if (events) g_ptr_array_unref(events);

  if (ctx->callback) {
    ctx->callback(classified, ctx->user_data);
  }

  if (classified) gnostr_classified_free(classified);
  classified_single_ctx_free(ctx);
}

void
gnostr_fetch_classified_by_naddr_async(const gchar *naddr,
                                        GCancellable *cancellable,
                                        GnostrClassifiedSingleCallback callback,
                                        gpointer user_data)
{
  g_return_if_fail(naddr != NULL);

  /* Parse naddr: "30402:pubkey:d_tag" */
  gchar **parts = g_strsplit(naddr, ":", 3);
  if (!parts || !parts[0] || !parts[1] || !parts[2]) {
    g_warning("fetch_classified_by_naddr: invalid naddr format: %s", naddr);
    g_strfreev(parts);
    if (callback) callback(NULL, user_data);
    return;
  }

  ClassifiedSingleCtx *ctx = g_new0(ClassifiedSingleCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->pool = gnostr_simple_pool_new();
  ctx->naddr = g_strdup(naddr);

  /* Get relay URLs */
  GPtrArray *relay_urls = gnostr_get_read_relay_urls();
  if (!relay_urls || relay_urls->len == 0) {
    g_strfreev(parts);
    if (relay_urls) g_ptr_array_unref(relay_urls);
    if (callback) callback(NULL, user_data);
    classified_single_ctx_free(ctx);
    return;
  }

  /* Build filter */
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { NIP99_KIND_CLASSIFIED_LISTING };
  nostr_filter_set_kinds(filter, kinds, 1);
  const char *authors[] = { parts[1] };
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filter_tags_append(filter, "#d", parts[2], NULL);
  nostr_filter_set_limit(filter, 1);

  /* Convert relay URLs to array */
  const char **urls = g_new0(const char *, relay_urls->len + 1);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  gnostr_simple_pool_query_single_async(ctx->pool, urls, relay_urls->len,
                                         filter, ctx->cancellable,
                                         on_single_classified_fetched, ctx);

  g_free(urls);
  nostr_filter_free(filter);
  g_strfreev(parts);
  g_ptr_array_unref(relay_urls);
}

/* ============== Image Cache ============== */

void
gnostr_classified_prefetch_images(const GnostrClassified *classified)
{
  if (!classified || !classified->images) return;

  for (guint i = 0; i < classified->images->len; i++) {
    const gchar *url = g_ptr_array_index(classified->images, i);
    if (url && *url) {
      gnostr_avatar_prefetch(url);
    }
  }
}

GdkTexture *
gnostr_classified_get_cached_image(const gchar *url)
{
  if (!url || !*url) return NULL;
  return gnostr_avatar_try_load_cached(url);
}
