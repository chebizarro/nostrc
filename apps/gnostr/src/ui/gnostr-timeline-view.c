#include "gnostr-timeline-view.h"
#include <string.h>
#include <time.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-timeline-view.ui"

/* Item representing a post row, optionally with children for threading. */
typedef struct _TimelineItem {
  GObject parent_instance;
  gchar *display_name;
  gchar *handle;
  gchar *timestamp;
  gchar *content;
  guint depth;
  /* metadata for threading */
  gchar *id;
  gchar *root_id;
  gchar *pubkey;
  gint64 created_at;
  /* children list when acting as a parent in a thread */
  GListStore *children; /* element-type: TimelineItem */
} TimelineItem;

typedef struct _TimelineItemClass {
  GObjectClass parent_class;
} TimelineItemClass;

G_DEFINE_TYPE(TimelineItem, timeline_item, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_DISPLAY_NAME,
  PROP_HANDLE,
  PROP_TIMESTAMP,
  PROP_CONTENT,
  PROP_DEPTH,
  PROP_ID,
  PROP_ROOT_ID,
  PROP_PUBKEY,
  PROP_CREATED_AT,
  N_PROPS
};

static GParamSpec *ti_props[N_PROPS];

static void timeline_item_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
  TimelineItem *self = (TimelineItem*)obj;
  switch (prop_id) {
    case PROP_DISPLAY_NAME: g_free(self->display_name); self->display_name = g_value_dup_string(value); break;
    case PROP_HANDLE:       g_free(self->handle);       self->handle       = g_value_dup_string(value); break;
    case PROP_TIMESTAMP:    g_free(self->timestamp);    self->timestamp    = g_value_dup_string(value); break;
    case PROP_CONTENT:      g_free(self->content);      self->content      = g_value_dup_string(value); break;
    case PROP_DEPTH:        self->depth = g_value_get_uint(value); break;
    case PROP_ID:           g_free(self->id);           self->id           = g_value_dup_string(value); break;
    case PROP_ROOT_ID:      g_free(self->root_id);      self->root_id      = g_value_dup_string(value); break;
    case PROP_PUBKEY:       g_free(self->pubkey);       self->pubkey       = g_value_dup_string(value); break;
    case PROP_CREATED_AT:   self->created_at            = g_value_get_int64(value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void timeline_item_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
  TimelineItem *self = (TimelineItem*)obj;
  switch (prop_id) {
    case PROP_DISPLAY_NAME: g_value_set_string(value, self->display_name); break;
    case PROP_HANDLE:       g_value_set_string(value, self->handle); break;
    case PROP_TIMESTAMP:    g_value_set_string(value, self->timestamp); break;
    case PROP_CONTENT:      g_value_set_string(value, self->content); break;
    case PROP_DEPTH:        g_value_set_uint(value, self->depth); break;
    case PROP_ID:           g_value_set_string(value, self->id); break;
    case PROP_ROOT_ID:      g_value_set_string(value, self->root_id); break;
    case PROP_PUBKEY:       g_value_set_string(value, self->pubkey); break;
    case PROP_CREATED_AT:   g_value_set_int64 (value, self->created_at); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void timeline_item_dispose(GObject *obj) {
  TimelineItem *self = (TimelineItem*)obj;
  g_clear_pointer(&self->display_name, g_free);
  g_clear_pointer(&self->handle, g_free);
  g_clear_pointer(&self->timestamp, g_free);
  g_clear_pointer(&self->content, g_free);
  g_clear_pointer(&self->id, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->pubkey, g_free);
  if (self->children) g_clear_object(&self->children);
  G_OBJECT_CLASS(timeline_item_parent_class)->dispose(obj);
}

static void timeline_item_class_init(TimelineItemClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->set_property = timeline_item_set_property;
  oc->get_property = timeline_item_get_property;
  oc->dispose = timeline_item_dispose;
  ti_props[PROP_DISPLAY_NAME] = g_param_spec_string("display-name", "display-name", "Display Name", NULL, G_PARAM_READWRITE);
  ti_props[PROP_HANDLE]       = g_param_spec_string("handle",       "handle",       "Handle",       NULL, G_PARAM_READWRITE);
  ti_props[PROP_TIMESTAMP]    = g_param_spec_string("timestamp",    "timestamp",    "Timestamp",    NULL, G_PARAM_READWRITE);
  ti_props[PROP_CONTENT]      = g_param_spec_string("content",      "content",      "Content",      NULL, G_PARAM_READWRITE);
  ti_props[PROP_DEPTH]        = g_param_spec_uint   ("depth",        "depth",        "Depth",        0, 32, 0, G_PARAM_READWRITE);
  ti_props[PROP_ID]           = g_param_spec_string ("id",           "id",           "Event Id",     NULL, G_PARAM_READWRITE);
  ti_props[PROP_ROOT_ID]      = g_param_spec_string ("root-id",      "root-id",      "Root Event Id",NULL, G_PARAM_READWRITE);
  ti_props[PROP_PUBKEY]       = g_param_spec_string ("pubkey",       "pubkey",       "Pubkey",       NULL, G_PARAM_READWRITE);
  ti_props[PROP_CREATED_AT]   = g_param_spec_int64  ("created-at",   "created-at",   "Created At",   0, G_MAXINT64, 0, G_PARAM_READWRITE);
  g_object_class_install_properties(oc, N_PROPS, ti_props);
}

static void timeline_item_init(TimelineItem *self) { (void)self; }

static TimelineItem *timeline_item_new(const char *display, const char *handle, const char *ts, const char *content, guint depth) {
  TimelineItem *it = g_object_new(timeline_item_get_type(),
                                  "display-name", display ? display : "Anonymous",
                                  "handle",       handle  ? handle  : "@anon",
                                  "timestamp",    ts      ? ts      : "now",
                                  "content",      content ? content : "",
                                  "depth",        depth,
                                  NULL);
  it->children = g_list_store_new(timeline_item_get_type());
  return it;
}

static void timeline_item_set_meta(TimelineItem *it, const char *id, const char *pubkey, gint64 created_at) {
  if (!it) return;
  g_object_set(it,
               "id", id,
               "pubkey", pubkey,
               "created-at", created_at,
               NULL);
}

static GListModel *timeline_item_get_children_model(TimelineItem *it) {
  return it ? G_LIST_MODEL(it->children) : NULL;
}

static void timeline_item_add_child(TimelineItem *parent, TimelineItem *child) {
  if (!parent || !child) return;
  g_list_store_append(parent->children, child);
}

/* Public wrappers for building trees from outside */
void gnostr_timeline_item_add_child(TimelineItem *parent, TimelineItem *child) {
  timeline_item_add_child(parent, child);
}

GListModel *gnostr_timeline_item_get_children(TimelineItem *item) {
  return timeline_item_get_children_model(item);
}

struct _GnostrTimelineView {
  GtkWidget parent_instance;
  GtkWidget *root_scroller;
  GtkWidget *list_view;
  GtkSelectionModel *selection_model; /* owned */
  GListStore *list_model;             /* owned: TimelineItem (flat) or tree roots when tree is active */
  GtkTreeListModel *tree_model;       /* owned when tree roots set */
};

G_DEFINE_TYPE(GnostrTimelineView, gnostr_timeline_view, GTK_TYPE_WIDGET)

static void gnostr_timeline_view_dispose(GObject *obj) {
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(obj);
  g_debug("timeline_view dispose: list_view=%p list_model=%p", (void*)self->list_view, (void*)self->list_model);
  if (self->list_view && GTK_IS_LIST_VIEW(self->list_view))
    gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), NULL);
  if (self->selection_model) {
    if (G_IS_OBJECT(self->selection_model)) g_object_unref(self->selection_model);
    self->selection_model = NULL;
  }
  if (self->list_model) {
    if (G_IS_OBJECT(self->list_model)) g_object_unref(self->list_model);
    self->list_model = NULL;
  }
  if (self->tree_model) {
    if (G_IS_OBJECT(self->tree_model)) g_object_unref(self->tree_model);
    self->tree_model = NULL;
  }
  /* Dispose template children before chaining up so they are unparented first */
  gtk_widget_dispose_template(GTK_WIDGET(obj), GNOSTR_TYPE_TIMELINE_VIEW);
  self->root_scroller = NULL;
  self->list_view = NULL;
  G_OBJECT_CLASS(gnostr_timeline_view_parent_class)->dispose(obj);
}

static void gnostr_timeline_view_finalize(GObject *obj) {
  G_OBJECT_CLASS(gnostr_timeline_view_parent_class)->finalize(obj);
}

/* Setup: load row UI from resource and set as child. Cache subwidgets on the row. */
static void factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GtkBuilder *b = gtk_builder_new_from_resource("/org/gnostr/ui/ui/widgets/gnostr-timeline-row.ui");
  GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(b, "row"));
  if (!row) {
    g_warning("timeline row builder missing 'row'");
    g_object_unref(b);
    return;
  }
  /* Cache pointers to frequently-updated subwidgets on the row for fast lookup in bind */
  GtkWidget *w_display_name = GTK_WIDGET(gtk_builder_get_object(b, "display_name"));
  GtkWidget *w_handle       = GTK_WIDGET(gtk_builder_get_object(b, "handle"));
  GtkWidget *w_timestamp    = GTK_WIDGET(gtk_builder_get_object(b, "timestamp"));
  GtkWidget *w_content      = GTK_WIDGET(gtk_builder_get_object(b, "content"));
  GtkWidget *w_avatar_init  = GTK_WIDGET(gtk_builder_get_object(b, "avatar_initials"));
  GtkWidget *w_indicator    = GTK_WIDGET(gtk_builder_get_object(b, "thread_indicator"));
  g_object_set_data(G_OBJECT(row), "display_name", w_display_name);
  g_object_set_data(G_OBJECT(row), "handle", w_handle);
  g_object_set_data(G_OBJECT(row), "timestamp", w_timestamp);
  g_object_set_data(G_OBJECT(row), "content", w_content);
  g_object_set_data(G_OBJECT(row), "avatar_initials", w_avatar_init);
  g_object_set_data(G_OBJECT(row), "thread_indicator", w_indicator);
  /* ListItem takes ownership of the child */
  gtk_list_item_set_child(item, row);
  g_object_unref(b);
}

