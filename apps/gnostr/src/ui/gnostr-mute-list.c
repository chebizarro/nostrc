/**
 * GNostrMuteList - Mute List Editor Dialog Implementation
 *
 * Provides a tabbed interface for managing muted users, words, and hashtags.
 * Integrates with the mute_list service for persistence.
 */

#include "gnostr-mute-list.h"
#include <nostr-gobject-1.0/gnostr-mute-list.h>
#include "../ipc/signer_ipc.h"
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <glib.h>
#include <glib/gi18n.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/dialogs/gnostr-mute-list.ui"

struct _GNostrMuteListDialog {
    GtkWindow parent_instance;

    /* Template children */
    GtkWidget *btn_close;
    GtkWidget *btn_save;
    GtkWidget *spinner;
    GtkWidget *toast_revealer;
    GtkWidget *toast_label;
    GtkWidget *notebook;
    GtkWidget *entry_add_user;
    GtkWidget *btn_add_user;
    GtkWidget *list_users;
    GtkWidget *entry_add_word;
    GtkWidget *btn_add_word;
    GtkWidget *list_words;
    GtkWidget *entry_add_hashtag;
    GtkWidget *btn_add_hashtag;
    GtkWidget *list_hashtags;

    /* State */
    gboolean saving;
};

G_DEFINE_TYPE(GNostrMuteListDialog, gnostr_mute_list_dialog, GTK_TYPE_WINDOW)

/* Forward declarations */
static void on_close_clicked(GtkButton *btn, gpointer user_data);
static void on_save_clicked(GtkButton *btn, gpointer user_data);
static void on_add_user_clicked(GtkButton *btn, gpointer user_data);
static void on_add_word_clicked(GtkButton *btn, gpointer user_data);
static void on_add_hashtag_clicked(GtkButton *btn, gpointer user_data);
static void refresh_all_lists(GNostrMuteListDialog *self);
static void show_toast(GNostrMuteListDialog *self, const char *msg);
static void update_save_button(GNostrMuteListDialog *self);

static void gnostr_mute_list_dialog_dispose(GObject *obj) {
    GNostrMuteListDialog *self = GNOSTR_MUTE_LIST(obj);
    gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_MUTE_LIST);
    G_OBJECT_CLASS(gnostr_mute_list_dialog_parent_class)->dispose(obj);
}

static void gnostr_mute_list_dialog_finalize(GObject *obj) {
    G_OBJECT_CLASS(gnostr_mute_list_dialog_parent_class)->finalize(obj);
}

static void gnostr_mute_list_dialog_class_init(GNostrMuteListDialogClass *klass) {
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
    GObjectClass *gclass = G_OBJECT_CLASS(klass);

    gclass->dispose = gnostr_mute_list_dialog_dispose;
    gclass->finalize = gnostr_mute_list_dialog_finalize;

    gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);

    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, btn_close);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, btn_save);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, spinner);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, toast_revealer);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, toast_label);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, notebook);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, entry_add_user);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, btn_add_user);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, list_users);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, entry_add_word);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, btn_add_word);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, list_words);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, entry_add_hashtag);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, btn_add_hashtag);
    gtk_widget_class_bind_template_child(wclass, GNostrMuteListDialog, list_hashtags);

    gtk_widget_class_bind_template_callback(wclass, on_close_clicked);
    gtk_widget_class_bind_template_callback(wclass, on_save_clicked);
    gtk_widget_class_bind_template_callback(wclass, on_add_user_clicked);
    gtk_widget_class_bind_template_callback(wclass, on_add_word_clicked);
    gtk_widget_class_bind_template_callback(wclass, on_add_hashtag_clicked);
}

