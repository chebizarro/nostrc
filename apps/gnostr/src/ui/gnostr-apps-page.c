/**
 * GnostrAppsPage - App Handler Discovery Page Implementation
 */

#define G_LOG_DOMAIN "gnostr-apps-page"

#include "gnostr-apps-page.h"
#include "gnostr-avatar-cache.h"
#include "../util/nip89_handlers.h"

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-apps-page.ui"

struct _GnostrAppsPage {
  GtkWidget parent_instance;

  /* Template widgets */
  GtkSearchEntry *search_entry;
  GtkDropDown *kind_filter;
  GtkListView *handler_list;
  GtkStack *content_stack;
  GtkSpinner *loading_spinner;
  GtkLabel *count_label;
  GtkScrolledWindow *scroller;

  /* Model */
  GListStore *handler_model;
  GtkSingleSelection *selection;
  GtkListItemFactory *factory;

  /* State */
  guint filter_kind;
  char *search_text;
  guint search_debounce_id;
  GHashTable *followed_set;
  GCancellable *query_cancellable;
};

G_DEFINE_FINAL_TYPE(GnostrAppsPage, gnostr_apps_page, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_HANDLER_WEBSITE,
  SIGNAL_PREFERENCE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ============== Handler Item GObject ============== */

#define GNOSTR_TYPE_HANDLER_ITEM (gnostr_handler_item_get_type())
G_DECLARE_FINAL_TYPE(GnostrHandlerItem, gnostr_handler_item, GNOSTR, HANDLER_ITEM, GObject)

struct _GnostrHandlerItem {
  GObject parent_instance;
  GnostrNip89HandlerInfo *handler;  /* Not owned */
  gboolean is_preferred;
  guint recommendation_count;
};

G_DEFINE_FINAL_TYPE(GnostrHandlerItem, gnostr_handler_item, G_TYPE_OBJECT)

static void
gnostr_handler_item_class_init(GnostrHandlerItemClass *klass)
{
  (void)klass;
}

static void
gnostr_handler_item_init(GnostrHandlerItem *self)
{
  self->handler = NULL;
  self->is_preferred = FALSE;
  self->recommendation_count = 0;
}

static GnostrHandlerItem *
gnostr_handler_item_new(GnostrNip89HandlerInfo *handler)
{
  GnostrHandlerItem *item = g_object_new(GNOSTR_TYPE_HANDLER_ITEM, NULL);
  item->handler = handler;
  return item;
}

/* ============== Row Factory ============== */

static void
setup_handler_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  (void)user_data;

  /* Create card-style row */
  GtkBox *card = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
  gtk_widget_add_css_class(GTK_WIDGET(card), "card");
  gtk_widget_set_margin_top(GTK_WIDGET(card), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(card), 6);
  gtk_widget_set_margin_start(GTK_WIDGET(card), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(card), 12);

  /* Header row with icon and name */
  GtkBox *header = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
  gtk_widget_set_margin_top(GTK_WIDGET(header), 12);
  gtk_widget_set_margin_start(GTK_WIDGET(header), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(header), 12);

  /* Icon */
  GtkImage *icon = GTK_IMAGE(gtk_image_new_from_icon_name("application-x-executable-symbolic"));
  gtk_image_set_pixel_size(icon, 48);
  gtk_widget_set_name(GTK_WIDGET(icon), "handler-icon");
  gtk_box_append(header, GTK_WIDGET(icon));

  /* Name and subtitle */
  GtkBox *name_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2));
  gtk_widget_set_hexpand(GTK_WIDGET(name_box), TRUE);

  GtkLabel *name = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(name, 0);
  gtk_widget_add_css_class(GTK_WIDGET(name), "title-3");
  gtk_widget_set_name(GTK_WIDGET(name), "handler-name");
  gtk_box_append(name_box, GTK_WIDGET(name));

  GtkLabel *subtitle = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(subtitle, 0);
  gtk_widget_add_css_class(GTK_WIDGET(subtitle), "dim-label");
  gtk_widget_set_name(GTK_WIDGET(subtitle), "handler-subtitle");
  gtk_box_append(name_box, GTK_WIDGET(subtitle));

  gtk_box_append(header, GTK_WIDGET(name_box));

  /* Preferred indicator */
  GtkImage *preferred = GTK_IMAGE(gtk_image_new_from_icon_name("emblem-default-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(preferred), "success");
  gtk_widget_set_name(GTK_WIDGET(preferred), "preferred-icon");
  gtk_widget_set_tooltip_text(GTK_WIDGET(preferred), "Your preferred handler");
  gtk_widget_set_visible(GTK_WIDGET(preferred), FALSE);
  gtk_box_append(header, GTK_WIDGET(preferred));

  gtk_box_append(card, GTK_WIDGET(header));

  /* Description */
  GtkLabel *desc = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(desc, 0);
  gtk_label_set_wrap(desc, TRUE);
  gtk_label_set_wrap_mode(desc, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(desc, 60);
  gtk_widget_set_margin_start(GTK_WIDGET(desc), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(desc), 12);
  gtk_widget_set_name(GTK_WIDGET(desc), "handler-description");
  gtk_box_append(card, GTK_WIDGET(desc));

  /* Supported kinds */
  GtkLabel *kinds = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(kinds, 0);
  gtk_label_set_wrap(kinds, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(kinds), "caption");
  gtk_widget_set_margin_start(GTK_WIDGET(kinds), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(kinds), 12);
  gtk_widget_set_name(GTK_WIDGET(kinds), "handler-kinds");
  gtk_box_append(card, GTK_WIDGET(kinds));

  /* Platforms */
  GtkLabel *platforms = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(platforms, 0);
  gtk_widget_add_css_class(GTK_WIDGET(platforms), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(platforms), "accent");
  gtk_widget_set_margin_start(GTK_WIDGET(platforms), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(platforms), 12);
  gtk_widget_set_name(GTK_WIDGET(platforms), "handler-platforms");
  gtk_box_append(card, GTK_WIDGET(platforms));

  /* Action buttons */
  GtkBox *actions = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
  gtk_widget_set_halign(GTK_WIDGET(actions), GTK_ALIGN_END);
  gtk_widget_set_margin_top(GTK_WIDGET(actions), 8);
  gtk_widget_set_margin_bottom(GTK_WIDGET(actions), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(actions), 12);

  GtkButton *website_btn = GTK_BUTTON(gtk_button_new_with_label("Website"));
  gtk_widget_add_css_class(GTK_WIDGET(website_btn), "flat");
  gtk_widget_set_name(GTK_WIDGET(website_btn), "website-button");
  gtk_box_append(actions, GTK_WIDGET(website_btn));

  GtkButton *prefer_btn = GTK_BUTTON(gtk_button_new_with_label("Set as Default"));
  gtk_widget_add_css_class(GTK_WIDGET(prefer_btn), "suggested-action");
  gtk_widget_set_name(GTK_WIDGET(prefer_btn), "prefer-button");
  gtk_box_append(actions, GTK_WIDGET(prefer_btn));

  gtk_box_append(card, GTK_WIDGET(actions));

  gtk_list_item_set_child(list_item, GTK_WIDGET(card));
}

static GtkWidget *
find_child_by_name(GtkWidget *parent, const char *name)
{
  for (GtkWidget *child = gtk_widget_get_first_child(parent);
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (g_strcmp0(gtk_widget_get_name(child), name) == 0) {
      return child;
    }
    GtkWidget *found = find_child_by_name(child, name);
    if (found) return found;
  }
  return NULL;
}

static void
bind_handler_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  GnostrAppsPage *self = GNOSTR_APPS_PAGE(user_data);

  GtkWidget *card = gtk_list_item_get_child(list_item);
  GnostrHandlerItem *item = gtk_list_item_get_item(list_item);
  if (!card || !item || !item->handler) return;

  GnostrNip89HandlerInfo *handler = item->handler;

  /* Find widgets */
  GtkImage *icon = GTK_IMAGE(find_child_by_name(card, "handler-icon"));
  GtkLabel *name = GTK_LABEL(find_child_by_name(card, "handler-name"));
  GtkLabel *subtitle = GTK_LABEL(find_child_by_name(card, "handler-subtitle"));
  GtkLabel *desc = GTK_LABEL(find_child_by_name(card, "handler-description"));
  GtkLabel *kinds = GTK_LABEL(find_child_by_name(card, "handler-kinds"));
  GtkLabel *platforms = GTK_LABEL(find_child_by_name(card, "handler-platforms"));
  GtkImage *preferred = GTK_IMAGE(find_child_by_name(card, "preferred-icon"));
  GtkButton *website_btn = GTK_BUTTON(find_child_by_name(card, "website-button"));
  GtkButton *prefer_btn = GTK_BUTTON(find_child_by_name(card, "prefer-button"));

  /* Set name */
  if (name) {
    const char *display = handler->display_name ? handler->display_name :
                          handler->name ? handler->name : handler->d_tag;
    gtk_label_set_text(name, display);
  }

  /* Set subtitle (NIP-05 or pubkey) */
  if (subtitle) {
    if (handler->nip05) {
      gtk_label_set_text(subtitle, handler->nip05);
    } else if (handler->pubkey_hex) {
      char short_pk[16];
      snprintf(short_pk, sizeof(short_pk), "%.8s...", handler->pubkey_hex);
      gtk_label_set_text(subtitle, short_pk);
    }
  }

  /* Set description */
  if (desc) {
    if (handler->about && *handler->about) {
      gtk_label_set_text(desc, handler->about);
      gtk_widget_set_visible(GTK_WIDGET(desc), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(desc), FALSE);
    }
  }

  /* Set supported kinds */
  if (kinds) {
    if (handler->handled_kinds && handler->n_handled_kinds > 0) {
      GString *kinds_str = g_string_new("Handles: ");
      for (gsize i = 0; i < handler->n_handled_kinds && i < 5; i++) {
        if (i > 0) g_string_append(kinds_str, ", ");
        guint k = handler->handled_kinds[i];
        const char *k_desc = gnostr_nip89_get_kind_description(k);
        g_string_append_printf(kinds_str, "%s (%u)", k_desc, k);
      }
      if (handler->n_handled_kinds > 5) {
        g_string_append_printf(kinds_str, " +%zu more", handler->n_handled_kinds - 5);
      }
      gtk_label_set_text(kinds, kinds_str->str);
      g_string_free(kinds_str, TRUE);
      gtk_widget_set_visible(GTK_WIDGET(kinds), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(kinds), FALSE);
    }
  }

  /* Set platforms */
  if (platforms) {
    if (handler->platforms && handler->platforms->len > 0) {
      GString *plat_str = g_string_new("Available: ");
      for (guint i = 0; i < handler->platforms->len; i++) {
        GnostrNip89PlatformHandler *ph = g_ptr_array_index(handler->platforms, i);
        if (i > 0) g_string_append(plat_str, ", ");
        g_string_append(plat_str, gnostr_nip89_platform_to_string(ph->platform));
      }
      gtk_label_set_text(platforms, plat_str->str);
      g_string_free(plat_str, TRUE);
      gtk_widget_set_visible(GTK_WIDGET(platforms), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(platforms), FALSE);
    }
  }

  /* Preferred indicator */
  if (preferred) {
    gtk_widget_set_visible(GTK_WIDGET(preferred), item->is_preferred);
  }

  /* Website button */
  if (website_btn) {
    gtk_widget_set_visible(GTK_WIDGET(website_btn), handler->website != NULL);
    g_object_set_data_full(G_OBJECT(website_btn), "website",
                           g_strdup(handler->website), g_free);
    g_signal_handlers_disconnect_by_data(website_btn, self);
    g_signal_connect(website_btn, "clicked", G_CALLBACK(({
      void inner(GtkButton *btn, gpointer user_data) {
        GnostrAppsPage *page = GNOSTR_APPS_PAGE(user_data);
        const char *url = g_object_get_data(G_OBJECT(btn), "website");
        if (url) {
          g_signal_emit(page, signals[SIGNAL_OPEN_HANDLER_WEBSITE], 0, url);
        }
      }
      inner;
    })), self);
  }

  /* Set as default button */
  if (prefer_btn) {
    if (item->is_preferred) {
      gtk_button_set_label(prefer_btn, "Default");
      gtk_widget_remove_css_class(GTK_WIDGET(prefer_btn), "suggested-action");
      gtk_widget_add_css_class(GTK_WIDGET(prefer_btn), "flat");
      gtk_widget_set_sensitive(GTK_WIDGET(prefer_btn), FALSE);
    } else {
      gtk_button_set_label(prefer_btn, "Set as Default");
      gtk_widget_remove_css_class(GTK_WIDGET(prefer_btn), "flat");
      gtk_widget_add_css_class(GTK_WIDGET(prefer_btn), "suggested-action");
      gtk_widget_set_sensitive(GTK_WIDGET(prefer_btn), TRUE);
    }

    /* Store handler reference for callback */
    g_object_set_data(G_OBJECT(prefer_btn), "handler", handler);
    g_signal_handlers_disconnect_by_data(prefer_btn, self);
    g_signal_connect(prefer_btn, "clicked", G_CALLBACK(({
      void inner(GtkButton *btn, gpointer user_data) {
        GnostrAppsPage *page = GNOSTR_APPS_PAGE(user_data);
        GnostrNip89HandlerInfo *h = g_object_get_data(G_OBJECT(btn), "handler");
        if (h && h->handled_kinds && h->n_handled_kinds > 0) {
          /* Set as preferred for first handled kind */
          guint kind = h->handled_kinds[0];
          char *a_tag = g_strdup_printf("%d:%s:%s",
                                         GNOSTR_NIP89_KIND_HANDLER_INFO,
                                         h->pubkey_hex, h->d_tag);
          gnostr_nip89_set_preferred_handler(kind, a_tag);
          g_signal_emit(page, signals[SIGNAL_PREFERENCE_CHANGED], 0, kind, a_tag);
          g_free(a_tag);
          gnostr_apps_page_refresh(page);
        }
      }
      inner;
    })), self);
  }

  /* Load icon */
  if (icon && handler->picture && *handler->picture) {
    gnostr_avatar_cache_load_async(handler->picture, 48, NULL,
      (GAsyncReadyCallback)({
        void inner(GObject *source, GAsyncResult *res, gpointer user_data) {
          (void)source;
          GtkImage *img = GTK_IMAGE(user_data);
          if (!GTK_IS_IMAGE(img)) return;
          GdkTexture *texture = gnostr_avatar_cache_load_finish(res, NULL);
          if (texture) {
            gtk_image_set_from_paintable(img, GDK_PAINTABLE(texture));
            g_object_unref(texture);
          }
        }
        inner;
      }), icon);
  }
}

static void
unbind_handler_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void)factory;
  (void)user_data;

  GtkWidget *card = gtk_list_item_get_child(list_item);
  if (!card) return;

  GtkButton *website_btn = GTK_BUTTON(find_child_by_name(card, "website-button"));
  GtkButton *prefer_btn = GTK_BUTTON(find_child_by_name(card, "prefer-button"));

  if (website_btn) g_signal_handlers_disconnect_by_data(website_btn, user_data);
  if (prefer_btn) g_signal_handlers_disconnect_by_data(prefer_btn, user_data);
}

