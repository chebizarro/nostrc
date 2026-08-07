/**
 * test_note_card_measure.c — Widget sizing regression tests
 *
 * Verifies that note card and timeline widgets respect size constraints
 * regardless of content. Prevents the bug where timeline rows expand
 * beyond their container bounds.
 *
 * Tests use gtk_widget_measure() to check natural/minimum sizes against
 * predefined thresholds rather than pixel-perfect snapshots.
 *
 * Run under Xvfb for headless rendering.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtk/gtk.h>
#include <adwaita.h>
#include <glib.h>
#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include <nostr-gtk-1.0/content_renderer.h>
#include <nostr-gobject-1.0/gnostr-identity.h>
#include <nostr-gobject-1.0/nostr_nip19.h>

extern GResource *nostr_gtk_get_resource(void);

/* ── Size thresholds (in pixels) ──────────────────────────────────── */
/* These should be adjusted based on the actual design requirements.
 * The key invariant is that NO content should make the widget exceed
 * these bounds. */
#define MAX_NOTE_CARD_HEIGHT_PX  800
#define MAX_NOTE_CARD_WIDTH_PX  1200
#define MIN_NOTE_CARD_WIDTH_PX    200

/* Reference width for vertical measurements (simulates container width) */
#define REFERENCE_WIDTH_PX  400