static void gnostr_mute_list_dialog_init(GNostrMuteListDialog *self) {
    gtk_widget_init_template(GTK_WIDGET(self));
    self->saving = FALSE;

    /* Connect signals (fallback if template callbacks don't work) */
    g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close_clicked), self);
    g_signal_connect(self->btn_save, "clicked", G_CALLBACK(on_save_clicked), self);
    g_signal_connect(self->btn_add_user, "clicked", G_CALLBACK(on_add_user_clicked), self);
    g_signal_connect(self->btn_add_word, "clicked", G_CALLBACK(on_add_word_clicked), self);
    g_signal_connect(self->btn_add_hashtag, "clicked", G_CALLBACK(on_add_hashtag_clicked), self);

    /* Initial refresh */
    refresh_all_lists(self);
}

GNostrMuteListDialog *gnostr_mute_list_dialog_new(GtkWindow *parent) {
    GNostrMuteListDialog *self = g_object_new(GNOSTR_TYPE_MUTE_LIST,
                                               "transient-for", parent,
                                               "modal", TRUE,
                                               NULL);
    return self;
}

/* ---- Helper Functions ---- */

static gboolean hide_toast_timeout_cb(gpointer user_data) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(user_data), FALSE);
    return G_SOURCE_REMOVE;
}

static void show_toast(GNostrMuteListDialog *self, const char *msg) {
    if (!self->toast_label || !self->toast_revealer) return;
    gtk_label_set_text(GTK_LABEL(self->toast_label), msg);
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), TRUE);
    /* Auto-hide after 3 seconds */
    g_timeout_add_full(G_PRIORITY_DEFAULT, 3000, hide_toast_timeout_cb,
                       g_object_ref(self->toast_revealer), g_object_unref);
}

static void set_ui_sensitive(GNostrMuteListDialog *self, gboolean sensitive) {
    gtk_widget_set_sensitive(self->entry_add_user, sensitive);
    gtk_widget_set_sensitive(self->btn_add_user, sensitive);
    gtk_widget_set_sensitive(self->entry_add_word, sensitive);
    gtk_widget_set_sensitive(self->btn_add_word, sensitive);
    gtk_widget_set_sensitive(self->entry_add_hashtag, sensitive);
    gtk_widget_set_sensitive(self->btn_add_hashtag, sensitive);
    gtk_widget_set_sensitive(self->btn_save, sensitive && gnostr_mute_list_is_dirty(gnostr_mute_list_get_default()));
    gtk_widget_set_sensitive(self->btn_close, sensitive);

    if (self->spinner) {
        gtk_widget_set_visible(self->spinner, !sensitive);
        if (!sensitive) {
            gtk_spinner_start(GTK_SPINNER(self->spinner));
        } else {
            gtk_spinner_stop(GTK_SPINNER(self->spinner));
        }
    }
}

static void update_save_button(GNostrMuteListDialog *self) {
    gboolean dirty = gnostr_mute_list_is_dirty(gnostr_mute_list_get_default());
    gtk_widget_set_sensitive(self->btn_save, dirty);
}

/* ---- List Row Creation ---- */

typedef struct {
    GNostrMuteListDialog *dialog;
    char *value;
    int type; /* 0=user, 1=word, 2=hashtag */
} RowData;

static void row_data_free(gpointer data) {
    RowData *rd = (RowData *)data;
    if (rd) {
        g_free(rd->value);
        g_free(rd);
    }
}

static void on_remove_row_clicked(GtkButton *btn, gpointer user_data) {
    RowData *rd = (RowData *)user_data;
    if (!rd || !rd->dialog) return;

    GNostrMuteList *mute_list = gnostr_mute_list_get_default();

    switch (rd->type) {
        case 0: /* User */
            gnostr_mute_list_remove_pubkey(mute_list, rd->value);
            break;
        case 1: /* Word */
            gnostr_mute_list_remove_word(mute_list, rd->value);
            break;
        case 2: /* Hashtag */
            gnostr_mute_list_remove_hashtag(mute_list, rd->value);
            break;
    }

    refresh_all_lists(rd->dialog);
    update_save_button(rd->dialog);
    (void)btn;
}

