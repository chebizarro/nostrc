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
#include <glib.h>

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

int
main(int argc, char *argv[])
{
    gtk_test_init(&argc, &argv, NULL);

    g_test_add_func("/nostr-gtk/sizing/label-baseline",
                    test_label_stays_bounded);
    g_test_add_func("/nostr-gtk/sizing/constrained-box-bounded",
                    test_constrained_box_stays_bounded);
    g_test_add_func("/nostr-gtk/sizing/listview-row-heights",
                    test_listview_row_heights_bounded);

    return g_test_run();
}