static void factory_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GObject *obj = gtk_list_item_get_item(item);
  gchar *display = NULL, *handle = NULL, *ts = NULL, *content = NULL, *root_id = NULL;
  guint depth = 0; gboolean is_reply = FALSE; gint64 created_at = 0;
  if (G_IS_OBJECT(obj) && G_TYPE_CHECK_INSTANCE_TYPE(obj, timeline_item_get_type())) {
    g_object_get(obj,
                 "display-name", &display,
                 "handle",       &handle,
                 "timestamp",    &ts,
                 "content",      &content,
                 "depth",        &depth,
                 "root-id",      &root_id,
                 "created-at",   &created_at,
                 NULL);
    is_reply = depth > 0;
  }
  GtkWidget *row = gtk_list_item_get_child(item);
  if (!GTK_IS_WIDGET(row)) return;
  GtkWidget *w_display_name = GTK_WIDGET(g_object_get_data(G_OBJECT(row), "display_name"));
  GtkWidget *w_handle       = GTK_WIDGET(g_object_get_data(G_OBJECT(row), "handle"));
  GtkWidget *w_timestamp    = GTK_WIDGET(g_object_get_data(G_OBJECT(row), "timestamp"));
  GtkWidget *w_content      = GTK_WIDGET(g_object_get_data(G_OBJECT(row), "content"));
  GtkWidget *w_avatar_init  = GTK_WIDGET(g_object_get_data(G_OBJECT(row), "avatar_initials"));
  GtkWidget *w_indicator    = GTK_WIDGET(g_object_get_data(G_OBJECT(row), "thread_indicator"));

  /* Populate */
  if (GTK_IS_LABEL(w_display_name)) gtk_label_set_text(GTK_LABEL(w_display_name), display ? display : "Anonymous");
  if (GTK_IS_LABEL(w_handle))       gtk_label_set_text(GTK_LABEL(w_handle), handle ? handle : "@anon");
  if (GTK_IS_LABEL(w_timestamp)) {
    /* Pretty relative time from created_at if available */
    if (created_at > 0) {
      time_t now = time(NULL);
      long diff = (long)(now - (time_t)created_at);
      const char *unit = "s"; long val = diff;
      if (diff < 0) diff = 0;
      if (diff >= 60) { val = diff/60; unit = "m"; }
      if (diff >= 3600) { val = diff/3600; unit = "h"; }
      if (diff >= 86400) { val = diff/86400; unit = "d"; }
      char buf[32];
      if (diff < 5) g_strlcpy(buf, "now", sizeof(buf));
      else g_snprintf(buf, sizeof(buf), "%ld%s", val, unit);
      gtk_label_set_text(GTK_LABEL(w_timestamp), buf);
    } else {
      gtk_label_set_text(GTK_LABEL(w_timestamp), ts ? ts : "now");
    }
  }
  if (GTK_IS_LABEL(w_avatar_init)) {
    /* Derive monogram initials from display or handle */
    const char *src = (display && *display) ? display : (handle && *handle ? handle : "AN");
    gunichar c1 = g_utf8_get_char_validated(src, -1);
    const char *p = g_utf8_next_char(src);
    /* find next letter start */
    gunichar c2 = 0;
    while (p && *p) {
      gunichar c = g_utf8_get_char_validated(p, -1);
      if (g_unichar_isalpha(c)) { c2 = c; break; }
      p = g_utf8_next_char(p);
    }
    char out[8] = {0};
    gchar *s1 = g_ucs4_to_utf8(&c1, 1, NULL, NULL, NULL);
    if (!s1) s1 = g_strdup("A");
    if (c2) {
      gchar *s2 = g_ucs4_to_utf8(&c2, 1, NULL, NULL, NULL);
      g_snprintf(out, sizeof(out), "%.1s%.1s", s1, s2 ? s2 : "");
      g_free(s2);
    } else {
      g_snprintf(out, sizeof(out), "%.1s", s1);
    }
    for (char *q = out; *q; ++q) *q = g_ascii_toupper(*q);
    gtk_label_set_text(GTK_LABEL(w_avatar_init), out);
    g_free(s1);
  }
  if (GTK_IS_LABEL(w_content))      gtk_label_set_text(GTK_LABEL(w_content), content ? content : "");

  /* Indent by depth */
  gtk_widget_set_margin_start(row, depth * 16);

   /* Thread classes and indicator */
  if (w_indicator) {
    gtk_widget_set_visible(w_indicator, TRUE);
    /* apply root-id based class nibble if available */
    if (root_id && strlen(root_id) > 0) {
      char cls[16];
      char c = g_ascii_tolower(root_id[0]);
      if (!g_ascii_isxdigit(c)) c = '0';
      g_snprintf(cls, sizeof(cls), "root-%c", c);
      GtkStyleContext *sc = gtk_widget_get_style_context(w_indicator);
      /* remove any previous root-* class to avoid accumulation */
      const char *roots[] = {"root-0","root-1","root-2","root-3","root-4","root-5","root-6","root-7","root-8","root-9","root-a","root-b","root-c","root-d","root-e","root-f"};
      for (guint i = 0; i < G_N_ELEMENTS(roots); i++) gtk_style_context_remove_class(sc, roots[i]);
      gtk_style_context_add_class(sc, cls);
    }
  }
  GtkStyleContext *row_sc = gtk_widget_get_style_context(row);
  gtk_style_context_add_class(row_sc, is_reply ? "thread-reply" : "thread-root");

  g_debug("factory bind: item=%p content=%.60s depth=%u", (void*)item, content ? content : "", depth);
  g_free(display); g_free(handle); g_free(ts); g_free(content); g_free(root_id);
}

