/*
 * gnostr-articles-view.c - Long-form Content Browse View
 *
 * Displays browsable lists of NIP-54 Wiki and NIP-23 Long-form articles.
 */

#define G_LOG_DOMAIN "gnostr-articles-view"

#include "gnostr-articles-view.h"
#include "gnostr-wiki-card.h"
#include "gnostr-article-card.h"
#include "../storage_ndb.h"
#include "../util/nip23.h"
#include "../util/nip54_wiki.h"
#include "../util/relays.h"
#include "../util/utils.h"
#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr-filter.h"
#include <glib/gi18n.h>
#include "nostr_json.h"
#include <json.h>
#include <string.h>
#include "../util/debounce.h"

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-articles-view.ui"

/* Maximum number of articles to load initially */
#define ARTICLES_LOAD_LIMIT 100

/* Maximum number of articles to fetch from relays */
#define ARTICLES_FETCH_LIMIT 50

/* NIP-23 Long-form content */
#define KIND_LONG_FORM 30023

/* NIP-54 Wiki article */
#define KIND_WIKI 30818

struct _GnostrArticlesView {
  GtkWidget parent_instance;

  /* Template widgets */
  GtkWidget *root;
  GtkWidget *header_box;
  GtkWidget *title_label;
  GtkWidget *search_entry;
  GtkWidget *btn_all;
  GtkWidget *btn_wiki;
  GtkWidget *btn_blog;
  GtkWidget *lbl_count;
  GtkWidget *content_stack;
  GtkWidget *articles_scroll;
  GtkWidget *articles_list;
  GtkWidget *empty_state;
  GtkWidget *loading_spinner;
  GtkWidget *topic_filter_box;
  GtkWidget *topic_filter_label;
  GtkWidget *btn_clear_topic;

  /* Model */
  GListStore *articles_model;
  GtkFilterListModel *filtered_model;
  GtkCustomFilter *custom_filter;
  GtkSingleSelection *selection;
  GtkListItemFactory *factory;

  /* State */
  GnostrArticlesType type_filter;
  gchar *topic_filter;
  gchar *search_text;
  gboolean articles_loaded;
  gboolean is_logged_in;
  GnostrDebounce *search_debounce;

  /* Async fetch state */
  GCancellable *fetch_cancellable;
  /* Uses gnostr_get_shared_query_pool() instead of per-widget pool */
  gboolean fetch_in_progress;
};

G_DEFINE_TYPE(GnostrArticlesView, gnostr_articles_view, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_ARTICLE,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_TOPIC_CLICKED,
  SIGNAL_ZAP_REQUESTED,
  SIGNAL_BOOKMARK_TOGGLED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* --- Article List Item (GObject wrapper) --- */

#define GNOSTR_TYPE_ARTICLE_ITEM (gnostr_article_item_get_type())
G_DECLARE_FINAL_TYPE(GnostrArticleItem, gnostr_article_item, GNOSTR, ARTICLE_ITEM, GObject)

struct _GnostrArticleItem {
  GObject parent_instance;
  gint kind;                /* 30023 or 30818 */
  gchar *event_id;
  gchar *d_tag;
  gchar *pubkey_hex;
  gchar *title;
  gchar *summary;
  gchar *image_url;
  gchar *content;
  gint64 published_at;
  gint64 created_at;
  gchar **topics;
  gsize topics_count;
  /* Author info (cached from profile) */
  gchar *author_name;
  gchar *author_handle;
  gchar *author_avatar;
  gchar *author_nip05;
  gchar *author_lud16;
};

G_DEFINE_TYPE(GnostrArticleItem, gnostr_article_item, G_TYPE_OBJECT)

static void gnostr_article_item_finalize(GObject *object) {
  GnostrArticleItem *self = GNOSTR_ARTICLE_ITEM(object);
  g_free(self->event_id);
  g_free(self->d_tag);
  g_free(self->pubkey_hex);
  g_free(self->title);
  g_free(self->summary);
  g_free(self->image_url);
  g_free(self->content);
  g_free(self->author_name);
  g_free(self->author_handle);
  g_free(self->author_avatar);
  g_free(self->author_nip05);
  g_free(self->author_lud16);
  if (self->topics) {
    for (gsize i = 0; i < self->topics_count; i++) {
      g_free(self->topics[i]);
    }
    g_free(self->topics);
  }
  G_OBJECT_CLASS(gnostr_article_item_parent_class)->finalize(object);
}

static void gnostr_article_item_class_init(GnostrArticleItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gnostr_article_item_finalize;
}

static void gnostr_article_item_init(GnostrArticleItem *self) {
  (void)self;
}

static GnostrArticleItem *gnostr_article_item_new(void) {
  return g_object_new(GNOSTR_TYPE_ARTICLE_ITEM, NULL);
}

/* --- Forward declarations --- */
static void update_content_state(GnostrArticlesView *self);
static void update_article_count(GnostrArticlesView *self);
static void apply_filters(GnostrArticlesView *self);
static void load_articles_from_nostrdb(GnostrArticlesView *self);
static void fetch_articles_from_relays(GnostrArticlesView *self);
static void populate_author_info(GnostrArticleItem *item, void *txn);

/* --- Helper: Convert hex string to 32 bytes --- */
static gboolean hex_to_bytes_32(const char *hex, unsigned char out[32]) {
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + i * 2, "%2x", &byte) != 1) return FALSE;
    out[i] = (unsigned char)byte;
  }
  return TRUE;
}

