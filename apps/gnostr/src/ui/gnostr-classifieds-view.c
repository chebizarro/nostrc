/*
 * gnostr-classifieds-view.c - NIP-99 Classified Listings Grid View
 *
 * Displays a responsive grid of classified listing cards.
 */

#include "gnostr-classifieds-view.h"
#include "gnostr-classified-card.h"
#include <glib/gi18n.h>

/* Minimum column width for responsive layout */
#define MIN_COLUMN_WIDTH 300

struct _GnostrClassifiedsView {
  GtkWidget parent_instance;

  /* Main layout */
  GtkWidget *root_box;
  GtkWidget *filter_bar;
  GtkWidget *content_stack;
  GtkWidget *scrolled_window;
  GtkWidget *grid_box;
  GtkWidget *empty_state;
  GtkWidget *loading_spinner;

  /* Filter bar widgets */
  GtkWidget *search_entry;
  GtkWidget *category_dropdown;
  GtkWidget *location_entry;
  GtkWidget *price_min_entry;
  GtkWidget *price_max_entry;
  GtkWidget *currency_dropdown;
  GtkWidget *sort_dropdown;
  GtkWidget *btn_clear_filters;

  /* State */
  gchar *category_filter;
  gchar *location_filter;
  gchar *search_text;
  gdouble price_min;
  gdouble price_max;
  gchar *price_currency;
  GnostrClassifiedsSortOrder sort_order;
  gboolean is_loading;
  gboolean is_logged_in;
  gchar *user_pubkey;
  guint columns;

  /* Listings storage */
  GHashTable *listings;       /* event_id -> GnostrClassifiedCard* */
  GPtrArray *listing_data;    /* GnostrClassified* (owned) */
  GPtrArray *visible_cards;   /* GnostrClassifiedCard* (not owned) */

  /* Category options */
  GPtrArray *available_categories;

  /* Async fetch */
  GCancellable *fetch_cancellable;
};

G_DEFINE_TYPE(GnostrClassifiedsView, gnostr_classifieds_view, GTK_TYPE_WIDGET)

enum {
  SIGNAL_LISTING_CLICKED,
  SIGNAL_FILTER_CHANGED,
  SIGNAL_CONTACT_SELLER,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_CATEGORY_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void refresh_grid(GnostrClassifiedsView *self);
static void apply_filters(GnostrClassifiedsView *self);
static void apply_sort(GnostrClassifiedsView *self);
static void update_empty_state(GnostrClassifiedsView *self);

/* ============== Disposal ============== */

static void
gnostr_classifieds_view_dispose(GObject *obj)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(obj);

  if (self->fetch_cancellable) {
    g_cancellable_cancel(self->fetch_cancellable);
    g_clear_object(&self->fetch_cancellable);
  }

  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  if (child)
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(gnostr_classifieds_view_parent_class)->dispose(obj);
}

static void
gnostr_classifieds_view_finalize(GObject *obj)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(obj);

  g_clear_pointer(&self->category_filter, g_free);
  g_clear_pointer(&self->location_filter, g_free);
  g_clear_pointer(&self->search_text, g_free);
  g_clear_pointer(&self->price_currency, g_free);
  g_clear_pointer(&self->user_pubkey, g_free);

  g_hash_table_destroy(self->listings);
  g_ptr_array_unref(self->listing_data);
  g_ptr_array_unref(self->visible_cards);
  g_ptr_array_unref(self->available_categories);

  G_OBJECT_CLASS(gnostr_classifieds_view_parent_class)->finalize(obj);
}

/* ============== Card Signal Handlers ============== */

static void
on_card_view_details(GnostrClassifiedCard *card, const char *event_id,
                     const char *naddr, gpointer user_data)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  (void)card;
  g_signal_emit(self, signals[SIGNAL_LISTING_CLICKED], 0, event_id, naddr);
}

static void
on_card_contact_seller(GnostrClassifiedCard *card, const char *pubkey,
                       const char *lud16, gpointer user_data)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  (void)card;
  g_signal_emit(self, signals[SIGNAL_CONTACT_SELLER], 0, pubkey, lud16);
}

static void
on_card_open_profile(GnostrClassifiedCard *card, const char *pubkey,
                     gpointer user_data)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  (void)card;
  g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