/* ============== Search and Filter ============== */

static gboolean
handler_matches_filter(GnostrAppsPage *self, GnostrNip89HandlerInfo *handler)
{
  /* Kind filter */
  if (self->filter_kind > 0) {
    gboolean found = FALSE;
    for (gsize i = 0; i < handler->n_handled_kinds; i++) {
      if (handler->handled_kinds[i] == self->filter_kind) {
        found = TRUE;
        break;
      }
    }
    if (!found) return FALSE;
  }

  /* Search text filter */
  if (self->search_text && *self->search_text) {
    const char *name = handler->display_name ? handler->display_name : handler->name;
    if (name && strcasestr(name, self->search_text)) return TRUE;
    if (handler->about && strcasestr(handler->about, self->search_text)) return TRUE;
    if (handler->d_tag && strcasestr(handler->d_tag, self->search_text)) return TRUE;
    return FALSE;
  }

  return TRUE;
}

static void
update_handler_list(GnostrAppsPage *self)
{
  /* Clear model */
  g_list_store_remove_all(self->handler_model);

  /* Get all handlers from cache */
  GPtrArray *all_handlers = gnostr_nip89_cache_get_all_handlers();
  if (!all_handlers || all_handlers->len == 0) {
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
    if (all_handlers) g_ptr_array_unref(all_handlers);
    return;
  }

  guint visible_count = 0;
  for (guint i = 0; i < all_handlers->len; i++) {
    GnostrNip89HandlerInfo *handler = g_ptr_array_index(all_handlers, i);

    if (!handler_matches_filter(self, handler)) continue;

    GnostrHandlerItem *item = gnostr_handler_item_new(handler);

    /* Check if preferred for any handled kind */
    for (gsize j = 0; j < handler->n_handled_kinds; j++) {
      GnostrNip89HandlerInfo *pref = gnostr_nip89_get_preferred_handler(handler->handled_kinds[j]);
      if (pref && g_strcmp0(pref->pubkey_hex, handler->pubkey_hex) == 0 &&
          g_strcmp0(pref->d_tag, handler->d_tag) == 0) {
        item->is_preferred = TRUE;
        break;
      }
    }

    g_list_store_append(self->handler_model, item);
    g_object_unref(item);
    visible_count++;
  }

  g_ptr_array_unref(all_handlers);

  /* Update UI */
  if (visible_count == 0) {
    gtk_stack_set_visible_child_name(self->content_stack,
                                      self->search_text ? "no-results" : "empty");
  } else {
    gtk_stack_set_visible_child_name(self->content_stack, "list");
  }

  /* Update count label */
  char *count_text = g_strdup_printf("%u app%s", visible_count, visible_count == 1 ? "" : "s");
  gtk_label_set_text(self->count_label, count_text);
  g_free(count_text);
}

