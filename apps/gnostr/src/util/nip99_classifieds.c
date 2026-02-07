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
#include "json.h"
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

/* Callback context for parsing classified tags */
typedef struct {
  GnostrClassified *classified;
} ClassifiedParseCtx;

static bool
classified_tag_callback(size_t index, const char *element_json, void *user_data)
{
  (void)index;
  ClassifiedParseCtx *ctx = user_data;

  char *tag_name = NULL;
  char *tag_value = NULL;

  if (nostr_json_get_array_string(element_json, NULL, 0, &tag_name) != 0 || !tag_name) {
    return true;
  }

  if (nostr_json_get_array_string(element_json, NULL, 1, &tag_value) != 0 || !tag_value) {
    free(tag_name);
    return true;
  }

  if (g_strcmp0(tag_name, "d") == 0) {
    g_free(ctx->classified->d_tag);
    ctx->classified->d_tag = g_strdup(tag_value);
  } else if (g_strcmp0(tag_name, "title") == 0) {
    g_free(ctx->classified->title);
    ctx->classified->title = g_strdup(tag_value);
  } else if (g_strcmp0(tag_name, "summary") == 0) {
    g_free(ctx->classified->summary);
    ctx->classified->summary = g_strdup(tag_value);
  } else if (g_strcmp0(tag_name, "location") == 0) {
    g_free(ctx->classified->location);
    ctx->classified->location = g_strdup(tag_value);
  } else if (g_strcmp0(tag_name, "published_at") == 0) {
    ctx->classified->published_at = g_ascii_strtoll(tag_value, NULL, 10);
  } else if (g_strcmp0(tag_name, "price") == 0) {
    /* Price tag: ["price", "amount", "currency"] */
    char *currency = NULL;
    nostr_json_get_array_string(element_json, NULL, 2, &currency);
    gnostr_classified_price_free(ctx->classified->price);
    ctx->classified->price = gnostr_classified_price_parse(tag_value, currency);
    free(currency);
  } else if (g_strcmp0(tag_name, "t") == 0) {
    /* Category tag */
    g_ptr_array_add(ctx->classified->categories, g_strdup(tag_value));
  } else if (g_strcmp0(tag_name, "image") == 0) {
    /* Image URL */
    g_ptr_array_add(ctx->classified->images, g_strdup(tag_value));
  }

  free(tag_name);
  free(tag_value);
  return true;
}

GnostrClassified *
gnostr_classified_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  if (!nostr_json_is_valid(event_json)) {
    g_warning("classified: failed to parse JSON");
    return NULL;
  }

  /* Verify kind */
  int kind = 0;
  if (nostr_json_get_int(event_json, "kind", &kind) != 0 ||
      kind != NIP99_KIND_CLASSIFIED_LISTING) {
    return NULL;
  }

  GnostrClassified *classified = gnostr_classified_new();

  /* Extract event ID */
  char *id_val = NULL;
  if (nostr_json_get_string(event_json, "id", &id_val) == 0 && id_val) {
    classified->event_id = g_strdup(id_val);
    free(id_val);
  }

  /* Extract pubkey (seller) */
  char *pubkey_val = NULL;
  if (nostr_json_get_string(event_json, "pubkey", &pubkey_val) == 0 && pubkey_val) {
    classified->pubkey = g_strdup(pubkey_val);
    free(pubkey_val);
  }

  /* Extract created_at */
  int64_t created_at = 0;
  if (nostr_json_get_int64(event_json, "created_at", &created_at) == 0) {
    classified->created_at = created_at;
  }

  /* Extract content (full description) */
  char *content_val = NULL;
  if (nostr_json_get_string(event_json, "content", &content_val) == 0 && content_val) {
    classified->description = g_strdup(content_val);
    free(content_val);
  }

  /* Parse tags for classified metadata */
  char *tags_json = NULL;
  if (nostr_json_get_raw(event_json, "tags", &tags_json) == 0 && tags_json) {
    ClassifiedParseCtx ctx = { .classified = classified };
    nostr_json_array_foreach_root(tags_json, classified_tag_callback, &ctx);
    free(tags_json);
  }

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

  NostrJsonBuilder *builder = nostr_json_builder_new();
  nostr_json_builder_begin_object(builder);

  nostr_json_builder_set_key(builder, "kind");
  nostr_json_builder_add_int(builder, NIP99_KIND_CLASSIFIED_LISTING);

  /* Content is the full description */
  nostr_json_builder_set_key(builder, "content");
  nostr_json_builder_add_string(builder, classified->description ? classified->description : "");

  /* Build tags array */
  nostr_json_builder_set_key(builder, "tags");
  nostr_json_builder_begin_array(builder);

  /* d tag (required) */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "d");
  nostr_json_builder_add_string(builder, classified->d_tag);
  nostr_json_builder_end_array(builder);

  /* title tag */
  if (classified->title && *classified->title) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "title");
    nostr_json_builder_add_string(builder, classified->title);
    nostr_json_builder_end_array(builder);
  }

  /* summary tag */
  if (classified->summary && *classified->summary) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "summary");
    nostr_json_builder_add_string(builder, classified->summary);
    nostr_json_builder_end_array(builder);
  }

  /* published_at tag */
  if (classified->published_at > 0) {
    gchar *ts_str = g_strdup_printf("%" G_GINT64_FORMAT, classified->published_at);
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "published_at");
    nostr_json_builder_add_string(builder, ts_str);
    nostr_json_builder_end_array(builder);
    g_free(ts_str);
  }

  /* location tag */
  if (classified->location && *classified->location) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "location");
    nostr_json_builder_add_string(builder, classified->location);
    nostr_json_builder_end_array(builder);
  }

  /* price tag */
  if (classified->price && classified->price->amount) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "price");
    nostr_json_builder_add_string(builder, classified->price->amount);
    if (classified->price->currency && *classified->price->currency) {
      nostr_json_builder_add_string(builder, classified->price->currency);
    }
    nostr_json_builder_end_array(builder);
  }

  /* category (t) tags */
  if (classified->categories) {
    for (guint i = 0; i < classified->categories->len; i++) {
      const gchar *cat = g_ptr_array_index(classified->categories, i);
      if (cat && *cat) {
        nostr_json_builder_begin_array(builder);
        nostr_json_builder_add_string(builder, "t");
        nostr_json_builder_add_string(builder, cat);
        nostr_json_builder_end_array(builder);
      }
    }
  }

  /* image tags */
  if (classified->images) {
    for (guint i = 0; i < classified->images->len; i++) {
      const gchar *img = g_ptr_array_index(classified->images, i);
      if (img && *img) {
        nostr_json_builder_begin_array(builder);
        nostr_json_builder_add_string(builder, "image");
        nostr_json_builder_add_string(builder, img);
        nostr_json_builder_end_array(builder);
      }
    }
  }

  nostr_json_builder_end_array(builder);  /* tags */
  nostr_json_builder_end_object(builder);

  char *result = nostr_json_builder_finish(builder);
  nostr_json_builder_free(builder);

  return result;
}