on_card_category_clicked(GnostrClassifiedCard *card, const char *category,
                         gpointer user_data)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  (void)card;
  /* Update the category filter */
  gnostr_classifieds_view_set_category_filter(self, category);
  g_signal_emit(self, signals[SIGNAL_CATEGORY_CLICKED], 0, category);
}

/* ============== Filter Bar Handlers ============== */

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  gnostr_classifieds_view_set_search_text(self, text);
}

static void
on_category_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  guint selected = gtk_drop_down_get_selected(dropdown);

  if (selected == 0) {
    /* "All Categories" selected */
    gnostr_classifieds_view_set_category_filter(self, NULL);
  } else if (selected - 1 < self->available_categories->len) {
    const gchar *cat = g_ptr_array_index(self->available_categories, selected - 1);
    gnostr_classifieds_view_set_category_filter(self, cat);
  }
}

static void
on_location_changed(GtkEntry *entry, gpointer user_data)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  gnostr_classifieds_view_set_location_filter(self, (text && *text) ? text : NULL);
}

static void
on_price_changed(GtkEntry *entry, gpointer user_data)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  (void)entry;

  const char *min_text = gtk_editable_get_text(GTK_EDITABLE(self->price_min_entry));
  const char *max_text = gtk_editable_get_text(GTK_EDITABLE(self->price_max_entry));

  gdouble min_val = (min_text && *min_text) ? g_strtod(min_text, NULL) : -1;
  gdouble max_val = (max_text && *max_text) ? g_strtod(max_text, NULL) : -1;

  /* Get currency from dropdown */
  guint curr_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->currency_dropdown));
  const char *currencies[] = { "USD", "EUR", "GBP", "BTC", "sats" };
  const char *currency = (curr_idx < G_N_ELEMENTS(currencies)) ? currencies[curr_idx] : "USD";

  gnostr_classifieds_view_set_price_range(self, min_val, max_val, currency);
}

static void
on_sort_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  guint selected = gtk_drop_down_get_selected(dropdown);
  gnostr_classifieds_view_set_sort_order(self, (GnostrClassifiedsSortOrder)selected);
}

static void
on_clear_filters_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  gnostr_classifieds_view_clear_filters(self);
}

/* ============== Widget Construction ============== */