static gboolean
search_debounce_cb(gpointer user_data)
{
  GnostrAppsPage *self = GNOSTR_APPS_PAGE(user_data);
  self->search_debounce_id = 0;
  update_handler_list(self);
  return G_SOURCE_REMOVE;
}

static void
on_search_changed(GtkSearchEntry *entry, GnostrAppsPage *self)
{
  g_free(self->search_text);
  self->search_text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));

  if (self->search_debounce_id) {
    g_source_remove(self->search_debounce_id);
  }
  self->search_debounce_id = g_timeout_add(300, search_debounce_cb, self);
}

static void
on_kind_filter_changed(GtkDropDown *dropdown, GParamSpec *pspec, GnostrAppsPage *self)
{
  (void)pspec;

  guint selected = gtk_drop_down_get_selected(dropdown);

  /* Map selection to kind (0 = all, then common kinds) */
  static const guint kind_map[] = {
    0,     /* All */
    1,     /* Short Text Note */
    30023, /* Long-form Content */
    4,     /* Encrypted DM */
    30311, /* Live Event */
    34235, /* Video */
    9735,  /* Zap */
    30018, /* Product */
  };

  if (selected < G_N_ELEMENTS(kind_map)) {
    self->filter_kind = kind_map[selected];
  } else {
    self->filter_kind = 0;
  }

  update_handler_list(self);
}