/* ============== Async Fetch Context ============== */

/* Static pool for classifieds queries - reused to maintain relay connections */
static GnostrSimplePool *s_classifieds_pool = NULL;

static GnostrSimplePool *
get_classifieds_pool(void)
{
  if (!s_classifieds_pool) {
    s_classifieds_pool = gnostr_simple_pool_new();
  }
  return s_classifieds_pool;
}

typedef struct _ClassifiedFetchCtx {
  GnostrClassifiedFetchCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GPtrArray *classifieds;
} ClassifiedFetchCtx;

static void
classified_fetch_ctx_free(ClassifiedFetchCtx *ctx)
{
  if (!ctx) return;
  g_clear_object(&ctx->cancellable);
  /* Note: pool is static/shared, not freed here */
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

  /* Parse all events - note: query returns JSON strings, not NostrEvent* */
  ctx->classifieds = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_classified_free);

  for (guint i = 0; i < events->len; i++) {
    const gchar *event_json = g_ptr_array_index(events, i);
    if (event_json) {
      GnostrClassified *classified = gnostr_classified_parse(event_json);
      if (classified) {
        /* Prefetch primary image */
        const gchar *img = gnostr_classified_get_primary_image(classified);
        if (img) gnostr_avatar_prefetch(img);

        g_ptr_array_add(ctx->classifieds, classified);
      }
      /* Don't free event_json - it's owned by the events array */
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

  /* Use shared pool for better connection reuse */
  GnostrSimplePool *pool = get_classifieds_pool();
  gnostr_simple_pool_query_single_async(pool, urls, relay_urls->len,
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

  /* Use shared pool for better connection reuse */
  GnostrSimplePool *pool = get_classifieds_pool();
  gnostr_simple_pool_query_single_async(pool, urls, relay_urls->len,
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
  gchar *naddr;
} ClassifiedSingleCtx;

static void
classified_single_ctx_free(ClassifiedSingleCtx *ctx)
{
  if (!ctx) return;
  g_free(ctx->naddr);
  g_clear_object(&ctx->cancellable);
  /* Note: uses shared pool from get_classifieds_pool(), don't free it */
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
    /* Query returns JSON strings, not NostrEvent* */
    const gchar *event_json = g_ptr_array_index(events, 0);
    if (event_json) {
      classified = gnostr_classified_parse(event_json);
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

  /* Convert relay URLs to array */
  const char **urls = g_new0(const char *, relay_urls->len + 1);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  gnostr_simple_pool_query_single_async(get_classifieds_pool(), urls, relay_urls->len,
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