static GtkWidget *
create_filter_bar(GnostrClassifiedsView *self)
{
  self->filter_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->filter_bar, "toolbar");
  gtk_widget_set_margin_start(self->filter_bar, 12);
  gtk_widget_set_margin_end(self->filter_bar, 12);
  gtk_widget_set_margin_top(self->filter_bar, 8);
  gtk_widget_set_margin_bottom(self->filter_bar, 8);

  /* Search entry */
  self->search_entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->search_entry, TRUE);
  gtk_widget_set_size_request(self->search_entry, 200, -1);
  gtk_editable_set_placeholder_text(GTK_EDITABLE(self->search_entry),
    _("Search listings..."));
  g_signal_connect(self->search_entry, "search-changed",
    G_CALLBACK(on_search_changed), self);
  gtk_box_append(GTK_BOX(self->filter_bar), self->search_entry);

  /* Category dropdown */
  const char *default_categories[] = { _("All Categories"), NULL };
  self->category_dropdown = gtk_drop_down_new_from_strings(default_categories);
  gtk_widget_set_tooltip_text(self->category_dropdown, _("Filter by category"));
  g_signal_connect(self->category_dropdown, "notify::selected",
    G_CALLBACK(on_category_changed), self);
  gtk_box_append(GTK_BOX(self->filter_bar), self->category_dropdown);

  /* Location entry */
  self->location_entry = gtk_entry_new();
  gtk_widget_set_size_request(self->location_entry, 150, -1);
  gtk_editable_set_placeholder_text(GTK_EDITABLE(self->location_entry),
    _("Location..."));
  g_signal_connect(self->location_entry, "changed",
    G_CALLBACK(on_location_changed), self);
  gtk_box_append(GTK_BOX(self->filter_bar), self->location_entry);

  /* Price range */
  GtkWidget *price_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  self->price_min_entry = gtk_entry_new();
  gtk_widget_set_size_request(self->price_min_entry, 80, -1);
  gtk_editable_set_placeholder_text(GTK_EDITABLE(self->price_min_entry), _("Min"));
  g_signal_connect(self->price_min_entry, "changed",
    G_CALLBACK(on_price_changed), self);
  gtk_box_append(GTK_BOX(price_box), self->price_min_entry);

  GtkWidget *dash = gtk_label_new("-");
  gtk_box_append(GTK_BOX(price_box), dash);

  self->price_max_entry = gtk_entry_new();
  gtk_widget_set_size_request(self->price_max_entry, 80, -1);
  gtk_editable_set_placeholder_text(GTK_EDITABLE(self->price_max_entry), _("Max"));
  g_signal_connect(self->price_max_entry, "changed",
    G_CALLBACK(on_price_changed), self);
  gtk_box_append(GTK_BOX(price_box), self->price_max_entry);

  const char *currencies[] = { "USD", "EUR", "GBP", "BTC", "sats", NULL };
  self->currency_dropdown = gtk_drop_down_new_from_strings(currencies);
  g_signal_connect(self->currency_dropdown, "notify::selected",
    G_CALLBACK(on_price_changed), self);
  gtk_box_append(GTK_BOX(price_box), self->currency_dropdown);

  gtk_box_append(GTK_BOX(self->filter_bar), price_box);

  /* Sort dropdown */
  const char *sort_options[] = {
    _("Newest first"),
    _("Oldest first"),
    _("Price: Low to High"),
    _("Price: High to Low"),
    NULL
  };
  self->sort_dropdown = gtk_drop_down_new_from_strings(sort_options);
  gtk_widget_set_tooltip_text(self->sort_dropdown, _("Sort by"));
  g_signal_connect(self->sort_dropdown, "notify::selected",
    G_CALLBACK(on_sort_changed), self);
  gtk_box_append(GTK_BOX(self->filter_bar), self->sort_dropdown);

  /* Clear filters button */
  self->btn_clear_filters = gtk_button_new_from_icon_name("edit-clear-symbolic");
  gtk_widget_set_tooltip_text(self->btn_clear_filters, _("Clear all filters"));
  g_signal_connect(self->btn_clear_filters, "clicked",
    G_CALLBACK(on_clear_filters_clicked), self);
  gtk_box_append(GTK_BOX(self->filter_bar), self->btn_clear_filters);

  return self->filter_bar;
}