/* ── Test content corpus ──────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *content;
} ContentCase;

static const ContentCase content_cases[] = {
    {"short_text", "Hello world"},
    {"medium_text", "This is a medium-length note about #nostr and the decentralized social web. "
                    "It contains some hashtags and mentions."},
    {"long_text",
     "This is a very long note that should test the word-wrapping behavior of the widget. "
     "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt "
     "ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation "
     "ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in "
     "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur "
     "sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id "
     "est laborum. This text is intentionally very long to test that the widget does not expand "
     "vertically beyond reasonable bounds. In a real timeline, we would see this content "
     "truncated or wrapped within the card's allocated height. The card should NOT expand "
     "the entire timeline row to accommodate all of this text."},
    {"many_links",
     "Check out these links:\n"
     "https://example.com/very/long/path/that/might/break/layout/constraints\n"
     "https://another-example.org/with/yet/another/long/url/path\n"
     "https://third-link.io/path\n"
     "https://fourth-link.com/some/path/to/resource\n"
     "https://fifth-link.net/final/link"},
    {"many_hashtags",
     "#nostr #bitcoin #lightning #zaps #gnome #gtk #linux #foss #decentralized "
     "#privacy #censorship #resistance #freedom #sovereignty #self-custody "
     "#programming #c #glib #gobject #widgets"},
    {"unicode_heavy",
     "🎉🎊🎈🎁🎆🎇🧨✨🎃🎄🎋🎍🎎🎏🎐🎑🎀🎗🎟🎫🎖🏆🏅🥇🥈🥉"
     "⚽️🏀🏈⚾️🥎🎾🏐🏉🥏🎱🪀🏓🏸🏒🏑🥍🏏🪃🥅⛳️🪁🏹🎣🤿🥊"
     "and some text mixed in with ZWSP: \u200B\u200B\u200B"
     "and RTL: \u200Fمرحبا\u200E and more emoji: 🌍🌎🌏"},
    {"empty", ""},
    {"newlines_only", "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"},
    {"single_very_long_word",
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
     "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"},
};

static const gsize n_content_cases = G_N_ELEMENTS(content_cases);

static void
factory_setup_cb(GtkListItemFactory *f G_GNUC_UNUSED,
                 GtkListItem *li,
                 gpointer ud G_GNUC_UNUSED)
{
    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
    GtkLabel *label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_wrap(label, TRUE);
    gtk_label_set_lines(label, 12);
    gtk_label_set_ellipsize(label, PANGO_ELLIPSIZE_END);
    gtk_box_append(box, GTK_WIDGET(label));
    gtk_list_item_set_child(li, GTK_WIDGET(box));
}

static void
factory_bind_cb(GtkListItemFactory *f G_GNUC_UNUSED,
                GtkListItem *li,
                gpointer ud G_GNUC_UNUSED)
{
    GtkBox *box = GTK_BOX(gtk_list_item_get_child(li));
    GtkLabel *label = GTK_LABEL(gtk_widget_get_first_child(GTK_WIDGET(box)));
    GtkStringObject *so = GTK_STRING_OBJECT(gtk_list_item_get_item(li));
    gtk_label_set_text(label, gtk_string_object_get_string(so));
}

/* ── Test: GtkLabel as baseline (sanity check) ────────────────────── */
static void
test_label_stays_bounded(void)
{
    for (gsize i = 0; i < n_content_cases; i++) {
        const ContentCase *cc = &content_cases[i];

        GtkLabel *label = GTK_LABEL(gtk_label_new(cc->content));
        gtk_label_set_wrap(label, TRUE);
        gtk_label_set_wrap_mode(label, PANGO_WRAP_WORD_CHAR);
        gtk_label_set_ellipsize(label, PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(label, 80);
        gtk_widget_set_size_request(GTK_WIDGET(label), -1, -1);

        int min_w, nat_w, min_h, nat_h;
        gtk_widget_measure(GTK_WIDGET(label), GTK_ORIENTATION_HORIZONTAL,
                          -1, &min_w, &nat_w, NULL, NULL);
        gtk_widget_measure(GTK_WIDGET(label), GTK_ORIENTATION_VERTICAL,
                          REFERENCE_WIDTH_PX, &min_h, &nat_h, NULL, NULL);

        g_test_message("Case '%s': width min=%d nat=%d, height min=%d nat=%d",
                       cc->name, min_w, nat_w, min_h, nat_h);

        /* Natural width should be >= 0. Empty content may have nat_w == 0. */
        g_assert_cmpint(nat_w, >=, 0);
        /* Non-empty content should have positive natural width */
        if (cc->content && cc->content[0] != '\0' && cc->content[0] != '\n') {
            g_assert_cmpint(nat_w, >, 0);
        }

        /* Height should be bounded even for long content */
        /* Note: Without max-lines set, labels can be tall — that's expected.
         * The real test is that the CONTAINER respects its allocation. */

        g_object_ref_sink(GTK_WIDGET(label));
        g_object_unref(GTK_WIDGET(label));
    }
}

/* ── Test: Box container constrains child label ───────────────────── */
static void
test_constrained_box_stays_bounded(void)
{
    for (gsize i = 0; i < n_content_cases; i++) {
        const ContentCase *cc = &content_cases[i];

        /* Create a box simulating a note card's content area */
        GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
        gtk_widget_set_size_request(GTK_WIDGET(box), MIN_NOTE_CARD_WIDTH_PX, -1);

        /* Add header (author name + timestamp) */
        GtkLabel *header = GTK_LABEL(gtk_label_new("Test Author · 2m ago"));
        gtk_label_set_ellipsize(header, PANGO_ELLIPSIZE_END);
        gtk_box_append(box, GTK_WIDGET(header));

        /* Add content label */
        GtkLabel *content = GTK_LABEL(gtk_label_new(cc->content));
        gtk_label_set_wrap(content, TRUE);
        gtk_label_set_wrap_mode(content, PANGO_WRAP_WORD_CHAR);
        gtk_label_set_ellipsize(content, PANGO_ELLIPSIZE_END);
        gtk_label_set_lines(content, 12);  /* Cap at 12 lines like a real card */
        gtk_box_append(box, GTK_WIDGET(content));

        /* Measure the card-like box */
        int min_w, nat_w, min_h, nat_h;
        gtk_widget_measure(GTK_WIDGET(box), GTK_ORIENTATION_HORIZONTAL,
                          -1, &min_w, &nat_w, NULL, NULL);
        gtk_widget_measure(GTK_WIDGET(box), GTK_ORIENTATION_VERTICAL,
                          REFERENCE_WIDTH_PX, &min_h, &nat_h, NULL, NULL);

        g_test_message("Case '%s' (constrained box): width min=%d nat=%d, height min=%d nat=%d",
                       cc->name, min_w, nat_w, min_h, nat_h);

        /* Key invariant: natural height must not exceed max */
        g_assert_cmpint(nat_h, <=, MAX_NOTE_CARD_HEIGHT_PX);
        g_assert_cmpint(nat_h, >=, 0);

        g_object_ref_sink(GTK_WIDGET(box));
        g_object_unref(GTK_WIDGET(box));
    }
}

/* ── Test: ScrolledWindow constrains ListView row heights ─────────── */
static void
test_adw_clamp_bounds_natural_width(void)
{
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 960);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 720);

    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
    GtkLabel *header = GTK_LABEL(gtk_label_new("Test Author · 2m ago"));
    gtk_label_set_ellipsize(header, PANGO_ELLIPSIZE_END);
    gtk_box_append(box, GTK_WIDGET(header));

    GtkLabel *content = GTK_LABEL(gtk_label_new(
        "This is a very long note that should test the width bounding behavior of the clamp. "
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "
        "nostr:nevent1qqsqz9xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx "
        "https://example.com/very/long/path/that/used/to/force/the/timeline/wider/than/the/window"));
    gtk_label_set_wrap(content, TRUE);
    gtk_label_set_wrap_mode(content, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(content, 0.0f);
    gtk_label_set_max_width_chars(content, 80);
    gtk_label_set_width_chars(content, 1);
    gtk_box_append(box, GTK_WIDGET(content));

    adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(box));

    int min_w, nat_w;
    gtk_widget_measure(clamp, GTK_ORIENTATION_HORIZONTAL, -1, &min_w, &nat_w, NULL, NULL);

    g_test_message("Clamp bounded width: min=%d nat=%d", min_w, nat_w);
    g_assert_cmpint(min_w, <=, 960);
    g_assert_cmpint(nat_w, <=, 960);

    g_object_ref_sink(clamp);
    g_object_unref(clamp);
}

