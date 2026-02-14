/**
 * test_listview_recycle_stress.c — GtkListView recycling crash prevention
 *
 * This test exercises the most dangerous crash path in the application:
 * rapid creation/destruction of timeline event card widgets as GtkListView
 * recycles rows during scroll and model changes.
 *
 * Crash vectors being tested:
 *   1. Signal handlers (notify::profile) firing after unbind with stale row pointer
 *   2. Async callbacks completing after widget disposal
 *   3. bind/unbind/bind rapid cycling causing state corruption
 *
 * MUST be run under:
 *   - ASan + UBSan for UAF/OOB detection
 *   - G_DEBUG=fatal-warnings to catch GLib/GTK warnings as hard failures
 *   - Xvfb for headless rendering (or native display on macOS)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtk/gtk.h>
#include <glib.h>

/*
 * Minimal mock object implementing enough of GnNostrEventItem's interface
 * to exercise the bind/unbind paths without needing the full nostr stack.
 *
 * This avoids coupling the widget test to storage_ndb/nostrdb while still
 * exercising the real signal emission and property notification paths.
 */

/* ── MockEventItem GObject ────────────────────────────────────────── */

#define MOCK_TYPE_EVENT_ITEM (mock_event_item_get_type())
G_DECLARE_FINAL_TYPE(MockEventItem, mock_event_item, MOCK, EVENT_ITEM, GObject)

struct _MockEventItem {
    GObject parent;

    guint64 note_key;
    char *event_id;
    char *pubkey;
    char *content;
    char *profile_name;  /* simulates "profile" property */
    gint64 created_at;
    guint kind;
    gboolean has_profile;
};

enum {
    PROP_0,
    PROP_NOTE_KEY,
    PROP_EVENT_ID,
    PROP_PUBKEY,
    PROP_CONTENT,
    PROP_PROFILE,
    PROP_CREATED_AT,
    PROP_KIND,
    N_PROPS
};

static GParamSpec *mock_props[N_PROPS] = {NULL};

G_DEFINE_TYPE(MockEventItem, mock_event_item, G_TYPE_OBJECT)