/* --- Helper: Parse author profile from nostrdb --- */
static void populate_author_info(GnostrArticleItem *item, void *txn) {
  if (!item || !item->pubkey_hex || !txn) return;

  unsigned char pubkey_bytes[32];
  if (!hex_to_bytes_32(item->pubkey_hex, pubkey_bytes)) return;

  char *profile_json = NULL;
  int profile_len = 0;

  if (storage_ndb_get_profile_by_pubkey(txn, pubkey_bytes, &profile_json, &profile_len) != 0 || !profile_json) {
    /* No profile found - use short pubkey as fallback */
    item->author_name = g_strndup(item->pubkey_hex, 8);
    item->author_handle = g_strdup_printf("@%s...", item->author_name);
    return;
  }

  /* Parse the kind-0 event to get profile content */
  NostrEvent *evt = nostr_event_new();
  if (!evt || nostr_event_deserialize(evt, profile_json) != 0) {
    if (evt) nostr_event_free(evt);
    item->author_name = g_strndup(item->pubkey_hex, 8);
    item->author_handle = g_strdup_printf("@%s...", item->author_name);
    return;
  }

  const char *content = nostr_event_get_content(evt);
  if (content && *content) {
    /* Parse profile JSON content using nostr_json helpers */
    char *dn = NULL;
    if ((dn = gnostr_json_get_string(content, "display_name", NULL)) != NULL && dn && *dn) {
      item->author_name = dn;
    } else {
      free(dn);
    }
    if (!item->author_name) {
      char *n = NULL;
      if ((n = gnostr_json_get_string(content, "name", NULL)) != NULL && n && *n) {
        item->author_name = n;
      } else {
        free(n);
      }
    }
    char *name = NULL;
    if ((name = gnostr_json_get_string(content, "name", NULL)) != NULL && name && *name) {
      item->author_handle = g_strdup_printf("@%s", name);
      free(name);
    } else {
      free(name);
    }
    char *pic = NULL;
    if ((pic = gnostr_json_get_string(content, "picture", NULL)) != NULL && pic && *pic) {
      item->author_avatar = pic;
    } else {
      free(pic);
    }
    char *nip05 = NULL;
    if ((nip05 = gnostr_json_get_string(content, "nip05", NULL)) != NULL && nip05 && *nip05) {
      item->author_nip05 = nip05;
    } else {
      free(nip05);
    }
    char *lud16 = NULL;
    if ((lud16 = gnostr_json_get_string(content, "lud16", NULL)) != NULL && lud16 && *lud16) {
      item->author_lud16 = lud16;
    } else {
      free(lud16);
    }
  }

  nostr_event_free(evt);

  /* Fallback if no name found */
  if (!item->author_name) {
    item->author_name = g_strndup(item->pubkey_hex, 8);
  }
  if (!item->author_handle) {
    gchar *short_pk = g_strndup(item->pubkey_hex, 8);
    item->author_handle = g_strdup_printf("@%s...", short_pk);
    g_free(short_pk);
  }
}