static void setup_default_factory(GnostrTimelineView *self) {
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), NULL);
  gtk_list_view_set_factory(GTK_LIST_VIEW(self->list_view), factory);
  g_object_unref(factory);
  g_debug("setup_default_factory: list_view=%p", (void*)self->list_view);
}

/* Child model function for GtkTreeListModel (passthrough) */
static GListModel *timeline_child_model_func(gpointer item, gpointer user_data) {
  (void)user_data;
  if (!item || !G_TYPE_CHECK_INSTANCE_TYPE(item, timeline_item_get_type())) return NULL;
  return timeline_item_get_children_model((TimelineItem*)item);
}

static void ensure_list_model(GnostrTimelineView *self) {
  if (self->list_model) return;
  self->list_model = g_list_store_new(timeline_item_get_type());
  self->selection_model = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->list_model)));
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
  g_debug("ensure_list_model: list_model=%p selection_model=%p", (void*)self->list_model, (void*)self->selection_model);
}

static void gnostr_timeline_view_class_init(GnostrTimelineViewClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  gobj_class->dispose = gnostr_timeline_view_dispose;
  gobj_class->finalize = gnostr_timeline_view_finalize;
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, root_scroller);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, list_view);
}

static void gnostr_timeline_view_init(GnostrTimelineView *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  /* Ensure our single child fills available space */
  GtkLayoutManager *lm = gtk_box_layout_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_layout_manager(GTK_WIDGET(self), lm);
  /* Child widgets already have hexpand/vexpand in template */
  g_debug("timeline_view init: self=%p root_scroller=%p list_view=%p", (void*)self, (void*)self->root_scroller, (void*)self->list_view);
  setup_default_factory(self);

  /* Install minimal CSS for thread indicator and avatar */
  static const char *css =
    ".avatar { border-radius: 18px; background: @theme_bg_color; padding: 2px; }\n"
    ".dim-label { opacity: 0.7; }\n"
    ".thread-reply { }\n"
    ".thread-root { }\n"
    ".thread-indicator { min-width: 4px; min-height: 4px; background: @theme_selected_bg_color; }\n"
    ".root-0 { background: #6b7280; } .root-1 { background: #ef4444; } .root-2 { background: #f59e0b; } .root-3 { background: #10b981; }\n"
    ".root-4 { background: #3b82f6; } .root-5 { background: #8b5cf6; } .root-6 { background: #ec4899; } .root-7 { background: #22c55e; }\n"
    ".root-8 { background: #06b6d4; } .root-9 { background: #f97316; } .root-a { background: #0ea5e9; } .root-b { background: #84cc16; }\n"
    ".root-c { background: #a855f7; } .root-d { background: #eab308; } .root-e { background: #f43f5e; } .root-f { background: #14b8a6; }\n";
  GtkCssProvider *prov = gtk_css_provider_new();
  gtk_css_provider_load_from_data(prov, css, -1);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(prov);
}