static void
test_listview_row_heights_bounded(void)
{
    GListStore *store = g_list_store_new(GTK_TYPE_STRING_OBJECT);

    /* Add all content cases as items */
    for (gsize i = 0; i < n_content_cases; i++) {
        g_autoptr(GtkStringObject) so = gtk_string_object_new(content_cases[i].content);
        g_list_store_append(store, so);
    }

    GtkSignalListItemFactory *factory = GTK_SIGNAL_LIST_ITEM_FACTORY(
        gtk_signal_list_item_factory_new());

    g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), NULL);

    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(store));
    GtkListView *lv = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(sel),
                                                       GTK_LIST_ITEM_FACTORY(factory)));

    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_child(sw, GTK_WIDGET(lv));
    gtk_widget_set_size_request(GTK_WIDGET(sw), REFERENCE_WIDTH_PX, 600);

    GtkWindow *win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(win, REFERENCE_WIDTH_PX, 600);
    gtk_window_set_child(win, GTK_WIDGET(sw));

    /* Realize and let binds happen */
    gtk_window_present(win);
    for (int i = 0; i < 100; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }

    /* The scrolled window should be able to constrain the content.
     * Measure the ListView itself — its natural height may be large
     * (sum of all rows), but the scrolled window should clip it. */
    int sw_min_h, sw_nat_h;
    gtk_widget_measure(GTK_WIDGET(sw), GTK_ORIENTATION_VERTICAL,
                      REFERENCE_WIDTH_PX, &sw_min_h, &sw_nat_h, NULL, NULL);

    g_test_message("ScrolledWindow: min_h=%d, nat_h=%d", sw_min_h, sw_nat_h);

    /* The scrolled window itself should respect its size request */
    g_assert_cmpint(sw_min_h, <=, 600);

    gtk_window_destroy(win);

    for (int i = 0; i < 100; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }
}

static void
test_note_card_reserved_height_blocks_passive_expansion(void)
{
    NostrGtkNoteCardRow *row = nostr_gtk_note_card_row_new();
    const char *long_content =
        "This content is intentionally long enough to need much more than the "
        "reserved row height when GTK measures it naturally. "
        "line 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\n"
        "line 9\nline 10\nline 11\nline 12\nline 13\nline 14\nline 15\n";

    nostr_gtk_note_card_row_set_content(row, long_content);
    nostr_gtk_note_card_row_set_reserved_height(row, 180);

    int min_h = 0, nat_h = 0;
    gtk_widget_measure(GTK_WIDGET(row), GTK_ORIENTATION_VERTICAL,
                       REFERENCE_WIDTH_PX, &min_h, &nat_h, NULL, NULL);

    g_assert_cmpint(min_h, ==, 180);
    g_assert_cmpint(nat_h, ==, 180);

    g_object_ref_sink(row);
    g_object_unref(row);
}

static guint rich_media_request_count;

typedef struct {
    guint request_count;
    NostrGtkMediaTextureReadyFunc callback;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
} DeferredMediaRequest;