/* --- Helper: Create article item from event JSON --- */
static GnostrArticleItem *create_article_item_from_json(const char *event_json, void *txn) {
  if (!event_json || !*event_json) return NULL;

  NostrEvent *evt = nostr_event_new();
  if (!evt || nostr_event_deserialize(evt, event_json) != 0) {
    if (evt) nostr_event_free(evt);
    return NULL;
  }

  int kind = nostr_event_get_kind(evt);
  if (kind != KIND_LONG_FORM && kind != KIND_WIKI) {
    nostr_event_free(evt);
    return NULL;
  }

  GnostrArticleItem *item = gnostr_article_item_new();
  item->kind = kind;

  /* Basic event data */
  char *event_id = nostr_event_get_id(evt);
  if (event_id) {
    item->event_id = event_id;  /* Takes ownership */
  }

  const char *pubkey = nostr_event_get_pubkey(evt);
  if (pubkey) {
    item->pubkey_hex = g_strdup(pubkey);
  }

  item->created_at = (gint64)nostr_event_get_created_at(evt);

  const char *content = nostr_event_get_content(evt);
  if (content && *content) {
    item->content = g_strdup(content);
  }

  /* Parse tags using appropriate NIP utility */
  if (kind == KIND_WIKI) {
    GnostrWikiArticle *wiki = gnostr_wiki_article_parse_json(event_json);
    if (wiki) {
      item->d_tag = g_strdup(wiki->d_tag);
      item->title = g_strdup(wiki->title);
      item->summary = g_strdup(wiki->summary);
      item->published_at = wiki->published_at > 0 ? wiki->published_at : item->created_at;

      /* Copy topics */
      if (wiki->topics && wiki->topics_count > 0) {
        item->topics_count = wiki->topics_count;
        item->topics = g_new0(gchar*, wiki->topics_count + 1);
        for (gsize i = 0; i < wiki->topics_count; i++) {
          item->topics[i] = g_strdup(wiki->topics[i]);
        }
      }

      gnostr_wiki_article_free(wiki);
    }
  } else {
    /* KIND_LONG_FORM - parse via NIP-23 */
    /* Extract raw tags JSON using nostr_json_get_raw helper */
    char *tags_json = NULL;
    tags_json = gnostr_json_get_raw(event_json, "tags", NULL);
    if (tags_json) {
      GnostrArticleMeta *meta = gnostr_article_parse_tags(tags_json);
      if (meta) {
        item->d_tag = g_strdup(meta->d_tag);
        item->title = g_strdup(meta->title);
        item->summary = g_strdup(meta->summary);
        item->image_url = g_strdup(meta->image);
        item->published_at = meta->published_at > 0 ? meta->published_at : item->created_at;

        /* Copy hashtags as topics */
        if (meta->hashtags && meta->hashtags_count > 0) {
          item->topics_count = meta->hashtags_count;
          item->topics = g_new0(gchar*, meta->hashtags_count + 1);
          for (gsize i = 0; i < meta->hashtags_count; i++) {
            item->topics[i] = g_strdup(meta->hashtags[i]);
          }
        }

        gnostr_article_meta_free(meta);
      }
      free(tags_json);
    }
  }

  /* Fallbacks */
  if (!item->title || !*item->title) {
    g_free(item->title);
    if (item->d_tag && *item->d_tag) {
      item->title = g_strdup(item->d_tag);
    } else {
      item->title = g_strdup("Untitled");
    }
  }

  if (item->published_at == 0) {
    item->published_at = item->created_at;
  }

  /* Populate author info from profile */
  if (txn) {
    populate_author_info(item, txn);
  }

  nostr_event_free(evt);
  return item;
}

/* --- Load articles from local nostrdb --- */
static void load_articles_from_nostrdb(GnostrArticlesView *self) {
  g_debug("articles-view: Loading articles from nostrdb");

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    g_warning("articles-view: Failed to begin nostrdb query");
    return;
  }

  /* Build filter JSON for both kinds */
  char *filter_json = g_strdup_printf(
    "{\"kinds\":[%d,%d],\"limit\":%d}",
    KIND_LONG_FORM, KIND_WIKI, ARTICLES_LOAD_LIMIT
  );

  char **results = NULL;
  int result_count = 0;

  if (storage_ndb_query(txn, filter_json, &results, &result_count) == 0 && results) {
    g_debug("articles-view: Found %d articles in nostrdb", result_count);

    /* Use a hash set to deduplicate by event ID */
    GHashTable *seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (int i = 0; i < result_count; i++) {
      if (!results[i]) continue;

      GnostrArticleItem *item = create_article_item_from_json(results[i], txn);
      if (item && item->event_id) {
        /* Check for duplicates */
        if (!g_hash_table_contains(seen_ids, item->event_id)) {
          g_hash_table_add(seen_ids, g_strdup(item->event_id));
          g_list_store_append(self->articles_model, item);
        }
        g_object_unref(item);
      } else if (item) {
        g_object_unref(item);
      }
    }

    g_hash_table_destroy(seen_ids);
    storage_ndb_free_results(results, result_count);
  }

  g_free(filter_json);
  storage_ndb_end_query(txn);

  g_debug("articles-view: Loaded %u articles into model",
          g_list_model_get_n_items(G_LIST_MODEL(self->articles_model)));
}