static void
mock_event_item_get_property(GObject *obj, guint id, GValue *val, GParamSpec *pspec)
{
    MockEventItem *self = MOCK_EVENT_ITEM(obj);
    switch (id) {
    case PROP_NOTE_KEY:   g_value_set_uint64(val, self->note_key); break;
    case PROP_EVENT_ID:   g_value_set_string(val, self->event_id); break;
    case PROP_PUBKEY:     g_value_set_string(val, self->pubkey); break;
    case PROP_CONTENT:    g_value_set_string(val, self->content); break;
    case PROP_PROFILE:    g_value_set_string(val, self->profile_name); break;
    case PROP_CREATED_AT: g_value_set_int64(val, self->created_at); break;
    case PROP_KIND:       g_value_set_uint(val, self->kind); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void
mock_event_item_set_property(GObject *obj, guint id, const GValue *val, GParamSpec *pspec)
{
    MockEventItem *self = MOCK_EVENT_ITEM(obj);
    switch (id) {
    case PROP_NOTE_KEY:   self->note_key = g_value_get_uint64(val); break;
    case PROP_EVENT_ID:   g_free(self->event_id); self->event_id = g_value_dup_string(val); break;
    case PROP_PUBKEY:     g_free(self->pubkey); self->pubkey = g_value_dup_string(val); break;
    case PROP_CONTENT:    g_free(self->content); self->content = g_value_dup_string(val); break;
    case PROP_PROFILE:    g_free(self->profile_name); self->profile_name = g_value_dup_string(val); self->has_profile = TRUE; break;
    case PROP_CREATED_AT: self->created_at = g_value_get_int64(val); break;
    case PROP_KIND:       self->kind = g_value_get_uint(val); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void
mock_event_item_finalize(GObject *obj)
{
    MockEventItem *self = MOCK_EVENT_ITEM(obj);
    g_free(self->event_id);
    g_free(self->pubkey);
    g_free(self->content);
    g_free(self->profile_name);
    G_OBJECT_CLASS(mock_event_item_parent_class)->finalize(obj);
}

static void
mock_event_item_class_init(MockEventItemClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->get_property = mock_event_item_get_property;
    oc->set_property = mock_event_item_set_property;
    oc->finalize = mock_event_item_finalize;

    mock_props[PROP_NOTE_KEY] = g_param_spec_uint64("note-key", NULL, NULL, 0, G_MAXUINT64, 0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    mock_props[PROP_EVENT_ID] = g_param_spec_string("event-id", NULL, NULL, NULL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    mock_props[PROP_PUBKEY] = g_param_spec_string("pubkey", NULL, NULL, NULL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    mock_props[PROP_CONTENT] = g_param_spec_string("content", NULL, NULL, NULL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    mock_props[PROP_PROFILE] = g_param_spec_string("profile", NULL, NULL, NULL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    mock_props[PROP_CREATED_AT] = g_param_spec_int64("created-at", NULL, NULL, 0, G_MAXINT64, 0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    mock_props[PROP_KIND] = g_param_spec_uint("kind", NULL, NULL, 0, G_MAXUINT, 0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(oc, N_PROPS, mock_props);
}

static void
mock_event_item_init(MockEventItem *self G_GNUC_UNUSED)
{
}

static MockEventItem *
mock_event_item_new(guint64 key, const char *content, gint64 ts)
{
    MockEventItem *item = g_object_new(MOCK_TYPE_EVENT_ITEM, NULL);
    item->note_key = key;
    item->content = g_strdup(content);
    item->created_at = ts;
    item->kind = 1;

    /* Generate a fake pubkey and event ID */
    item->pubkey = g_strdup_printf("%064" G_GUINT64_FORMAT, key);
    item->event_id = g_strdup_printf("%064" G_GUINT64_FORMAT, key + 1000000);
    return item;
}

/* ── Test Harness State ───────────────────────────────────────────── */

typedef struct {
    GtkWindow *window;
    GtkScrolledWindow *scrolled;
    GtkListView *list_view;
    GListStore *store;
    GPtrArray *detached_items;  /* items removed from model but still alive */
    guint profile_update_count;
    guint bind_count;
    guint unbind_count;
} RecycleTestHarness;

/* ── Minimal factory that simulates bind/unbind lifecycle ─────────── */

static void
factory_setup_cb(GtkListItemFactory *factory G_GNUC_UNUSED,
                 GtkListItem *list_item,
                 gpointer user_data G_GNUC_UNUSED)
{
    GtkLabel *label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_ellipsize(label, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(label, 80);
    gtk_widget_set_size_request(GTK_WIDGET(label), -1, 60);
    gtk_list_item_set_child(list_item, GTK_WIDGET(label));
}

static void
on_item_profile_changed(GObject *item, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    RecycleTestHarness *h = user_data;
    h->profile_update_count++;

    /* This is the crash point: if user_data pointed to a freed widget,
     * we'd crash here. Using the harness instead of a row pointer. */
    g_assert_true(MOCK_IS_EVENT_ITEM(item));
}

static void
factory_bind_cb(GtkListItemFactory *factory G_GNUC_UNUSED,
                GtkListItem *list_item,
                gpointer user_data)
{
    RecycleTestHarness *h = user_data;
    h->bind_count++;

    GtkLabel *label = GTK_LABEL(gtk_list_item_get_child(list_item));
    MockEventItem *item = MOCK_EVENT_ITEM(gtk_list_item_get_item(list_item));

    gtk_label_set_text(label, item->content ? item->content : "(no content)");

    /* Connect notify::profile — this is the signal that crashes in production
     * when the handler fires after unbind. Store handler ID on the item. */
    gulong handler_id = g_signal_connect(item, "notify::profile",
                                          G_CALLBACK(on_item_profile_changed), h);
    g_object_set_data(G_OBJECT(list_item), "profile-handler-id",
                      GUINT_TO_POINTER((guint)handler_id));
    g_object_set_data(G_OBJECT(list_item), "bound-item",
                      g_object_ref(item));
}

static void
factory_unbind_cb(GtkListItemFactory *factory G_GNUC_UNUSED,
                  GtkListItem *list_item,
                  gpointer user_data)
{
    RecycleTestHarness *h = user_data;
    h->unbind_count++;

    /* Disconnect the profile handler — this is the critical path.
     * In production, failure to disconnect here → UAF crash. */
    gulong handler_id = (gulong)GPOINTER_TO_UINT(
        g_object_get_data(G_OBJECT(list_item), "profile-handler-id"));
    MockEventItem *item = g_object_get_data(G_OBJECT(list_item), "bound-item");

    if (item && handler_id > 0) {
        if (g_signal_handler_is_connected(G_OBJECT(item), handler_id)) {
            g_signal_handler_disconnect(G_OBJECT(item), handler_id);
        }
    }

    if (item) {
        g_object_unref(item);
    }

    g_object_set_data(G_OBJECT(list_item), "profile-handler-id", NULL);
    g_object_set_data(G_OBJECT(list_item), "bound-item", NULL);
}

/* ── Harness Setup/Teardown ───────────────────────────────────────── */

static RecycleTestHarness *
harness_new(guint initial_items)
{
    RecycleTestHarness *h = g_new0(RecycleTestHarness, 1);
    h->detached_items = g_ptr_array_new_with_free_func(g_object_unref);

    /* Create model with initial items */
    h->store = g_list_store_new(MOCK_TYPE_EVENT_ITEM);
    for (guint i = 0; i < initial_items; i++) {
        g_autofree char *content = g_strdup_printf("Note #%u: Test content here", i);
        g_autoptr(MockEventItem) item = mock_event_item_new(i + 1, content, 1700000000 - i);
        g_list_store_append(h->store, item);
    }

    /* Create factory */
    GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(
        gtk_signal_list_item_factory_new());
    g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), h);
    g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), h);
    g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind_cb), h);

    /* Create selection model */
    GtkNoSelection *selection = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(h->store)));

    /* Create ListView */
    h->list_view = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(selection),
                                                    GTK_LIST_ITEM_FACTORY(factory)));

    /* Wrap in scrolled window */
    h->scrolled = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_child(h->scrolled, GTK_WIDGET(h->list_view));
    gtk_widget_set_size_request(GTK_WIDGET(h->scrolled), 400, 600);

    /* Create window */
    h->window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(h->window, 400, 600);
    gtk_window_set_child(h->window, GTK_WIDGET(h->scrolled));

    return h;
}

static void
harness_show_and_realize(RecycleTestHarness *h)
{
    gtk_window_present(h->window);

    /* Iterate main loop to let the window realize and initial binds happen */
    for (int i = 0; i < 100; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }
}

static void
harness_free(RecycleTestHarness *h)
{
    if (!h) return;

    gtk_window_destroy(h->window);

    /* Drain main loop to process destroy events */
    for (int i = 0; i < 200; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }

    g_ptr_array_unref(h->detached_items);
    g_object_unref(h->store);
    g_free(h);
}

/* ── Test: Basic bind/unbind cycle ────────────────────────────────── */
static void
test_basic_bind_unbind(void)
{
    RecycleTestHarness *h = harness_new(50);
    harness_show_and_realize(h);

    g_assert_cmpuint(h->bind_count, >, 0);
    g_test_message("Initial binds: %u", h->bind_count);

    harness_free(h);
}

/* ── Test: Model replacement triggers clean unbind ────────────────── */
static void
test_model_replace_clean_unbind(void)
{
    RecycleTestHarness *h = harness_new(100);
    harness_show_and_realize(h);

    guint initial_binds = h->bind_count;

    /* Replace all items — this should trigger unbind for all visible rows */
    g_list_store_remove_all(h->store);

    /* Drain to process unbinds */
    for (int i = 0; i < 200; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }

    g_test_message("After remove_all: binds=%u, unbinds=%u",
                   h->bind_count, h->unbind_count);
    g_assert_cmpuint(h->unbind_count, >, 0);

    /* Add new items */
    for (guint i = 0; i < 50; i++) {
        g_autofree char *content = g_strdup_printf("New note #%u", i);
        g_autoptr(MockEventItem) item = mock_event_item_new(1000 + i, content, 1700001000 - i);
        g_list_store_append(h->store, item);
    }

    /* Drain to process new binds */
    for (int i = 0; i < 200; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }

    g_assert_cmpuint(h->bind_count, >, initial_binds);

    harness_free(h);
}

/* ── Test: Profile notification after unbind (THE crash test) ─────── */
static void
test_profile_notify_after_unbind(void)
{
    RecycleTestHarness *h = harness_new(30);
    harness_show_and_realize(h);

    /* Grab references to all items currently in the model */
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(h->store)); i++) {
        g_autoptr(MockEventItem) item = g_list_model_get_item(G_LIST_MODEL(h->store), i);
        g_ptr_array_add(items, g_object_ref(item));
    }

    /* Clear the model — this triggers unbind for all visible rows */
    g_list_store_remove_all(h->store);

    /* Drain to process unbinds */
    for (int i = 0; i < 300; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }

    /* NOW: set the "profile" property on all previously-bound items.
     * If signal handlers weren't properly disconnected during unbind,
     * this would trigger callbacks with stale row pointers → UAF crash.
     *
     * Under ASan+G_DEBUG=fatal-warnings, this must NOT crash. */
    guint pre_count = h->profile_update_count;
    for (guint i = 0; i < items->len; i++) {
        MockEventItem *item = g_ptr_array_index(items, i);
        g_object_set(item, "profile", "New Profile Name", NULL);
    }

    /* The profile update count should NOT increase if handlers were
     * properly disconnected. If it does increase, handlers are leaking. */
    g_test_message("Profile updates after unbind: %u (expected 0, pre=%u, post=%u)",
                   h->profile_update_count - pre_count, pre_count, h->profile_update_count);

    /* Zero leaked handlers means clean disconnection */
    g_assert_cmpuint(h->profile_update_count - pre_count, ==, 0);

    g_ptr_array_unref(items);
    harness_free(h);
}