GtkWidget *gnostr_timeline_view_new(void) {
  return g_object_new(GNOSTR_TYPE_TIMELINE_VIEW, NULL);
}

void gnostr_timeline_view_set_model(GnostrTimelineView *self, GtkSelectionModel *model) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  if (self->selection_model == model) return;
  if (self->selection_model) g_clear_object(&self->selection_model);
  if (self->list_model) g_clear_object(&self->list_model);
  if (self->tree_model) g_clear_object(&self->tree_model);
  self->selection_model = model ? g_object_ref(model) : NULL;
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
}

/* New: set tree roots model (GListModel of TimelineItem), creating a GtkTreeListModel passthrough */
void gnostr_timeline_view_set_tree_roots(GnostrTimelineView *self, GListModel *roots) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  if (self->selection_model) g_clear_object(&self->selection_model);
  if (self->tree_model) g_clear_object(&self->tree_model);
  if (roots) {
    /* root=roots, passthrough=TRUE, autoexpand=TRUE */
    self->tree_model = gtk_tree_list_model_new(roots, TRUE, TRUE,
                                               (GtkTreeListModelCreateModelFunc)timeline_child_model_func,
                                               NULL, NULL);
    self->selection_model = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->tree_model)));
  } else {
    self->tree_model = NULL;
    self->selection_model = NULL;
  }
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
}