/* --- Async relay fetch callback --- */
static void on_relay_fetch_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  g_debug("articles-view: on_relay_fetch_complete called");

  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  if (!GNOSTR_IS_ARTICLES_VIEW(self)) {
    g_warning("articles-view: widget no longer valid in callback");
    return;
  }

  self->fetch_in_progress = FALSE;

  GError *error = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);
  g_debug("articles-view: fetch complete, results=%p, results_len=%u, error=%s",
          (void*)results, results ? results->len : 0, error ? error->message : "none");

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("articles-view: Relay fetch error: %s", error->message);
    }
    g_clear_error(&error);
    gnostr_articles_view_set_loading(self, FALSE);
    return;
  }

  if (results && results->len > 0) {
    g_debug("articles-view: Fetched %u articles from relays", results->len);

    void *txn = NULL;
    if (storage_ndb_begin_query(&txn) == 0 && txn) {
      /* Get existing event IDs to avoid duplicates */
      GHashTable *existing_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      guint n_items = g_list_model_get_n_items(G_LIST_MODEL(self->articles_model));
      for (guint i = 0; i < n_items; i++) {
        GnostrArticleItem *item = g_list_model_get_item(G_LIST_MODEL(self->articles_model), i);
        if (item && item->event_id) {
          g_hash_table_add(existing_ids, g_strdup(item->event_id));
        }
        if (item) g_object_unref(item);
      }

      for (guint i = 0; i < results->len; i++) {
        const char *event_json = g_ptr_array_index(results, i);
        if (!event_json) continue;

        /* Ingest into nostrdb */
        storage_ndb_ingest_event_json(event_json, NULL);

        /* Create item and add to model if not duplicate */
        GnostrArticleItem *item = create_article_item_from_json(event_json, txn);
        if (item && item->event_id) {
          if (!g_hash_table_contains(existing_ids, item->event_id)) {
            g_hash_table_add(existing_ids, g_strdup(item->event_id));
            g_list_store_append(self->articles_model, item);
          }
          g_object_unref(item);
        } else if (item) {
          g_object_unref(item);
        }
      }

      g_hash_table_destroy(existing_ids);
      storage_ndb_end_query(txn);
    }
  }

  if (results) g_ptr_array_unref(results);

  g_debug("articles-view: calling set_loading(FALSE)");
  gnostr_articles_view_set_loading(self, FALSE);
  g_debug("articles-view: set_loading(FALSE) complete, model has %u items",
          g_list_model_get_n_items(G_LIST_MODEL(self->articles_model)));
}

/* --- Fetch articles from relays --- */
static void fetch_articles_from_relays(GnostrArticlesView *self) {
  if (self->fetch_in_progress) return;

  /* Get read relay URLs */
  GPtrArray *relay_arr = gnostr_get_read_relay_urls();
  if (!relay_arr || relay_arr->len == 0) {
    g_debug("articles-view: No relays configured for fetching");
    if (relay_arr) g_ptr_array_unref(relay_arr);
    gnostr_articles_view_set_loading(self, FALSE);
    return;
  }

  /* Convert to const char** */
  const char **urls = g_new0(const char*, relay_arr->len);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }

  /* Create filter for both article kinds */
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { KIND_LONG_FORM, KIND_WIKI };
  nostr_filter_set_kinds(filter, kinds, 2);
  nostr_filter_set_limit(filter, ARTICLES_FETCH_LIMIT);

  /* Uses shared query pool from gnostr_get_shared_query_pool() */

  /* Create cancellable */
  if (self->fetch_cancellable) {
    g_cancellable_cancel(self->fetch_cancellable);
    g_clear_object(&self->fetch_cancellable);
  }
  self->fetch_cancellable = g_cancellable_new();

  self->fetch_in_progress = TRUE;

  GNostrPool *pool = gnostr_get_shared_query_pool();
  g_debug("articles-view: Fetching articles from %u relays, pool=%p", relay_arr->len, (void*)pool);

  if (!pool) {
    g_warning("articles-view: shared query pool is NULL, cannot fetch");
    self->fetch_in_progress = FALSE;
    gnostr_articles_view_set_loading(self, FALSE);
    g_free(urls);
    g_ptr_array_unref(relay_arr);
    nostr_filter_free(filter);
    return;
  }

  gnostr_pool_sync_relays(pool, (const gchar **)urls, relay_arr->len);
  {
    static gint _qf_counter = 0;
    int _qfid = g_atomic_int_add(&_qf_counter, 1);
    char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-%d", _qfid);
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    g_object_set_data_full(G_OBJECT(pool), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
    gnostr_pool_query_async(pool, _qf, self->fetch_cancellable, on_relay_fetch_complete, self);
  }

  g_free(urls);
  g_ptr_array_unref(relay_arr);
  nostr_filter_free(filter);
}

/* --- Row signal handlers --- */

static void on_card_open_article(GtkWidget *card, const char *event_id, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  gint kind = KIND_LONG_FORM;

  /* Determine kind from card type */
  if (GNOSTR_IS_WIKI_CARD(card)) {
    kind = KIND_WIKI;
  }

  g_signal_emit(self, signals[SIGNAL_OPEN_ARTICLE], 0, event_id, kind);
}