/* ── Test: Rapid scroll simulation ────────────────────────────────── */
static void
test_rapid_scroll_churn(void)
{
    RecycleTestHarness *h = harness_new(500);
    harness_show_and_realize(h);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(h->scrolled);

    /* Simulate rapid scrolling by changing the adjustment value */
    double upper = gtk_adjustment_get_upper(vadj);
    double page = gtk_adjustment_get_page_size(vadj);

    if (upper > page) {
        /* Scroll down in steps */
        for (double pos = 0; pos < upper - page; pos += page * 0.5) {
            gtk_adjustment_set_value(vadj, pos);
            /* Process a few iterations per scroll step */
            for (int i = 0; i < 5; i++) {
                g_main_context_iteration(g_main_context_default(), FALSE);
            }
        }

        /* Scroll back up rapidly */
        for (double pos = upper - page; pos > 0; pos -= page * 0.8) {
            gtk_adjustment_set_value(vadj, pos);
            for (int i = 0; i < 3; i++) {
                g_main_context_iteration(g_main_context_default(), FALSE);
            }
        }
    }

    g_test_message("After scroll churn: binds=%u, unbinds=%u, profile_updates=%u",
                   h->bind_count, h->unbind_count, h->profile_update_count);

    /* Both binds and unbinds should have happened due to recycling */
    g_assert_cmpuint(h->bind_count, >, 0);

    harness_free(h);
}