void gnostr_timeline_view_prepend_text(GnostrTimelineView *self, const char *text) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  ensure_list_model(self);
  TimelineItem *item = timeline_item_new(NULL, NULL, NULL, text, 0);
  /* Prepend by inserting at position 0 */
  g_list_store_insert(self->list_model, 0, item);
  g_object_unref(item);
  /* Auto-scroll to top so the newly prepended row is visible */
  if (self->root_scroller && GTK_IS_SCROLLED_WINDOW(self->root_scroller)) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller));
    if (vadj) {
      gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
    }
  }
  g_debug("prepend_text: added=%.40s count=%u", text ? text : "", (unsigned)g_list_model_get_n_items(G_LIST_MODEL(self->list_model)));
}

/* New: prepend a fully specified item */
void gnostr_timeline_view_prepend(GnostrTimelineView *self,
                                  const char *display,
                                  const char *handle,
                                  const char *ts,
                                  const char *content,
                                  guint depth) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  ensure_list_model(self);
  TimelineItem *item = timeline_item_new(display, handle, ts, content, depth);
  g_list_store_insert(self->list_model, 0, item);
  g_object_unref(item);
  if (self->root_scroller && GTK_IS_SCROLLED_WINDOW(self->root_scroller)) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller));
    if (vadj) gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
  }
}