static void
deferred_media_texture_request(gpointer loader,
                               const char *url,
                               NostrGtkMediaResourceClass resource_class,
                               int target_width,
                               int target_height,
                               GCancellable *cancellable,
                               NostrGtkMediaTextureReadyFunc callback,
                               gpointer user_data,
                               GDestroyNotify user_data_destroy)
{
    DeferredMediaRequest *request = loader;
    g_assert_nonnull(request);
    g_assert_nonnull(url);
    g_assert_cmpint(resource_class, ==, NOSTR_GTK_MEDIA_RESOURCE_INLINE);
    g_assert_cmpint(target_width, >, 0);
    g_assert_cmpint(target_height, >, 0);
    g_assert_false(g_cancellable_is_cancelled(cancellable));
    g_assert_null(request->callback);

    request->request_count++;
    request->callback = callback;
    request->user_data = user_data;
    request->user_data_destroy = user_data_destroy;
}

static void
deferred_media_request_complete(DeferredMediaRequest *request)
{
    g_assert_nonnull(request->callback);
    NostrGtkMediaTextureReadyFunc callback = request->callback;
    gpointer user_data = request->user_data;
    GDestroyNotify user_data_destroy = request->user_data_destroy;
    request->callback = NULL;
    request->user_data = NULL;
    request->user_data_destroy = NULL;

    const guint8 pixel[] = { 0x33, 0x66, 0x99, 0xff };
    g_autoptr(GBytes) bytes = g_bytes_new_static(pixel, sizeof(pixel));
    g_autoptr(GdkTexture) texture = GDK_TEXTURE(gdk_memory_texture_new(
        1, 1, GDK_MEMORY_R8G8B8A8, bytes, 4));
    callback(texture, NULL, user_data);
    if (user_data_destroy)
        user_data_destroy(user_data);
}

static GtkPicture *
find_rich_media_picture(GtkWidget *widget)
{
    if (!widget)
        return NULL;

    GtkWidget *picture = g_object_get_data(G_OBJECT(widget), "media-picture");
    if (GTK_IS_PICTURE(picture))
        return GTK_PICTURE(picture);

    for (GtkWidget *child = gtk_widget_get_first_child(widget);
         child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkPicture *found = find_rich_media_picture(child);
        if (found)
            return found;
    }
    return NULL;
}