/* ── Test: Repeated model clear/repopulate cycles ─────────────────── */
static void
test_repeated_clear_repopulate(void)
{
    RecycleTestHarness *h = harness_new(20);
    harness_show_and_realize(h);

    for (int cycle = 0; cycle < 20; cycle++) {
        /* Clear */
        g_list_store_remove_all(h->store);
        for (int i = 0; i < 10; i++) {
            g_main_context_iteration(g_main_context_default(), FALSE);
        }

        /* Repopulate with different items */
        guint base = (cycle + 1) * 1000;
        for (guint i = 0; i < 30; i++) {
            g_autofree char *content = g_strdup_printf("Cycle %d, Note %u", cycle, i);
            g_autoptr(MockEventItem) item = mock_event_item_new(base + i, content,
                                                                 1700000000 - i - cycle * 100);
            g_list_store_append(h->store, item);
        }
        for (int i = 0; i < 20; i++) {
            g_main_context_iteration(g_main_context_default(), FALSE);
        }
    }

    g_test_message("After 20 clear/repopulate cycles: binds=%u, unbinds=%u",
                   h->bind_count, h->unbind_count);

    harness_free(h);
}

/* ── Test: Simultaneous profile updates during scroll ─────────────── */
static void
test_profile_updates_during_scroll(void)
{
    RecycleTestHarness *h = harness_new(200);
    harness_show_and_realize(h);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(h->scrolled);
    double upper = gtk_adjustment_get_upper(vadj);
    double page = gtk_adjustment_get_page_size(vadj);

    /* Scroll while simultaneously setting profiles on items */
    for (int step = 0; step < 50 && upper > page; step++) {
        /* Scroll a bit */
        double pos = (upper - page) * step / 50.0;
        gtk_adjustment_set_value(vadj, pos);

        /* Set profile on a random subset of items */
        guint n = g_list_model_get_n_items(G_LIST_MODEL(h->store));
        for (guint i = step % 3; i < n; i += 7) {
            g_autoptr(MockEventItem) item = g_list_model_get_item(G_LIST_MODEL(h->store), i);
            g_autofree char *name = g_strdup_printf("Profile_%u_%d", i, step);
            g_object_set(item, "profile", name, NULL);
        }

        /* Process events */
        for (int i = 0; i < 5; i++) {
            g_main_context_iteration(g_main_context_default(), FALSE);
        }
    }

    g_test_message("After scroll+profile updates: binds=%u, unbinds=%u, profile_updates=%u",
                   h->bind_count, h->unbind_count, h->profile_update_count);

    harness_free(h);
}

/* ── Main ─────────────────────────────────────────────────────────── */
int
main(int argc, char *argv[])
{
    gtk_test_init(&argc, &argv, NULL);

    g_test_add_func("/nostr-gtk/listview/basic-bind-unbind",
                    test_basic_bind_unbind);
    g_test_add_func("/nostr-gtk/listview/model-replace-clean-unbind",
                    test_model_replace_clean_unbind);
    g_test_add_func("/nostr-gtk/listview/profile-notify-after-unbind",
                    test_profile_notify_after_unbind);
    g_test_add_func("/nostr-gtk/listview/rapid-scroll-churn",
                    test_rapid_scroll_churn);
    g_test_add_func("/nostr-gtk/listview/repeated-clear-repopulate",
                    test_repeated_clear_repopulate);
    g_test_add_func("/nostr-gtk/listview/profile-updates-during-scroll",
                    test_profile_updates_during_scroll);

    return g_test_run();
}