static void on_card_open_profile(GtkWidget *card, const char *pubkey_hex, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)card;
  g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
}

static void on_card_topic_clicked(GtkWidget *card, const char *topic, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)card;

  /* Set topic filter */
  gnostr_articles_view_set_topic_filter(self, topic);

  g_signal_emit(self, signals[SIGNAL_TOPIC_CLICKED], 0, topic);
}

static void on_card_zap_requested(GtkWidget *card, const char *event_id,
                                   const char *pubkey_hex, const char *lud16,
                                   gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)card;
  g_signal_emit(self, signals[SIGNAL_ZAP_REQUESTED], 0, event_id, pubkey_hex, lud16);
}

static void on_card_bookmark_toggled(GtkWidget *card, const char *event_id,
                                      gboolean is_bookmarked, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)card;
  g_signal_emit(self, signals[SIGNAL_BOOKMARK_TOGGLED], 0, event_id, is_bookmarked);
}

/* --- List item factory --- */

static void setup_article_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  /* Create a placeholder box - actual card type determined in bind */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_list_item_set_child(list_item, box);
}

static void bind_article_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  GtkWidget *box = gtk_list_item_get_child(list_item);
  GnostrArticleItem *item = gtk_list_item_get_item(list_item);

  if (!item) return;

  /* Clear existing children */
  GtkWidget *child = gtk_widget_get_first_child(box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(box), child);
    child = next;
  }

  /* Create appropriate card based on kind */
  GtkWidget *card = NULL;

  if (item->kind == KIND_WIKI) {
    GnostrWikiCard *wiki_card = gnostr_wiki_card_new();
    card = GTK_WIDGET(wiki_card);

    gnostr_wiki_card_set_article(wiki_card,
                                  item->event_id,
                                  item->d_tag,
                                  item->title,
                                  item->summary,
                                  item->published_at,
                                  item->created_at);

    gnostr_wiki_card_set_author(wiki_card,
                                 item->author_name,
                                 item->author_handle,
                                 item->author_avatar,
                                 item->pubkey_hex);

    if (item->content) {
      gnostr_wiki_card_set_content(wiki_card, item->content);
    }

    if (item->topics && item->topics_count > 0) {
      gnostr_wiki_card_set_topics(wiki_card,
                                   (const char **)item->topics,
                                   item->topics_count);
    }

    if (item->author_nip05) {
      gnostr_wiki_card_set_nip05(wiki_card, item->author_nip05, item->pubkey_hex);
    }

    if (item->author_lud16) {
      gnostr_wiki_card_set_author_lud16(wiki_card, item->author_lud16);
    }

    gnostr_wiki_card_set_logged_in(wiki_card, self->is_logged_in);

    /* Connect signals */
    g_signal_connect(wiki_card, "open-article", G_CALLBACK(on_card_open_article), self);
    g_signal_connect(wiki_card, "open-profile", G_CALLBACK(on_card_open_profile), self);
    g_signal_connect(wiki_card, "topic-clicked", G_CALLBACK(on_card_topic_clicked), self);
    g_signal_connect(wiki_card, "zap-requested", G_CALLBACK(on_card_zap_requested), self);
    g_signal_connect(wiki_card, "bookmark-toggled", G_CALLBACK(on_card_bookmark_toggled), self);

  } else {
    /* KIND_LONG_FORM (30023) */
    GnostrArticleCard *article_card = gnostr_article_card_new();
    card = GTK_WIDGET(article_card);

    gnostr_article_card_set_article(article_card,
                                     item->event_id,
                                     item->d_tag,
                                     item->title,
                                     item->summary,
                                     item->image_url,
                                     item->published_at);

    gnostr_article_card_set_author(article_card,
                                    item->author_name,
                                    item->author_handle,
                                    item->author_avatar,
                                    item->pubkey_hex);

    if (item->content) {
      gnostr_article_card_set_content(article_card, item->content);
    }

    if (item->author_nip05) {
      gnostr_article_card_set_nip05(article_card, item->author_nip05, item->pubkey_hex);
    }

    if (item->author_lud16) {
      gnostr_article_card_set_author_lud16(article_card, item->author_lud16);
    }

    gnostr_article_card_set_logged_in(article_card, self->is_logged_in);

    /* Connect signals */
    g_signal_connect(article_card, "open-article", G_CALLBACK(on_card_open_article), self);
    g_signal_connect(article_card, "open-profile", G_CALLBACK(on_card_open_profile), self);
    g_signal_connect(article_card, "zap-requested", G_CALLBACK(on_card_zap_requested), self);
    g_signal_connect(article_card, "bookmark-toggled", G_CALLBACK(on_card_bookmark_toggled), self);
  }

  if (card) {
    gtk_box_append(GTK_BOX(box), card);
  }
}