/* ============== GObject Implementation ============== */

static void
gnostr_apps_page_dispose(GObject *object)
{
  GnostrAppsPage *self = GNOSTR_APPS_PAGE(object);

  if (self->search_debounce_id) {
    g_source_remove(self->search_debounce_id);
    self->search_debounce_id = 0;
  }

  if (self->query_cancellable) {
    g_cancellable_cancel(self->query_cancellable);
    g_clear_object(&self->query_cancellable);
  }

  g_clear_object(&self->handler_model);
  g_clear_object(&self->selection);
  g_clear_object(&self->factory);
  g_clear_pointer(&self->search_text, g_free);
  g_clear_pointer(&self->followed_set, g_hash_table_destroy);

  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  if (child) gtk_widget_unparent(child);

  G_OBJECT_CLASS(gnostr_apps_page_parent_class)->dispose(object);
}

static void
gnostr_apps_page_class_init(GnostrAppsPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_apps_page_dispose;

  /* Signals */
  signals[SIGNAL_OPEN_HANDLER_WEBSITE] = g_signal_new(
    "open-handler-website",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_PREFERENCE_CHANGED] = g_signal_new(
    "preference-changed",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_STRING);

  /* Template */
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  gtk_widget_class_bind_template_child(widget_class, GnostrAppsPage, search_entry);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppsPage, kind_filter);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppsPage, handler_list);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppsPage, content_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppsPage, loading_spinner);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppsPage, count_label);
  gtk_widget_class_bind_template_child(widget_class, GnostrAppsPage, scroller);

  gtk_widget_class_set_css_name(widget_class, "apps-page");
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_apps_page_init(GnostrAppsPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  self->filter_kind = 0;
  self->search_text = NULL;
  self->search_debounce_id = 0;
  self->followed_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  /* Create model */
  self->handler_model = g_list_store_new(GNOSTR_TYPE_HANDLER_ITEM);
  self->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->handler_model)));
  gtk_single_selection_set_autoselect(self->selection, FALSE);
  gtk_single_selection_set_can_unselect(self->selection, TRUE);

  /* Create factory */
  self->factory = gtk_signal_list_item_factory_new();
  g_signal_connect(self->factory, "setup", G_CALLBACK(setup_handler_row), self);
  g_signal_connect(self->factory, "bind", G_CALLBACK(bind_handler_row), self);
  g_signal_connect(self->factory, "unbind", G_CALLBACK(unbind_handler_row), self);

  /* Set up list view */
  gtk_list_view_set_model(self->handler_list, GTK_SELECTION_MODEL(self->selection));
  gtk_list_view_set_factory(self->handler_list, self->factory);

  /* Connect signals */
  g_signal_connect(self->search_entry, "search-changed",
                   G_CALLBACK(on_search_changed), self);
  g_signal_connect(self->kind_filter, "notify::selected",
                   G_CALLBACK(on_kind_filter_changed), self);

  /* Initial state */
  gtk_stack_set_visible_child_name(self->content_stack, "empty");
}