static GtkWidget *
create_empty_state(GnostrClassifiedsView *self)
{
  self->empty_state = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_halign(self->empty_state, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->empty_state, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(self->empty_state, 48);
  gtk_widget_set_margin_bottom(self->empty_state, 48);

  GtkWidget *icon = gtk_image_new_from_icon_name("view-grid-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
  gtk_widget_add_css_class(icon, "dim-label");
  gtk_box_append(GTK_BOX(self->empty_state), icon);

  GtkWidget *title = gtk_label_new(_("No Listings Found"));
  gtk_widget_add_css_class(title, "title-2");
  gtk_box_append(GTK_BOX(self->empty_state), title);

  GtkWidget *subtitle = gtk_label_new(_("Try adjusting your filters or check back later."));
  gtk_widget_add_css_class(subtitle, "dim-label");
  gtk_box_append(GTK_BOX(self->empty_state), subtitle);

  return self->empty_state;
}

static GtkWidget *
create_loading_state(GnostrClassifiedsView *self)
{
  GtkWidget *loading_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_halign(loading_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(loading_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(loading_box, 48);

  self->loading_spinner = gtk_spinner_new();
  gtk_spinner_set_spinning(GTK_SPINNER(self->loading_spinner), TRUE);
  gtk_widget_set_size_request(self->loading_spinner, 48, 48);
  gtk_box_append(GTK_BOX(loading_box), self->loading_spinner);

  GtkWidget *label = gtk_label_new(_("Loading listings..."));
  gtk_widget_add_css_class(label, "dim-label");
  gtk_box_append(GTK_BOX(loading_box), label);

  return loading_box;
}

/* ============== Class Init ============== */

static void
gnostr_classifieds_view_class_init(GnostrClassifiedsViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_classifieds_view_dispose;
  object_class->finalize = gnostr_classifieds_view_finalize;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "classifieds-view");

  /* Signals */
  signals[SIGNAL_LISTING_CLICKED] = g_signal_new("listing-clicked",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_FILTER_CHANGED] = g_signal_new("filter-changed",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_STRING);

  signals[SIGNAL_CONTACT_SELLER] = g_signal_new("contact-seller",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_CATEGORY_CLICKED] = g_signal_new("category-clicked",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnostr_classifieds_view_init(GnostrClassifiedsView *self)
{
  /* Initialize state */
  self->price_min = -1;
  self->price_max = -1;
  self->price_currency = g_strdup("USD");
  self->sort_order = GNOSTR_CLASSIFIEDS_SORT_NEWEST;
  self->columns = 0; /* Auto */

  /* Initialize storage */
  self->listings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->listing_data = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_classified_free);
  self->visible_cards = g_ptr_array_new();
  self->available_categories = g_ptr_array_new_with_free_func(g_free);

  /* Build main layout */
  self->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(self->root_box, GTK_WIDGET(self));

  /* Filter bar */
  GtkWidget *filter_bar = create_filter_bar(self);
  gtk_box_append(GTK_BOX(self->root_box), filter_bar);

  /* Separator */
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(self->root_box), sep);

  /* Content stack */
  self->content_stack = gtk_stack_new();
  gtk_widget_set_vexpand(self->content_stack, TRUE);
  gtk_box_append(GTK_BOX(self->root_box), self->content_stack);

  /* Scrolled window with grid */
  self->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled_window),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  /* FlowBox for responsive grid */
  self->grid_box = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->grid_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(self->grid_box), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->grid_box), 1);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->grid_box), 4);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->grid_box), 16);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->grid_box), 16);
  gtk_widget_set_margin_start(self->grid_box, 16);
  gtk_widget_set_margin_end(self->grid_box, 16);
  gtk_widget_set_margin_top(self->grid_box, 16);
  gtk_widget_set_margin_bottom(self->grid_box, 16);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window),
    self->grid_box);
  gtk_stack_add_named(GTK_STACK(self->content_stack), self->scrolled_window, "grid");

  /* Empty state */
  GtkWidget *empty = create_empty_state(self);
  gtk_stack_add_named(GTK_STACK(self->content_stack), empty, "empty");

  /* Loading state */
  GtkWidget *loading = create_loading_state(self);
  gtk_stack_add_named(GTK_STACK(self->content_stack), loading, "loading");

  /* Start with empty state */
  gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "empty");
}

/* ============== Public API ============== */

GnostrClassifiedsView *
gnostr_classifieds_view_new(void)
{
  return g_object_new(GNOSTR_TYPE_CLASSIFIEDS_VIEW, NULL);
}

void
gnostr_classifieds_view_add_listing(GnostrClassifiedsView *self,
                                     const GnostrClassified *classified)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));
  g_return_if_fail(classified != NULL);
  g_return_if_fail(classified->event_id != NULL);

  /* Skip if already exists */
  if (g_hash_table_contains(self->listings, classified->event_id)) {
    return;
  }

  /* Store a copy of the data */
  GnostrClassified *copy = gnostr_classified_new();
  copy->event_id = g_strdup(classified->event_id);
  copy->d_tag = g_strdup(classified->d_tag);
  copy->pubkey = g_strdup(classified->pubkey);
  copy->title = g_strdup(classified->title);
  copy->summary = g_strdup(classified->summary);
  copy->description = g_strdup(classified->description);
  copy->location = g_strdup(classified->location);
  copy->published_at = classified->published_at;
  copy->created_at = classified->created_at;
  copy->seller_name = g_strdup(classified->seller_name);
  copy->seller_avatar = g_strdup(classified->seller_avatar);
  copy->seller_nip05 = g_strdup(classified->seller_nip05);
  copy->seller_lud16 = g_strdup(classified->seller_lud16);

  if (classified->price) {
    copy->price = gnostr_classified_price_parse(
      classified->price->amount, classified->price->currency);
  }

  if (classified->categories) {
    for (guint i = 0; i < classified->categories->len; i++) {
      g_ptr_array_add(copy->categories,
        g_strdup(g_ptr_array_index(classified->categories, i)));
    }
  }

  if (classified->images) {
    for (guint i = 0; i < classified->images->len; i++) {
      g_ptr_array_add(copy->images,
        g_strdup(g_ptr_array_index(classified->images, i)));
    }
  }

  g_ptr_array_add(self->listing_data, copy);

  /* Create card widget */
  GnostrClassifiedCard *card = gnostr_classified_card_new();
  gnostr_classified_card_set_listing(card, copy);
  gnostr_classified_card_set_logged_in(card, self->is_logged_in);
  gnostr_classified_card_set_compact(card, TRUE);

  /* Connect signals */
  g_signal_connect(card, "view-details", G_CALLBACK(on_card_view_details), self);
  g_signal_connect(card, "contact-seller", G_CALLBACK(on_card_contact_seller), self);
  g_signal_connect(card, "open-profile", G_CALLBACK(on_card_open_profile), self);
  g_signal_connect(card, "category-clicked", G_CALLBACK(on_card_category_clicked), self);

  /* Add to hash and flowbox */
  g_hash_table_insert(self->listings, g_strdup(classified->event_id), card);
  gtk_flow_box_append(GTK_FLOW_BOX(self->grid_box), GTK_WIDGET(card));
  g_ptr_array_add(self->visible_cards, card);

  /* Add categories to available list */
  if (classified->categories) {
    for (guint i = 0; i < classified->categories->len; i++) {
      gnostr_classifieds_view_add_category(self,
        g_ptr_array_index(classified->categories, i));
    }
  }

  update_empty_state(self);
}