static void unbind_article_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  GtkWidget *box = gtk_list_item_get_child(list_item);
  if (!box) return;

  /* Remove and destroy children */
  GtkWidget *child = gtk_widget_get_first_child(box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);

    /* Disconnect all signals */
    g_signal_handlers_disconnect_matched(child, G_SIGNAL_MATCH_DATA,
                                          0, 0, NULL, NULL, user_data);

    gtk_box_remove(GTK_BOX(box), child);
    child = next;
  }
}

/* --- Filter button handlers --- */

static void on_filter_all_toggled(GtkToggleButton *button, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  if (gtk_toggle_button_get_active(button)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_wiki), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_blog), FALSE);
    self->type_filter = GNOSTR_ARTICLES_TYPE_ALL;
    apply_filters(self);
  } else {
    /* Don't allow all to be inactive */
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_wiki)) &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_blog))) {
      gtk_toggle_button_set_active(button, TRUE);
    }
  }
}

static void on_filter_wiki_toggled(GtkToggleButton *button, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  if (gtk_toggle_button_get_active(button)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_blog), FALSE);
    self->type_filter = GNOSTR_ARTICLES_TYPE_WIKI;
    apply_filters(self);
  } else {
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_all)) &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_blog))) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), TRUE);
    }
  }
}

static void on_filter_blog_toggled(GtkToggleButton *button, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  if (gtk_toggle_button_get_active(button)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_wiki), FALSE);
    self->type_filter = GNOSTR_ARTICLES_TYPE_BLOG;
    apply_filters(self);
  } else {
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_all)) &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_wiki))) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), TRUE);
    }
  }
}

static void on_clear_topic_clicked(GtkButton *button, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)button;
  gnostr_articles_view_set_topic_filter(self, NULL);
}

/* --- Search handling --- */

static gboolean search_debounce_cb(gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
  g_free(self->search_text);
  self->search_text = g_strdup(text && *text ? text : NULL);

  apply_filters(self);

  return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);
  (void)entry;

  gnostr_debounce_trigger(self->search_debounce);
}

/* --- Filter application --- */

static gboolean filter_func(gpointer item, gpointer user_data) {
  GnostrArticleItem *article = GNOSTR_ARTICLE_ITEM(item);
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(user_data);

  if (!article) return FALSE;

  /* Type filter */
  if (self->type_filter == GNOSTR_ARTICLES_TYPE_WIKI && article->kind != KIND_WIKI) {
    return FALSE;
  }
  if (self->type_filter == GNOSTR_ARTICLES_TYPE_BLOG && article->kind != KIND_LONG_FORM) {
    return FALSE;
  }

  /* Topic filter */
  if (self->topic_filter && *self->topic_filter) {
    gboolean topic_match = FALSE;
    if (article->topics) {
      for (gsize i = 0; i < article->topics_count; i++) {
        if (g_ascii_strcasecmp(article->topics[i], self->topic_filter) == 0) {
          topic_match = TRUE;
          break;
        }
      }
    }
    if (!topic_match) return FALSE;
  }

  /* Search text filter */
  if (self->search_text && *self->search_text) {
    gchar *search_lower = g_utf8_strdown(self->search_text, -1);
    gboolean match = FALSE;

    if (article->title) {
      gchar *title_lower = g_utf8_strdown(article->title, -1);
      if (strstr(title_lower, search_lower)) match = TRUE;
      g_free(title_lower);
    }

    if (!match && article->summary) {
      gchar *summary_lower = g_utf8_strdown(article->summary, -1);
      if (strstr(summary_lower, search_lower)) match = TRUE;
      g_free(summary_lower);
    }

    if (!match && article->author_name) {
      gchar *name_lower = g_utf8_strdown(article->author_name, -1);
      if (strstr(name_lower, search_lower)) match = TRUE;
      g_free(name_lower);
    }

    g_free(search_lower);
    if (!match) return FALSE;
  }

  return TRUE;
}

static void apply_filters(GnostrArticlesView *self) {
  /* Notify the filter that it needs to re-evaluate all items */
  if (self->custom_filter) {
    gtk_filter_changed(GTK_FILTER(self->custom_filter), GTK_FILTER_CHANGE_DIFFERENT);
  }
  update_content_state(self);
}

/* --- State updates --- */

static void update_article_count(GnostrArticlesView *self) {
  guint total = g_list_model_get_n_items(G_LIST_MODEL(self->articles_model));
  guint filtered = self->filtered_model ?
    g_list_model_get_n_items(G_LIST_MODEL(self->filtered_model)) : total;

  gchar *text;
  if (filtered == total) {
    text = g_strdup_printf("%u articles", total);
  } else {
    text = g_strdup_printf("%u of %u articles", filtered, total);
  }
  gtk_label_set_text(GTK_LABEL(self->lbl_count), text);
  g_free(text);
}