static GtkWidget *create_list_row(GNostrMuteListDialog *self,
                                   const char *value,
                                   int type) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);

    /* Value label */
    GtkWidget *label = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);

    /* Remove button */
    GtkWidget *remove_btn = gtk_button_new_from_icon_name("list-remove-symbolic");
    gtk_widget_add_css_class(remove_btn, "flat");
    gtk_widget_set_tooltip_text(remove_btn, "Remove from mute list");
    gtk_box_append(GTK_BOX(box), remove_btn);

    /* Store data for callback */
    RowData *rd = g_new0(RowData, 1);
    rd->dialog = self;
    rd->value = g_strdup(value);
    rd->type = type;

    g_signal_connect_data(remove_btn, "clicked",
                          G_CALLBACK(on_remove_row_clicked),
                          rd, (GClosureNotify)row_data_free, 0);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    return row;
}

/* ---- List Refresh ---- */

static void clear_list_box(GtkListBox *list_box) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list_box));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(list_box, child);
        child = next;
    }
}

static void refresh_users_list(GNostrMuteListDialog *self) {
    clear_list_box(GTK_LIST_BOX(self->list_users));

    GNostrMuteList *mute_list = gnostr_mute_list_get_default();
    size_t count = 0;
    const char **pubkeys = gnostr_mute_list_get_pubkeys(mute_list, &count);

    for (size_t i = 0; i < count; i++) {
        /* Try to convert to npub for display */
        char *npub = NULL;
        gboolean converted = FALSE;

        if (pubkeys[i] && strlen(pubkeys[i]) == 64) {
            g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_npub(pubkeys[i], NULL);
            if (n19) {
                npub = g_strdup(gnostr_nip19_get_bech32(n19));
                converted = TRUE;
            }
        }

        const char *display = converted ? npub : pubkeys[i];
        GtkWidget *row = create_list_row(self, display, 0);
        gtk_list_box_append(GTK_LIST_BOX(self->list_users), row);

        g_free(npub);
    }

    g_free((void *)pubkeys);
}

static void refresh_words_list(GNostrMuteListDialog *self) {
    clear_list_box(GTK_LIST_BOX(self->list_words));

    GNostrMuteList *mute_list = gnostr_mute_list_get_default();
    size_t count = 0;
    const char **words = gnostr_mute_list_get_words(mute_list, &count);

    for (size_t i = 0; i < count; i++) {
        GtkWidget *row = create_list_row(self, words[i], 1);
        gtk_list_box_append(GTK_LIST_BOX(self->list_words), row);
    }

    g_free((void *)words);
}

static void refresh_hashtags_list(GNostrMuteListDialog *self) {
    clear_list_box(GTK_LIST_BOX(self->list_hashtags));

    GNostrMuteList *mute_list = gnostr_mute_list_get_default();
    size_t count = 0;
    const char **hashtags = gnostr_mute_list_get_hashtags(mute_list, &count);

    for (size_t i = 0; i < count; i++) {
        /* Display with # prefix */
        g_autofree char *display = g_strdup_printf("#%s", hashtags[i]);
        GtkWidget *row = create_list_row(self, display, 2);
        gtk_list_box_append(GTK_LIST_BOX(self->list_hashtags), row);
    }

    g_free((void *)hashtags);
}

static void refresh_all_lists(GNostrMuteListDialog *self) {
    refresh_users_list(self);
    refresh_words_list(self);
    refresh_hashtags_list(self);
    update_save_button(self);
}

void gnostr_mute_list_dialog_refresh(GNostrMuteListDialog *self) {
    g_return_if_fail(GNOSTR_IS_MUTE_LIST(self));
    refresh_all_lists(self);
}

/* ---- Button Handlers ---- */

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GNostrMuteListDialog *self = GNOSTR_MUTE_LIST(user_data);
    gtk_window_close(GTK_WINDOW(self));
}