void
gnostr_classifieds_view_add_listings(GnostrClassifiedsView *self,
                                      GPtrArray *classifieds)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  if (!classifieds) return;

  for (guint i = 0; i < classifieds->len; i++) {
    GnostrClassified *c = g_ptr_array_index(classifieds, i);
    gnostr_classifieds_view_add_listing(self, c);
  }

  apply_sort(self);
}

void
gnostr_classifieds_view_remove_listing(GnostrClassifiedsView *self,
                                        const char *event_id)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));
  g_return_if_fail(event_id != NULL);

  GnostrClassifiedCard *card = g_hash_table_lookup(self->listings, event_id);
  if (card) {
    g_ptr_array_remove(self->visible_cards, card);

    /* Find parent flowbox child */
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(card));
    if (parent) {
      gtk_flow_box_remove(GTK_FLOW_BOX(self->grid_box), parent);
    }

    g_hash_table_remove(self->listings, event_id);
  }

  /* Remove from data array */
  for (guint i = 0; i < self->listing_data->len; i++) {
    GnostrClassified *c = g_ptr_array_index(self->listing_data, i);
    if (g_strcmp0(c->event_id, event_id) == 0) {
      g_ptr_array_remove_index(self->listing_data, i);
      break;
    }
  }

  update_empty_state(self);
}

void
gnostr_classifieds_view_clear(GnostrClassifiedsView *self)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  /* Clear flowbox */
  GtkWidget *child = gtk_widget_get_first_child(self->grid_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_flow_box_remove(GTK_FLOW_BOX(self->grid_box), child);
    child = next;
  }

  g_hash_table_remove_all(self->listings);
  g_ptr_array_set_size(self->listing_data, 0);
  g_ptr_array_set_size(self->visible_cards, 0);

  update_empty_state(self);
}

guint
gnostr_classifieds_view_get_listing_count(GnostrClassifiedsView *self)
{
  g_return_val_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self), 0);
  return self->visible_cards->len;
}

/* ============== Filtering ============== */

void
gnostr_classifieds_view_set_category_filter(GnostrClassifiedsView *self,
                                             const char *category)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  g_clear_pointer(&self->category_filter, g_free);
  self->category_filter = g_strdup(category);

  apply_filters(self);

  g_signal_emit(self, signals[SIGNAL_FILTER_CHANGED], 0,
    self->category_filter, self->location_filter,
    self->price_min, self->price_max, self->price_currency);
}

void
gnostr_classifieds_view_set_location_filter(GnostrClassifiedsView *self,
                                             const char *location)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  g_clear_pointer(&self->location_filter, g_free);
  self->location_filter = g_strdup(location);

  apply_filters(self);

  g_signal_emit(self, signals[SIGNAL_FILTER_CHANGED], 0,
    self->category_filter, self->location_filter,
    self->price_min, self->price_max, self->price_currency);
}