static void update_content_state(GnostrArticlesView *self) {
  guint model_count = g_list_model_get_n_items(G_LIST_MODEL(self->articles_model));
  guint count = self->filtered_model ?
    g_list_model_get_n_items(G_LIST_MODEL(self->filtered_model)) : model_count;

  g_debug("articles-view: update_content_state model=%u filtered=%u", model_count, count);

  if (count == 0) {
    g_debug("articles-view: switching stack to 'empty'");
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "empty");
  } else {
    g_debug("articles-view: switching stack to 'results'");
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "results");
  }

  /* Update topic filter visibility */
  if (self->topic_filter && *self->topic_filter) {
    gtk_label_set_text(GTK_LABEL(self->topic_filter_label), self->topic_filter);
    gtk_widget_set_visible(self->topic_filter_box, TRUE);
  } else {
    gtk_widget_set_visible(self->topic_filter_box, FALSE);
  }

  update_article_count(self);
}

/* --- GObject implementation --- */

static void gnostr_articles_view_dispose(GObject *object) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(object);

  gnostr_debounce_free(self->search_debounce);

  /* Cancel any pending fetch */
  if (self->fetch_cancellable) {
    g_cancellable_cancel(self->fetch_cancellable);
    g_clear_object(&self->fetch_cancellable);
  }

  /* Shared query pool is managed globally - do not clear here */
  g_clear_object(&self->custom_filter);
  g_clear_object(&self->filtered_model);
  g_clear_object(&self->articles_model);
  g_clear_object(&self->selection);
  g_clear_object(&self->factory);

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_ARTICLES_VIEW);

  G_OBJECT_CLASS(gnostr_articles_view_parent_class)->dispose(object);
}

static void gnostr_articles_view_finalize(GObject *object) {
  GnostrArticlesView *self = GNOSTR_ARTICLES_VIEW(object);

  g_free(self->topic_filter);
  g_free(self->search_text);

  G_OBJECT_CLASS(gnostr_articles_view_parent_class)->finalize(object);
}

static void gnostr_articles_view_class_init(GnostrArticlesViewClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_articles_view_dispose;
  object_class->finalize = gnostr_articles_view_finalize;

  /* Load template */
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, root);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, header_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, title_label);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, search_entry);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, btn_all);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, btn_wiki);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, btn_blog);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, lbl_count);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, content_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, articles_scroll);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, articles_list);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, empty_state);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, loading_spinner);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, topic_filter_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, topic_filter_label);
  gtk_widget_class_bind_template_child(widget_class, GnostrArticlesView, btn_clear_topic);

  /* Signals */
  signals[SIGNAL_OPEN_ARTICLE] = g_signal_new(
    "open-article",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

  signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
    "open-profile",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_TOPIC_CLICKED] = g_signal_new(
    "topic-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_ZAP_REQUESTED] = g_signal_new(
    "zap-requested",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_BOOKMARK_TOGGLED] = g_signal_new(
    "bookmark-toggled",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  /* CSS */
  gtk_widget_class_set_css_name(widget_class, "articles-view");

  /* Layout */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void gnostr_articles_view_init(GnostrArticlesView *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->type_filter = GNOSTR_ARTICLES_TYPE_ALL;
  self->topic_filter = NULL;
  self->search_text = NULL;
  self->articles_loaded = FALSE;
  self->is_logged_in = FALSE;
  self->search_debounce = gnostr_debounce_new(300, search_debounce_cb, self);
  self->fetch_cancellable = NULL;
  /* Uses shared query pool from gnostr_get_shared_query_pool() */
  self->fetch_in_progress = FALSE;

  /* Create model with filter */
  self->articles_model = g_list_store_new(GNOSTR_TYPE_ARTICLE_ITEM);

  /* Create custom filter */
  self->custom_filter = gtk_custom_filter_new(filter_func, self, NULL);

  /* Create filtered model */
  self->filtered_model = gtk_filter_list_model_new(
    G_LIST_MODEL(g_object_ref(self->articles_model)),
    GTK_FILTER(g_object_ref(self->custom_filter))
  );

  /* Create selection model on filtered model */
  self->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->filtered_model)));
  gtk_single_selection_set_autoselect(self->selection, FALSE);
  gtk_single_selection_set_can_unselect(self->selection, TRUE);

  /* Create factory */
  self->factory = gtk_signal_list_item_factory_new();
  g_signal_connect(self->factory, "setup", G_CALLBACK(setup_article_row), self);
  g_signal_connect(self->factory, "bind", G_CALLBACK(bind_article_row), self);
  g_signal_connect(self->factory, "unbind", G_CALLBACK(unbind_article_row), self);

  /* Set up list view */
  gtk_list_view_set_model(GTK_LIST_VIEW(self->articles_list),
                          GTK_SELECTION_MODEL(self->selection));
  gtk_list_view_set_factory(GTK_LIST_VIEW(self->articles_list), self->factory);

  /* Connect filter button signals */
  g_signal_connect(self->btn_all, "toggled", G_CALLBACK(on_filter_all_toggled), self);
  g_signal_connect(self->btn_wiki, "toggled", G_CALLBACK(on_filter_wiki_toggled), self);
  g_signal_connect(self->btn_blog, "toggled", G_CALLBACK(on_filter_blog_toggled), self);
  g_signal_connect(self->btn_clear_topic, "clicked", G_CALLBACK(on_clear_topic_clicked), self);

  /* Connect search signal */
  g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);

  /* Default to All filter active */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), TRUE);

  /* Start with empty state */
  gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "empty");
}