GnostrAppsPage *
gnostr_apps_page_new(void)
{
  return g_object_new(GNOSTR_TYPE_APPS_PAGE, NULL);
}

void
gnostr_apps_page_refresh(GnostrAppsPage *self)
{
  g_return_if_fail(GNOSTR_IS_APPS_PAGE(self));
  update_handler_list(self);
}

void
gnostr_apps_page_set_loading(GnostrAppsPage *self, gboolean is_loading)
{
  g_return_if_fail(GNOSTR_IS_APPS_PAGE(self));

  if (is_loading) {
    gtk_spinner_start(self->loading_spinner);
    gtk_stack_set_visible_child_name(self->content_stack, "loading");
  } else {
    gtk_spinner_stop(self->loading_spinner);
    update_handler_list(self);
  }
}

void
gnostr_apps_page_filter_by_kind(GnostrAppsPage *self, guint kind)
{
  g_return_if_fail(GNOSTR_IS_APPS_PAGE(self));
  self->filter_kind = kind;
  update_handler_list(self);
}

guint
gnostr_apps_page_get_handler_count(GnostrAppsPage *self)
{
  g_return_val_if_fail(GNOSTR_IS_APPS_PAGE(self), 0);
  return g_list_model_get_n_items(G_LIST_MODEL(self->handler_model));
}

void
gnostr_apps_page_set_followed_pubkeys(GnostrAppsPage *self, const char **pubkeys)
{
  g_return_if_fail(GNOSTR_IS_APPS_PAGE(self));

  g_hash_table_remove_all(self->followed_set);

  if (pubkeys) {
    for (const char **p = pubkeys; *p; p++) {
      g_hash_table_add(self->followed_set, g_strdup(*p));
    }
  }
}