void
gnostr_classifieds_view_set_price_range(GnostrClassifiedsView *self,
                                         gdouble min_price,
                                         gdouble max_price,
                                         const char *currency)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  self->price_min = min_price;
  self->price_max = max_price;
  g_clear_pointer(&self->price_currency, g_free);
  self->price_currency = g_strdup(currency ? currency : "USD");

  apply_filters(self);

  g_signal_emit(self, signals[SIGNAL_FILTER_CHANGED], 0,
    self->category_filter, self->location_filter,
    self->price_min, self->price_max, self->price_currency);
}

void
gnostr_classifieds_view_clear_filters(GnostrClassifiedsView *self)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  g_clear_pointer(&self->category_filter, g_free);
  g_clear_pointer(&self->location_filter, g_free);
  g_clear_pointer(&self->search_text, g_free);
  self->price_min = -1;
  self->price_max = -1;

  /* Reset UI */
  gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
  gtk_drop_down_set_selected(GTK_DROP_DOWN(self->category_dropdown), 0);
  gtk_editable_set_text(GTK_EDITABLE(self->location_entry), "");
  gtk_editable_set_text(GTK_EDITABLE(self->price_min_entry), "");
  gtk_editable_set_text(GTK_EDITABLE(self->price_max_entry), "");

  refresh_grid(self);
}

const char *
gnostr_classifieds_view_get_category_filter(GnostrClassifiedsView *self)
{
  g_return_val_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self), NULL);
  return self->category_filter;
}

const char *
gnostr_classifieds_view_get_location_filter(GnostrClassifiedsView *self)
{
  g_return_val_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self), NULL);
  return self->location_filter;
}

/* ============== Sorting ============== */

void
gnostr_classifieds_view_set_sort_order(GnostrClassifiedsView *self,
                                        GnostrClassifiedsSortOrder order)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  self->sort_order = order;
  apply_sort(self);
}

GnostrClassifiedsSortOrder
gnostr_classifieds_view_get_sort_order(GnostrClassifiedsView *self)
{
  g_return_val_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self), GNOSTR_CLASSIFIEDS_SORT_NEWEST);
  return self->sort_order;
}

/* ============== View State ============== */

void
gnostr_classifieds_view_set_loading(GnostrClassifiedsView *self, gboolean is_loading)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  self->is_loading = is_loading;

  if (is_loading) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "loading");
  } else {
    update_empty_state(self);
  }
}

gboolean
gnostr_classifieds_view_is_loading(GnostrClassifiedsView *self)
{
  g_return_val_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self), FALSE);
  return self->is_loading;
}

void
gnostr_classifieds_view_set_logged_in(GnostrClassifiedsView *self, gboolean logged_in)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  self->is_logged_in = logged_in;

  /* Update all cards */
  for (guint i = 0; i < self->visible_cards->len; i++) {
    GnostrClassifiedCard *card = g_ptr_array_index(self->visible_cards, i);
    gnostr_classified_card_set_logged_in(card, logged_in);
  }
}

void
gnostr_classifieds_view_set_user_pubkey(GnostrClassifiedsView *self,
                                         const char *pubkey_hex)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  g_clear_pointer(&self->user_pubkey, g_free);
  self->user_pubkey = g_strdup(pubkey_hex);
}

void
gnostr_classifieds_view_show_filter_bar(GnostrClassifiedsView *self, gboolean show)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));
  gtk_widget_set_visible(self->filter_bar, show);
}

void
gnostr_classifieds_view_set_columns(GnostrClassifiedsView *self, guint columns)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  self->columns = columns;

  if (columns > 0) {
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->grid_box), columns);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->grid_box), columns);
  } else {
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->grid_box), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->grid_box), 4);
  }
}

/* ============== Categories ============== */

void
gnostr_classifieds_view_set_available_categories(GnostrClassifiedsView *self,
                                                  GPtrArray *categories)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  g_ptr_array_set_size(self->available_categories, 0);

  if (categories) {
    for (guint i = 0; i < categories->len; i++) {
      const gchar *cat = g_ptr_array_index(categories, i);
      if (cat && *cat) {
        g_ptr_array_add(self->available_categories, g_strdup(cat));
      }
    }
  }

  /* Rebuild dropdown */
  GPtrArray *items = g_ptr_array_new();
  g_ptr_array_add(items, g_strdup(_("All Categories")));
  for (guint i = 0; i < self->available_categories->len; i++) {
    g_ptr_array_add(items, g_strdup(g_ptr_array_index(self->available_categories, i)));
  }
  g_ptr_array_add(items, NULL);

  GtkStringList *model = gtk_string_list_new((const char * const *)items->pdata);
  gtk_drop_down_set_model(GTK_DROP_DOWN(self->category_dropdown), G_LIST_MODEL(model));
  g_object_unref(model);

  g_ptr_array_unref(items);
}