static void on_save_complete(GNostrMuteList *mute_list,
                              gboolean success,
                              const char *error_msg,
                              gpointer user_data) {
    (void)mute_list;
    GNostrMuteListDialog *self = GNOSTR_MUTE_LIST(user_data);
    if (!GNOSTR_IS_MUTE_LIST(self)) return;

    self->saving = FALSE;
    set_ui_sensitive(self, TRUE);

    if (success) {
        show_toast(self, "Mute list saved!");
    } else {
        g_autofree char *msg = g_strdup_printf("Save failed: %s", error_msg ? error_msg : "unknown error");
        show_toast(self, msg);
    }

    update_save_button(self);
}

static void on_save_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GNostrMuteListDialog *self = GNOSTR_MUTE_LIST(user_data);

    if (self->saving) return;

    self->saving = TRUE;
    set_ui_sensitive(self, FALSE);
    show_toast(self, "Saving mute list...");

    gnostr_mute_list_save_async(gnostr_mute_list_get_default(),
                                 on_save_complete,
                                 self);
}

static void on_add_user_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GNostrMuteListDialog *self = GNOSTR_MUTE_LIST(user_data);

    const char *input = gtk_editable_get_text(GTK_EDITABLE(self->entry_add_user));
    if (!input || !*input) return;

    char pubkey_hex[65] = {0};
    gboolean valid = FALSE;

    /* Try to parse as npub */
    if (g_str_has_prefix(input, "npub1")) {
        g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(input, NULL);
        if (n19) {
            g_strlcpy(pubkey_hex, gnostr_nip19_get_pubkey(n19), sizeof(pubkey_hex));
            valid = TRUE;
        }
    }
    /* Or check if it's already hex */
    else if (strlen(input) == 64) {
        gboolean all_hex = TRUE;
        for (int i = 0; i < 64 && all_hex; i++) {
            if (!g_ascii_isxdigit(input[i])) all_hex = FALSE;
        }
        if (all_hex) {
            g_strlcpy(pubkey_hex, input, sizeof(pubkey_hex));
            valid = TRUE;
        }
    }

    if (!valid) {
        show_toast(self, "Invalid pubkey. Enter npub or 64-character hex.");
        return;
    }

    gnostr_mute_list_add_pubkey(gnostr_mute_list_get_default(), pubkey_hex, FALSE);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_add_user), "");
    refresh_all_lists(self);
    show_toast(self, "User added to mute list");
}

static void on_add_word_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GNostrMuteListDialog *self = GNOSTR_MUTE_LIST(user_data);

    const char *word = gtk_editable_get_text(GTK_EDITABLE(self->entry_add_word));
    if (!word || !*word) return;

    gnostr_mute_list_add_word(gnostr_mute_list_get_default(), word, FALSE);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_add_word), "");
    refresh_all_lists(self);
    show_toast(self, "Word added to mute list");
}

static void on_add_hashtag_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GNostrMuteListDialog *self = GNOSTR_MUTE_LIST(user_data);

    const char *hashtag = gtk_editable_get_text(GTK_EDITABLE(self->entry_add_hashtag));
    if (!hashtag || !*hashtag) return;

    gnostr_mute_list_add_hashtag(gnostr_mute_list_get_default(), hashtag, FALSE);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_add_hashtag), "");
    refresh_all_lists(self);
    show_toast(self, "Hashtag added to mute list");
}

/* ---- Public API ---- */

void gnostr_mute_list_dialog_add_pubkey(GNostrMuteListDialog *self,
                                         const char *pubkey_hex) {
    g_return_if_fail(GNOSTR_IS_MUTE_LIST(self));
    if (!pubkey_hex || strlen(pubkey_hex) != 64) return;

    gnostr_mute_list_add_pubkey(gnostr_mute_list_get_default(), pubkey_hex, FALSE);
    refresh_all_lists(self);
    show_toast(self, "User added to mute list");

    /* Switch to Users tab */
    gtk_notebook_set_current_page(GTK_NOTEBOOK(self->notebook), 0);
}