GnostrArticlesView *gnostr_articles_view_new(void) {
  return g_object_new(GNOSTR_TYPE_ARTICLES_VIEW, NULL);
}

void gnostr_articles_view_set_type_filter(GnostrArticlesView *self,
                                           GnostrArticlesType type) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  if (self->type_filter == type) return;

  self->type_filter = type;

  /* Update toggle buttons */
  switch (type) {
    case GNOSTR_ARTICLES_TYPE_ALL:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_all), TRUE);
      break;
    case GNOSTR_ARTICLES_TYPE_WIKI:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_wiki), TRUE);
      break;
    case GNOSTR_ARTICLES_TYPE_BLOG:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_blog), TRUE);
      break;
  }
}

GnostrArticlesType gnostr_articles_view_get_type_filter(GnostrArticlesView *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLES_VIEW(self), GNOSTR_ARTICLES_TYPE_ALL);
  return self->type_filter;
}

void gnostr_articles_view_set_topic_filter(GnostrArticlesView *self,
                                            const char *topic) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  g_free(self->topic_filter);
  self->topic_filter = g_strdup(topic);

  apply_filters(self);
}

const char *gnostr_articles_view_get_topic_filter(GnostrArticlesView *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLES_VIEW(self), NULL);
  return self->topic_filter;
}

void gnostr_articles_view_set_search_text(GnostrArticlesView *self,
                                           const char *text) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  gtk_editable_set_text(GTK_EDITABLE(self->search_entry), text ? text : "");
}

void gnostr_articles_view_load_articles(GnostrArticlesView *self) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  if (self->articles_loaded) return;

  self->articles_loaded = TRUE;

  /* Show loading state */
  gnostr_articles_view_set_loading(self, TRUE);

  /* First, load from local nostrdb cache */
  load_articles_from_nostrdb(self);

  /* If we have some articles, show them immediately while fetching more */
  guint local_count = g_list_model_get_n_items(G_LIST_MODEL(self->articles_model));
  if (local_count > 0) {
    g_debug("articles-view: Showing %u local articles while fetching from relays", local_count);
    update_content_state(self);
  }

  /* Fetch from relays to get more/newer articles */
  fetch_articles_from_relays(self);

  /* If no local articles, keep loading state until relay fetch completes */
  if (local_count == 0 && !self->fetch_in_progress) {
    gnostr_articles_view_set_loading(self, FALSE);
  }
}

void gnostr_articles_view_refresh(GnostrArticlesView *self) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  /* Cancel any pending fetch */
  if (self->fetch_cancellable) {
    g_cancellable_cancel(self->fetch_cancellable);
    g_clear_object(&self->fetch_cancellable);
  }
  self->fetch_in_progress = FALSE;

  self->articles_loaded = FALSE;
  g_list_store_remove_all(self->articles_model);
  gnostr_articles_view_load_articles(self);
}

void gnostr_articles_view_set_loading(GnostrArticlesView *self,
                                       gboolean is_loading) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));

  g_debug("articles-view: set_loading(%s)", is_loading ? "TRUE" : "FALSE");

  if (is_loading) {
    gtk_spinner_start(GTK_SPINNER(self->loading_spinner));
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "loading");
  } else {
    gtk_spinner_stop(GTK_SPINNER(self->loading_spinner));
    update_content_state(self);
  }

  g_debug("articles-view: stack now showing '%s'",
          gtk_stack_get_visible_child_name(GTK_STACK(self->content_stack)));
}

guint gnostr_articles_view_get_article_count(GnostrArticlesView *self) {
  g_return_val_if_fail(GNOSTR_IS_ARTICLES_VIEW(self), 0);
  return g_list_model_get_n_items(G_LIST_MODEL(self->articles_model));
}

void gnostr_articles_view_set_logged_in(GnostrArticlesView *self,
                                         gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_ARTICLES_VIEW(self));
  self->is_logged_in = logged_in;
}