void
gnostr_classifieds_view_add_category(GnostrClassifiedsView *self, const char *category)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  if (!category || !*category) return;

  /* Check if already exists */
  for (guint i = 0; i < self->available_categories->len; i++) {
    if (g_strcmp0(g_ptr_array_index(self->available_categories, i), category) == 0) {
      return;
    }
  }

  g_ptr_array_add(self->available_categories, g_strdup(category));

  /* Update dropdown */
  GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(self->category_dropdown));
  if (GTK_IS_STRING_LIST(model)) {
    gtk_string_list_append(GTK_STRING_LIST(model), category);
  }
}

/* ============== Search ============== */

void
gnostr_classifieds_view_set_search_text(GnostrClassifiedsView *self, const char *text)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  g_clear_pointer(&self->search_text, g_free);
  self->search_text = g_strdup(text);

  apply_filters(self);
}

const char *
gnostr_classifieds_view_get_search_text(GnostrClassifiedsView *self)
{
  g_return_val_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self), NULL);
  return self->search_text;
}

/* ============== Async Fetch ============== */

static void
on_listings_fetched(GPtrArray *classifieds, gpointer user_data)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);

  if (!GNOSTR_IS_CLASSIFIEDS_VIEW(self)) {
    if (classifieds) g_ptr_array_unref(classifieds);
    return;
  }

  gnostr_classifieds_view_set_loading(self, FALSE);

  if (classifieds && classifieds->len > 0) {
    gnostr_classifieds_view_add_listings(self, classifieds);
  }

  if (classifieds) g_ptr_array_unref(classifieds);
}

void
gnostr_classifieds_view_fetch_listings(GnostrClassifiedsView *self)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  gnostr_classifieds_view_cancel_fetch(self);
  gnostr_classifieds_view_clear(self);
  gnostr_classifieds_view_set_loading(self, TRUE);

  self->fetch_cancellable = g_cancellable_new();

  gnostr_fetch_classifieds_async(
    self->category_filter,
    self->location_filter,
    50,
    self->fetch_cancellable,
    on_listings_fetched,
    self
  );
}

void
gnostr_classifieds_view_cancel_fetch(GnostrClassifiedsView *self)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIEDS_VIEW(self));

  if (self->fetch_cancellable) {
    g_cancellable_cancel(self->fetch_cancellable);
    g_clear_object(&self->fetch_cancellable);
  }
}

/* ============== Private Helpers ============== */

static gboolean
listing_matches_filters(GnostrClassifiedsView *self, GnostrClassified *classified)
{
  /* Search text filter */
  if (self->search_text && *self->search_text) {
    gchar *search_lower = g_utf8_strdown(self->search_text, -1);
    gboolean found = FALSE;

    if (classified->title) {
      gchar *title_lower = g_utf8_strdown(classified->title, -1);
      if (strstr(title_lower, search_lower)) found = TRUE;
      g_free(title_lower);
    }

    if (!found && classified->summary) {
      gchar *summary_lower = g_utf8_strdown(classified->summary, -1);
      if (strstr(summary_lower, search_lower)) found = TRUE;
      g_free(summary_lower);
    }

    if (!found && classified->description) {
      gchar *desc_lower = g_utf8_strdown(classified->description, -1);
      if (strstr(desc_lower, search_lower)) found = TRUE;
      g_free(desc_lower);
    }

    g_free(search_lower);
    if (!found) return FALSE;
  }

  /* Category filter */
  if (self->category_filter && *self->category_filter) {
    gboolean found = FALSE;
    if (classified->categories) {
      for (guint i = 0; i < classified->categories->len; i++) {
        const gchar *cat = g_ptr_array_index(classified->categories, i);
        if (g_ascii_strcasecmp(cat, self->category_filter) == 0) {
          found = TRUE;
          break;
        }
      }
    }
    if (!found) return FALSE;
  }

  /* Location filter */
  if (self->location_filter && *self->location_filter && classified->location) {
    gchar *loc_lower = g_utf8_strdown(classified->location, -1);
    gchar *filter_lower = g_utf8_strdown(self->location_filter, -1);
    gboolean match = (strstr(loc_lower, filter_lower) != NULL);
    g_free(loc_lower);
    g_free(filter_lower);
    if (!match) return FALSE;
  }

  /* Price range filter - simplified, assumes same currency */
  if (classified->price && classified->price->amount) {
    gdouble price = g_strtod(classified->price->amount, NULL);
    if (self->price_min >= 0 && price < self->price_min) return FALSE;
    if (self->price_max >= 0 && price > self->price_max) return FALSE;
  }

  return TRUE;
}