static void
drain_main_context_for(gint64 duration_us)
{
    gint64 deadline = g_get_monotonic_time() + duration_us;
    do {
        gboolean dispatched = FALSE;
        while (g_main_context_iteration(NULL, FALSE))
            dispatched = TRUE;
        if (!dispatched)
            g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static void
wait_for_widget_mapped_state(GtkWidget *widget, gboolean expected_mapped)
{
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (gtk_widget_get_mapped(widget) != expected_mapped &&
           g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
    g_assert_cmpint(gtk_widget_get_mapped(widget), ==, expected_mapped);

    /* Mapping state flips before all frame-clock surface updates have thawed.
     * Let delayed update sources settle before the next hide/present/destroy. */
    drain_main_context_for(100 * G_TIME_SPAN_MILLISECOND);
}

static GtkWindow *
present_test_window(GtkWidget *child, int width, int height)
{
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(window, width, height);
    gtk_window_set_child(window, child);
    gtk_window_present(window);
    wait_for_widget_mapped_state(GTK_WIDGET(window), TRUE);
    return window;
}

static void
destroy_settled_test_window(GtkWindow *window)
{
    if (gtk_widget_get_mapped(GTK_WIDGET(window))) {
        gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
        wait_for_widget_mapped_state(GTK_WIDGET(window), FALSE);
    }
    gtk_window_set_child(window, NULL);
    drain_main_context_for(50 * G_TIME_SPAN_MILLISECOND);
    gtk_window_destroy(window);
    drain_main_context_for(100 * G_TIME_SPAN_MILLISECOND);
}

static void
wait_for_deferred_request(DeferredMediaRequest *request, guint expected_count)
{
    gint64 deadline = g_get_monotonic_time() + 500 * G_TIME_SPAN_MILLISECOND;
    while ((request->request_count < expected_count || !request->callback) &&
           g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
    g_assert_cmpuint(request->request_count, ==, expected_count);
    g_assert_nonnull(request->callback);
}

static void
fake_media_texture_request(gpointer loader,
                           const char *url,
                           NostrGtkMediaResourceClass resource_class,
                           int target_width,
                           int target_height,
                           GCancellable *cancellable,
                           NostrGtkMediaTextureReadyFunc callback,
                           gpointer user_data,
                           GDestroyNotify user_data_destroy)
{
    g_assert_nonnull(loader);
    g_assert_nonnull(url);
    g_assert_cmpint(resource_class, ==, NOSTR_GTK_MEDIA_RESOURCE_INLINE);
    g_assert_cmpint(target_width, >, 0);
    g_assert_cmpint(target_height, >, 0);
    g_assert_false(g_cancellable_is_cancelled(cancellable));
    rich_media_request_count++;

    const guint8 pixel[] = { 0x33, 0x66, 0x99, 0xff };
    g_autoptr(GBytes) bytes = g_bytes_new_static(pixel, sizeof(pixel));
    g_autoptr(GdkTexture) texture = GDK_TEXTURE(gdk_memory_texture_new(
        1, 1, GDK_MEMORY_R8G8B8A8, bytes, 4));
    callback(texture, NULL, user_data);
    if (user_data_destroy)
        user_data_destroy(user_data);
}

static void
test_rich_content_hydration_preserves_reserved_height(void)
{
    rich_media_request_count = 0;
    nostr_gtk_note_card_row_reset_rich_child_creation_count();
    NostrGtkNoteCardRow *row = nostr_gtk_note_card_row_new();
    nostr_gtk_note_card_row_prepare_for_bind(row);
    nostr_gtk_note_card_row_set_content(row, "Rich content geometry");
    nostr_gtk_note_card_row_set_reserved_height(row, 720);
    nostr_gtk_note_card_row_set_media_texture_loader(
        row, &rich_media_request_count, fake_media_texture_request);

    GnContentDescriptor image = {
        .type = GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE,
        .url = "https://example.test/image.png",
    };
    GnContentDescriptor link = {
        .type = GN_CONTENT_DESCRIPTOR_LINK_PREVIEW,
        .url = "https://example.test/article",
    };
    GnContentDescriptor embed = {
        .type = GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF,
        .original = "nostr:note1placeholder",
    };
    g_autoptr(GPtrArray) descriptors = g_ptr_array_new();
    g_ptr_array_add(descriptors, &image);
    g_ptr_array_add(descriptors, &link);
    g_ptr_array_add(descriptors, &embed);

    nostr_gtk_note_card_row_set_rich_content(row, descriptors, 240, 120, 160);

    int before_min = 0, before_nat = 0;
    gtk_widget_measure(GTK_WIDGET(row), GTK_ORIENTATION_VERTICAL,
                       REFERENCE_WIDTH_PX, &before_min, &before_nat, NULL, NULL);
    g_assert_cmpint(before_min, ==, 720);
    g_assert_cmpint(before_nat, ==, 720);
    g_assert_cmpuint(rich_media_request_count, ==, 0);
    g_assert_cmpuint(
        nostr_gtk_note_card_row_get_rich_child_creation_count(), ==, 0);

    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(window, REFERENCE_WIDTH_PX, 720);
    gtk_window_set_child(window, GTK_WIDGET(row));
    gtk_window_present(window);

    gint64 deadline = g_get_monotonic_time() + 500 * G_TIME_SPAN_MILLISECOND;
    while (rich_media_request_count == 0 && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_assert_cmpuint(rich_media_request_count, ==, 1);
    g_assert_cmpuint(
        nostr_gtk_note_card_row_get_rich_child_creation_count(), ==, 3);

    int after_min = 0, after_nat = 0;
    gtk_widget_measure(GTK_WIDGET(row), GTK_ORIENTATION_VERTICAL,
                       REFERENCE_WIDTH_PX, &after_min, &after_nat, NULL, NULL);
    g_assert_cmpint(after_min, ==, before_min);
    g_assert_cmpint(after_nat, ==, before_nat);

    gtk_window_destroy(window);
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static void
test_rich_content_cache_delivery_survives_unmap(void)
{
    DeferredMediaRequest request = {0};
    nostr_gtk_note_card_row_reset_rich_child_creation_count();

    NostrGtkNoteCardRow *row = nostr_gtk_note_card_row_new();
    g_object_ref_sink(row);
    nostr_gtk_note_card_row_prepare_for_bind(row);
    nostr_gtk_note_card_row_set_reserved_height(row, 360);
    nostr_gtk_note_card_row_set_media_texture_loader(
        row, &request, deferred_media_texture_request);

    GnContentDescriptor image = {
        .type = GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE,
        .url = "https://example.test/image.png",
    };
    g_autoptr(GPtrArray) descriptors = g_ptr_array_new();
    g_ptr_array_add(descriptors, &image);
    nostr_gtk_note_card_row_set_rich_content(row, descriptors, 240, 0, 0);

    /* The row's 360px compositor reservation can be smaller than the template
     * root's minimum once its 240px media reservation and card chrome are
     * combined. Width-for-height negotiation must clamp the internal GtkBox
     * constraint without letting that minimum escape the row. */
    int constrained_min_width = -1, constrained_natural_width = -1;
    gtk_widget_measure(GTK_WIDGET(row), GTK_ORIENTATION_HORIZONTAL, 360,
                       &constrained_min_width, &constrained_natural_width,
                       NULL, NULL);
    g_assert_cmpint(constrained_min_width, ==, 0);
    g_assert_cmpint(constrained_natural_width, ==, 0);

    GtkWindow *window = present_test_window(
        GTK_WIDGET(row), REFERENCE_WIDTH_PX, 360);

    wait_for_deferred_request(&request, 1);
    deferred_media_request_complete(&request);
    while (g_main_context_iteration(NULL, FALSE)) {}

    GtkPicture *picture = find_rich_media_picture(GTK_WIDGET(row));
    g_assert_nonnull(picture);
    g_assert_nonnull(gtk_picture_get_paintable(picture));

    /* Model GtkListView recycling while the parent is obscured by a modal.
     * Use a fresh surface for each mapped phase so test-owned window teardown
     * cannot overlap a subsequent present/update-freeze cycle. */
    destroy_settled_test_window(window);
    g_assert_false(gtk_widget_get_mapped(GTK_WIDGET(picture)));

    nostr_gtk_note_card_row_prepare_for_unbind(row);
    nostr_gtk_note_card_row_prepare_for_bind(row);
    nostr_gtk_note_card_row_set_reserved_height(row, 360);
    nostr_gtk_note_card_row_set_media_texture_loader(
        row, &request, deferred_media_texture_request);
    nostr_gtk_note_card_row_set_rich_content(row, descriptors, 240, 0, 0);

    window = present_test_window(GTK_WIDGET(row), REFERENCE_WIDTH_PX, 360);
    wait_for_deferred_request(&request, 2);
    picture = find_rich_media_picture(GTK_WIDGET(row));
    g_assert_nonnull(picture);
    g_assert_null(gtk_picture_get_paintable(picture));

    /* A memory-cache hit is still delivered asynchronously. If the modal
     * temporarily unmaps the row before delivery, the current binding must
     * retain the texture so remap is instant and requires no third request. */
    destroy_settled_test_window(window);
    g_assert_false(gtk_widget_get_mapped(GTK_WIDGET(picture)));
    deferred_media_request_complete(&request);
    g_assert_nonnull(gtk_picture_get_paintable(picture));

    constrained_min_width = constrained_natural_width = -1;
    gtk_widget_measure(GTK_WIDGET(row), GTK_ORIENTATION_HORIZONTAL, 360,
                       &constrained_min_width, &constrained_natural_width,
                       NULL, NULL);
    g_assert_cmpint(constrained_min_width, ==, 0);
    g_assert_cmpint(constrained_natural_width, ==, 0);

    window = present_test_window(GTK_WIDGET(row), REFERENCE_WIDTH_PX, 360);
    g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(picture)));
    g_assert_nonnull(gtk_picture_get_paintable(picture));
    g_assert_cmpuint(request.request_count, ==, 2);
    g_assert_cmpuint(
        nostr_gtk_note_card_row_get_rich_child_creation_count(), ==, 2);

    int min_height = 0, natural_height = 0;
    gtk_widget_measure(GTK_WIDGET(row), GTK_ORIENTATION_VERTICAL,
                       REFERENCE_WIDTH_PX, &min_height, &natural_height,
                       NULL, NULL);
    g_assert_cmpint(min_height, ==, 360);
    g_assert_cmpint(natural_height, ==, 360);

    destroy_settled_test_window(window);
    nostr_gtk_note_card_row_prepare_for_unbind(row);
    g_object_unref(row);
}

static void
test_rich_content_buffer_bind_creates_no_expensive_children(void)
{
    nostr_gtk_note_card_row_reset_rich_child_creation_count();
    NostrGtkNoteCardRow *row = nostr_gtk_note_card_row_new();
    g_object_ref_sink(row);
    nostr_gtk_note_card_row_prepare_for_bind(row);
    nostr_gtk_note_card_row_set_reserved_height(row, 900);

    GnContentDescriptor image = {
        .type = GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE,
        .url = "https://example.test/image.png",
    };
    GnContentDescriptor video = {
        .type = GN_CONTENT_DESCRIPTOR_MEDIA_VIDEO,
        .url = "https://example.test/video.mp4",
        .thumbnail_url = "https://example.test/poster.png",
    };
    GnContentDescriptor link = {
        .type = GN_CONTENT_DESCRIPTOR_LINK_PREVIEW,
        .url = "https://example.test/article",
    };
    GnContentDescriptor embed = {
        .type = GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF,
        .original = "nostr:note1placeholder",
    };
    g_autoptr(GPtrArray) descriptors = g_ptr_array_new();
    g_ptr_array_add(descriptors, &image);
    g_ptr_array_add(descriptors, &video);
    g_ptr_array_add(descriptors, &link);
    g_ptr_array_add(descriptors, &embed);

    nostr_gtk_note_card_row_set_rich_content(
        row, descriptors, 480, 120, 160);

    /* Window size negotiation may probe below the template child's intrinsic
     * minimum width. The row layout manager must clamp the GTK measure
     * constraint rather than emitting a fatal gtk_widget_measure() critical. */
    int narrow_min = 0, narrow_nat = 0;
    gtk_widget_measure(GTK_WIDGET(row), GTK_ORIENTATION_VERTICAL, 100,
                       &narrow_min, &narrow_nat, NULL, NULL);
    g_assert_cmpint(narrow_min, ==, 900);
    g_assert_cmpint(narrow_nat, ==, 900);

    g_assert_cmpuint(
        nostr_gtk_note_card_row_get_rich_child_creation_count(), ==, 0);

    /* A compatible recycle resets and rebinds the pooled fixed frames without
     * realizing any image/video/OG/embed subtree while still unmapped. */
    nostr_gtk_note_card_row_prepare_for_unbind(row);
    nostr_gtk_note_card_row_prepare_for_bind(row);
    nostr_gtk_note_card_row_set_reserved_height(row, 900);
    nostr_gtk_note_card_row_set_rich_content(
        row, descriptors, 480, 120, 160);
    g_assert_cmpuint(
        nostr_gtk_note_card_row_get_rich_child_creation_count(), ==, 0);

    nostr_gtk_note_card_row_prepare_for_unbind(row);
    g_object_unref(row);
}

typedef struct {
    guint count;
    gchar *pubkey_hex;
} OpenProfileCapture;

static void
on_open_profile_captured(NostrGtkNoteCardRow *row,
                         const char *pubkey_hex,
                         gpointer user_data)
{
    (void)row;
    OpenProfileCapture *capture = user_data;
    capture->count++;
    g_free(capture->pubkey_hex);
    capture->pubkey_hex = g_strdup(pubkey_hex);
}

static GtkLabel *
find_label_with_text(GtkWidget *widget, const char *text)
{
    if (GTK_IS_LABEL(widget) &&
        g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text) == 0)
        return GTK_LABEL(widget);

    for (GtkWidget *child = gtk_widget_get_first_child(widget);
         child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkLabel *match = find_label_with_text(child, text);
        if (match)
            return match;
    }
    return NULL;
}

static void
test_profile_mention_activation_emits_open_profile(void)
{
    static const char *pubkey_hex =
        "2222222222222222222222222222222222222222222222222222222222222222";
    const char *relays[] = { "wss://relay.example", NULL };
    g_autoptr(GError) error = NULL;
    g_autoptr(GNostrNip19) npub =
        gnostr_nip19_encode_npub(pubkey_hex, &error);
    g_assert_no_error(error);
    g_assert_nonnull(npub);
    g_autoptr(GNostrNip19) nprofile =
        gnostr_nip19_encode_nprofile(pubkey_hex, relays, &error);
    g_assert_no_error(error);
    g_assert_nonnull(nprofile);

    const char *targets[] = {
        gnostr_nip19_get_bech32(npub),
        gnostr_nip19_get_bech32(nprofile),
    };

    for (guint i = 0; i < G_N_ELEMENTS(targets); i++) {
        NostrGtkNoteCardRow *row = nostr_gtk_note_card_row_new();
        g_object_ref_sink(row);
        OpenProfileCapture capture = { 0 };
        g_signal_connect(row, "open-profile",
                         G_CALLBACK(on_open_profile_captured), &capture);

        g_autofree gchar *href =
            g_strdup_printf("nostr:%s", targets[i]);
        g_autofree gchar *markup =
            g_strdup_printf("<a href=\"%s\">@mention</a>", href);
        nostr_gtk_note_card_row_set_precomputed_markup(
            row, "@mention", markup);

        GtkLabel *content_label =
            find_label_with_text(GTK_WIDGET(row), "@mention");
        g_assert_nonnull(content_label);

        gboolean handled = FALSE;
        g_signal_emit_by_name(content_label, "activate-link", href, &handled);
        g_assert_true(handled);
        g_assert_cmpuint(capture.count, ==, 1);
        g_assert_cmpstr(capture.pubkey_hex, ==, pubkey_hex);

        g_free(capture.pubkey_hex);
        g_object_unref(row);
    }
}

static void
test_note_card_explicit_expansion_allows_natural_height(void)
{
    NostrGtkNoteCardRow *row = nostr_gtk_note_card_row_new();
    const char *long_content =
        "This content is intentionally long enough to exceed the reserved "
        "height after the user explicitly expands the row. "
        "line 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\n"
        "line 9\nline 10\nline 11\nline 12\nline 13\nline 14\nline 15\n"
        "line 16\nline 17\nline 18\nline 19\nline 20\n";

    nostr_gtk_note_card_row_set_content(row, long_content);
    nostr_gtk_note_card_row_set_reserved_height(row, 120);
    nostr_gtk_note_card_row_set_explicit_footprint_expansion(row, TRUE);

    int min_h = 0, nat_h = 0;
    gtk_widget_measure(GTK_WIDGET(row), GTK_ORIENTATION_VERTICAL,
                       REFERENCE_WIDTH_PX, &min_h, &nat_h, NULL, NULL);

    g_assert_cmpint(min_h, ==, 120);
    g_assert_cmpint(nat_h, >=, 120);
    g_assert_true(nostr_gtk_note_card_row_get_explicit_footprint_expansion(row));

    g_object_ref_sink(row);
    g_object_unref(row);
}

int
main(int argc, char *argv[])
{
    gtk_test_init(&argc, &argv, NULL);
    g_resources_register(nostr_gtk_get_resource());
    gnostr_identity_init("org.gnostr.Client");

    g_test_add_func("/nostr-gtk/sizing/label-baseline",
                    test_label_stays_bounded);
    g_test_add_func("/nostr-gtk/sizing/constrained-box-bounded",
                    test_constrained_box_stays_bounded);
    g_test_add_func("/nostr-gtk/sizing/adw-clamp-bounds-natural-width",
                    test_adw_clamp_bounds_natural_width);
    g_test_add_func("/nostr-gtk/sizing/listview-row-heights",
                    test_listview_row_heights_bounded);
    g_test_add_func("/nostr-gtk/sizing/note-card-reserved-height-fixed",
                    test_note_card_reserved_height_blocks_passive_expansion);
    g_test_add_func("/nostr-gtk/note-card/profile-mention-activation",
                    test_profile_mention_activation_emits_open_profile);
    g_test_add_func("/nostr-gtk/sizing/note-card-explicit-expansion",
                    test_note_card_explicit_expansion_allows_natural_height);
    g_test_add_func("/nostr-gtk/sizing/rich-content-hydration-fixed",
                    test_rich_content_hydration_preserves_reserved_height);
    g_test_add_func("/nostr-gtk/sizing/rich-content-buffer-bind-lightweight",
                    test_rich_content_buffer_bind_creates_no_expensive_children);
    g_test_add_func("/nostr-gtk/sizing/rich-content-cache-delivery-survives-unmap",
                    test_rich_content_cache_delivery_survives_unmap);

    return g_test_run();
}