static void
apply_filters(GnostrClassifiedsView *self)
{
  refresh_grid(self);
}

static gint
compare_listings(gconstpointer a, gconstpointer b, gpointer user_data)
{
  GnostrClassifiedsView *self = GNOSTR_CLASSIFIEDS_VIEW(user_data);
  GnostrClassified *ca = *(GnostrClassified **)a;
  GnostrClassified *cb = *(GnostrClassified **)b;

  switch (self->sort_order) {
    case GNOSTR_CLASSIFIEDS_SORT_NEWEST:
      return (cb->published_at > ca->published_at) ? 1 :
             (cb->published_at < ca->published_at) ? -1 : 0;

    case GNOSTR_CLASSIFIEDS_SORT_OLDEST:
      return (ca->published_at > cb->published_at) ? 1 :
             (ca->published_at < cb->published_at) ? -1 : 0;

    case GNOSTR_CLASSIFIEDS_SORT_PRICE_LOW: {
      gdouble pa = ca->price ? g_strtod(ca->price->amount, NULL) : 0;
      gdouble pb = cb->price ? g_strtod(cb->price->amount, NULL) : 0;
      return (pa > pb) ? 1 : (pa < pb) ? -1 : 0;
    }

    case GNOSTR_CLASSIFIEDS_SORT_PRICE_HIGH: {
      gdouble pa = ca->price ? g_strtod(ca->price->amount, NULL) : 0;
      gdouble pb = cb->price ? g_strtod(cb->price->amount, NULL) : 0;
      return (pb > pa) ? 1 : (pb < pa) ? -1 : 0;
    }

    default:
      return 0;
  }
}

static void
apply_sort(GnostrClassifiedsView *self)
{
  if (self->listing_data->len == 0) return;

  g_ptr_array_sort_with_data(self->listing_data, compare_listings, self);
  refresh_grid(self);
}

static void
refresh_grid(GnostrClassifiedsView *self)
{
  /* Clear current grid */
  GtkWidget *child = gtk_widget_get_first_child(self->grid_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_flow_box_remove(GTK_FLOW_BOX(self->grid_box), child);
    child = next;
  }

  g_hash_table_remove_all(self->listings);
  g_ptr_array_set_size(self->visible_cards, 0);

  /* Re-add filtered and sorted listings */
  for (guint i = 0; i < self->listing_data->len; i++) {
    GnostrClassified *classified = g_ptr_array_index(self->listing_data, i);

    if (!listing_matches_filters(self, classified)) {
      continue;
    }

    GnostrClassifiedCard *card = gnostr_classified_card_new();
    gnostr_classified_card_set_listing(card, classified);
    gnostr_classified_card_set_logged_in(card, self->is_logged_in);
    gnostr_classified_card_set_compact(card, TRUE);

    g_signal_connect(card, "view-details", G_CALLBACK(on_card_view_details), self);
    g_signal_connect(card, "contact-seller", G_CALLBACK(on_card_contact_seller), self);
    g_signal_connect(card, "open-profile", G_CALLBACK(on_card_open_profile), self);
    g_signal_connect(card, "category-clicked", G_CALLBACK(on_card_category_clicked), self);

    g_hash_table_insert(self->listings, g_strdup(classified->event_id), card);
    gtk_flow_box_append(GTK_FLOW_BOX(self->grid_box), GTK_WIDGET(card));
    g_ptr_array_add(self->visible_cards, card);
  }

  update_empty_state(self);
}

static void
update_empty_state(GnostrClassifiedsView *self)
{
  if (self->is_loading) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "loading");
  } else if (self->visible_cards->len == 0) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "empty");
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "grid");
  }
}
