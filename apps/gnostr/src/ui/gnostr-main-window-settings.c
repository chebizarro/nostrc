/**
 * gnostr-main-window-settings.c — Settings Dialog
 *
 * Extracted from gnostr-main-window.c. Contains the settings/preferences dialog
 * with all panel setup: General, Display, Account, Relay, Index Relay (NIP-50),
 * Blossom Server, Media, Notifications, Metrics, NIP-47 Wallet, and Plugin Manager.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-main-window-private.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/gnostr-sync-service.h>
#include <nostr/metrics_collector.h>
#include <nostr/metrics_schema.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include "../util/blossom_settings.h"
#include "../util/nip51_settings.h"
#include "../util/nwc.h"
#include "gnostr-nwc-connect.h"
#include "gnostr-plugin-manager-panel.h"
#include "gnostr-mute-row-data.h"
#include <nostr-gobject-1.0/gnostr-mute-list.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <gtk/gtk.h>
#include <libadwaita-1/adwaita.h>
#include <string.h>

/* Use show_toast from main window */
#define show_toast(self, msg) gnostr_main_window_show_toast_internal(self, msg)

/* Settings dialog context for callbacks */
typedef struct {
  GtkWindow *win;
  GtkBuilder *builder;
  GnostrMainWindow *main_window;
  GSettings *client_settings;  /* Cached org.gnostr.Client settings */
  GSettings *notif_settings;   /* Cached org.gnostr.Notifications settings */
} SettingsDialogCtx;

static void settings_dialog_ctx_free(SettingsDialogCtx *ctx) {
  if (!ctx) return;
  if (ctx->builder) g_object_unref(ctx->builder);
  g_clear_object(&ctx->client_settings);
  g_clear_object(&ctx->notif_settings);
  g_free(ctx);
}

/* Callback for NIP-51 backup button */
/* hq-cnkj3: Negentropy background sync toggle changed */
static void on_negentropy_sync_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  (void)user_data;
  gboolean enabled = gtk_switch_get_active(sw);

  g_autoptr(GSettings) client = g_settings_new("org.gnostr.Client");
  g_settings_set_boolean(client, "negentropy-auto-sync", enabled);

  GNostrSyncService *svc = gnostr_sync_service_get_default();
  if (enabled)
    gnostr_sync_service_start(svc);
  else
    gnostr_sync_service_stop(svc);
}

static void on_nip51_backup_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  if (!ctx || !ctx->main_window) return;
  show_toast(ctx->main_window, "Backing up settings to relays...");
  gnostr_nip51_settings_backup_async(NULL, NULL);
}

/* Callback for NIP-51 restore button */
static void on_nip51_restore_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  if (!ctx || !ctx->main_window) return;

  const gchar *pubkey = ctx->main_window->user_pubkey_hex;
  if (!pubkey || !*pubkey) {
    show_toast(ctx->main_window, "Sign in to restore settings");
    return;
  }
  show_toast(ctx->main_window, "Restoring settings from relays...");
  gnostr_nip51_settings_load_async(pubkey, NULL, NULL);
}

/* Forward declaration for background mode callback */
static void on_background_mode_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);

/* nostrc-61s.6: Setup general settings panel (background mode) */
static void settings_dialog_setup_general_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  g_autoptr(GSettings) client_settings = g_settings_new("org.gnostr.Client");
  if (!client_settings) return;

  /* Background mode switch */
  GtkSwitch *w_background_mode = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_background_mode"));
  if (w_background_mode) {
    gtk_switch_set_active(w_background_mode, g_settings_get_boolean(client_settings, "background-mode"));
    g_signal_connect(w_background_mode, "notify::active", G_CALLBACK(on_background_mode_changed), ctx);
  }

}

/* Update Display settings panel from GSettings */
static void settings_dialog_setup_display_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  g_autoptr(GSettings) display_settings = g_settings_new("org.gnostr.Display");
  if (!display_settings) return;

  /* Color scheme dropdown (System=0, Light=1, Dark=2) */
  GtkDropDown *w_color_scheme = GTK_DROP_DOWN(gtk_builder_get_object(ctx->builder, "w_color_scheme"));
  if (w_color_scheme) {
    g_autofree gchar *scheme = g_settings_get_string(display_settings, "color-scheme");
    guint idx = 0;
    if (g_strcmp0(scheme, "light") == 0) idx = 1;
    else if (g_strcmp0(scheme, "dark") == 0) idx = 2;
    gtk_drop_down_set_selected(w_color_scheme, idx);
  }

  /* Font scale slider */
  GtkScale *w_font_scale = GTK_SCALE(gtk_builder_get_object(ctx->builder, "w_font_scale"));
  if (w_font_scale) {
    gdouble scale = g_settings_get_double(display_settings, "font-scale");
    gtk_range_set_value(GTK_RANGE(w_font_scale), scale);
  }

  /* Timeline density dropdown (Compact=0, Normal=1, Comfortable=2) */
  GtkDropDown *w_density = GTK_DROP_DOWN(gtk_builder_get_object(ctx->builder, "w_timeline_density"));
  if (w_density) {
    g_autofree gchar *density = g_settings_get_string(display_settings, "timeline-density");
    guint idx = 1;
    if (g_strcmp0(density, "compact") == 0) idx = 0;
    else if (g_strcmp0(density, "comfortable") == 0) idx = 2;
    gtk_drop_down_set_selected(w_density, idx);
  }

  /* Boolean switches */
  GtkSwitch *w_avatars = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_show_avatars"));
  if (w_avatars) gtk_switch_set_active(w_avatars, g_settings_get_boolean(display_settings, "show-avatars"));

  GtkSwitch *w_media = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_show_media_previews"));
  if (w_media) gtk_switch_set_active(w_media, g_settings_get_boolean(display_settings, "show-media-previews"));

  GtkSwitch *w_anim = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_enable_animations"));
  if (w_anim) gtk_switch_set_active(w_anim, g_settings_get_boolean(display_settings, "enable-animations"));

}

/* Update Account settings panel */
static void settings_dialog_setup_account_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder || !ctx->main_window) return;

  /* Check login state from GSettings for consistency with update_login_ui_state() */
  GSettings *acct_settings = g_settings_new("org.gnostr.Client");
  char *acct_npub = acct_settings ? g_settings_get_string(acct_settings, "current-npub") : NULL;
  gboolean is_logged_in = (acct_npub && *acct_npub);
  g_free(acct_npub);
  if (acct_settings) g_object_unref(acct_settings);

  /* Toggle login required / account content visibility */
  GtkWidget *account_login_required = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "account_login_required"));
  GtkWidget *account_content = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "account_content"));
  if (account_login_required) gtk_widget_set_visible(account_login_required, !is_logged_in);
  if (account_content) gtk_widget_set_visible(account_content, is_logged_in);

  /* NIP-51 sync enabled switch */
  GtkSwitch *w_sync = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_nip51_sync_enabled"));
  if (w_sync) gtk_switch_set_active(w_sync, gnostr_nip51_settings_sync_enabled());

  /* Last sync label */
  GtkLabel *lbl_sync = GTK_LABEL(gtk_builder_get_object(ctx->builder, "lbl_nip51_last_sync"));
  if (lbl_sync) {
    gint64 last_sync = gnostr_nip51_settings_last_sync();
    if (last_sync > 0) {
      GDateTime *dt = g_date_time_new_from_unix_local(last_sync);
      if (dt) {
        gchar *formatted = g_date_time_format(dt, "%Y-%m-%d %H:%M");
        gtk_label_set_text(lbl_sync, formatted);
        g_free(formatted);
        g_date_time_unref(dt);
      }
    } else {
      gtk_label_set_text(lbl_sync, "Never");
    }
  }

  /* Connect backup/restore buttons */
  GtkButton *btn_backup = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_nip51_backup"));
  GtkButton *btn_restore = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_nip51_restore"));
  if (btn_backup) g_signal_connect(btn_backup, "clicked", G_CALLBACK(on_nip51_backup_clicked), ctx);
  if (btn_restore) g_signal_connect(btn_restore, "clicked", G_CALLBACK(on_nip51_restore_clicked), ctx);

  /* hq-cnkj3: Negentropy background sync toggle */
  GtkSwitch *w_neg = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_negentropy_sync_enabled"));
  if (w_neg) {
    g_autoptr(GSettings) client = g_settings_new("org.gnostr.Client");
    gtk_switch_set_active(w_neg, g_settings_get_boolean(client, "negentropy-auto-sync"));
    g_signal_connect(w_neg, "notify::active", G_CALLBACK(on_negentropy_sync_changed), NULL);
  }
}

/* Populate the relay list in settings */
static void settings_dialog_setup_relay_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkListBox *list_relays = GTK_LIST_BOX(gtk_builder_get_object(ctx->builder, "list_relays"));
  if (!list_relays) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list_relays))) != NULL) {
    gtk_list_box_remove(list_relays, child);
  }

  /* Load relays */
  GPtrArray *relays = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relays);

  for (guint i = 0; i < relays->len; i++) {
    const gchar *url = g_ptr_array_index(relays, i);

    /* Create row with relay URL */
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);

    /* URL label */
    GtkWidget *label = gtk_label_new(url);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(box), label);

    /* Type dropdown (R+W, Read, Write) */
    const gchar *types[] = {"R+W", "Read", "Write", NULL};
    GtkWidget *type_dd = gtk_drop_down_new_from_strings(types);
    gtk_widget_set_valign(type_dd, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), type_dd);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(list_relays, row);
  }

  g_ptr_array_unref(relays);
}

/* ---- Index Relay Panel (NIP-50 Search) ---- */

/* Context for index relay row */
typedef struct {
  SettingsDialogCtx *dialog_ctx;
  gsize relay_index;
  char *relay_url;
} IndexRelayRowCtx;

static void index_relay_row_ctx_free(gpointer data) {
  IndexRelayRowCtx *ctx = (IndexRelayRowCtx *)data;
  if (!ctx) return;
  g_free(ctx->relay_url);
  g_free(ctx);
}

/* Forward declare refresh function */
static void settings_dialog_refresh_index_relay_list(SettingsDialogCtx *ctx);

/* Callback for remove button on an index relay row */
static void on_index_relay_remove(GtkButton *btn, gpointer user_data) {
  (void)btn;
  IndexRelayRowCtx *row_ctx = (IndexRelayRowCtx *)user_data;
  if (!row_ctx || !row_ctx->dialog_ctx) return;

  /* Load current index relays from GSettings */
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.gnostr");
  if (!settings) return;

  gchar **relays = g_settings_get_strv(settings, "index-relays");
  if (!relays) {
    return;
  }

  /* Count relays and build new array without the removed one */
  gsize count = g_strv_length(relays);
  GPtrArray *new_relays = g_ptr_array_new();

  for (gsize i = 0; i < count; i++) {
    if (g_strcmp0(relays[i], row_ctx->relay_url) != 0) {
      g_ptr_array_add(new_relays, relays[i]);
    }
  }
  g_ptr_array_add(new_relays, NULL);

  g_settings_set_strv(settings, "index-relays", (const gchar *const *)new_relays->pdata);

  g_ptr_array_free(new_relays, TRUE);
  g_strfreev(relays);

  settings_dialog_refresh_index_relay_list(row_ctx->dialog_ctx);

  if (row_ctx->dialog_ctx->main_window) {
    show_toast(row_ctx->dialog_ctx->main_window, "Index relay removed");
  }
}

/* Callback for add index relay button */
static void on_index_relay_add(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  if (!ctx || !ctx->builder) return;

  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(ctx->builder, "entry_index_relay"));
  if (!entry) return;

  GtkEntryBuffer *buffer = gtk_entry_get_buffer(entry);
  const char *url = gtk_entry_buffer_get_text(buffer);
  if (!url || !*url) {
    if (ctx->main_window) show_toast(ctx->main_window, "Enter a relay URL");
    return;
  }

  /* Validate URL starts with wss:// or ws:// */
  if (!g_str_has_prefix(url, "wss://") && !g_str_has_prefix(url, "ws://")) {
    if (ctx->main_window) show_toast(ctx->main_window, "URL must start with wss:// or ws://");
    return;
  }

  /* Load current index relays from GSettings */
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.gnostr");
  if (!settings) return;

  gchar **relays = g_settings_get_strv(settings, "index-relays");

  /* Check if relay already exists */
  gboolean exists = FALSE;
  if (relays) {
    for (gsize i = 0; relays[i] != NULL; i++) {
      if (g_strcmp0(relays[i], url) == 0) {
        exists = TRUE;
        break;
      }
    }
  }

  if (exists) {
    g_strfreev(relays);
    if (ctx->main_window) show_toast(ctx->main_window, "Relay already in list");
    return;
  }

  /* Add new relay to array */
  gsize count = relays ? g_strv_length(relays) : 0;
  gchar **new_relays = g_new0(gchar *, count + 2);
  for (gsize i = 0; i < count; i++) {
    new_relays[i] = g_strdup(relays[i]);
  }
  new_relays[count] = g_strdup(url);
  new_relays[count + 1] = NULL;

  g_settings_set_strv(settings, "index-relays", (const gchar *const *)new_relays);

  g_strfreev(new_relays);
  g_strfreev(relays);

  gtk_entry_buffer_set_text(buffer, "", 0);
  settings_dialog_refresh_index_relay_list(ctx);
  if (ctx->main_window) show_toast(ctx->main_window, "Index relay added");
}

/* Refresh the index relay list in settings */
static void settings_dialog_refresh_index_relay_list(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkListBox *list = GTK_LIST_BOX(gtk_builder_get_object(ctx->builder, "list_index_relays"));
  if (!list) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL) {
    gtk_list_box_remove(list, child);
  }

  /* Load index relays from GSettings */
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.gnostr");
  if (!settings) return;

  gchar **relays = g_settings_get_strv(settings, "index-relays");

  if (!relays) return;

  for (gsize i = 0; relays[i] != NULL; i++) {
    /* Create row context */
    IndexRelayRowCtx *row_ctx = g_new0(IndexRelayRowCtx, 1);
    row_ctx->dialog_ctx = ctx;
    row_ctx->relay_index = i;
    row_ctx->relay_url = g_strdup(relays[i]);

    /* Create row widget */
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* URL label */
    GtkWidget *label = gtk_label_new(relays[i]);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(box), label);

    /* Remove button */
    GtkWidget *btn_remove = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(btn_remove, "flat");
    gtk_widget_add_css_class(btn_remove, "error");
    gtk_widget_set_tooltip_text(btn_remove, "Remove relay");
    g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_index_relay_remove), row_ctx);
    gtk_box_append(GTK_BOX(box), btn_remove);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(list, row);

    /* Free row context when row is destroyed */
    g_object_set_data_full(G_OBJECT(row), "row-ctx", row_ctx, index_relay_row_ctx_free);
  }

  g_strfreev(relays);
}

/* Setup index relay panel */
static void settings_dialog_setup_index_relay_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  /* Connect add button */
  GtkButton *btn_add = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_add_index_relay"));
  if (btn_add) {
    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_index_relay_add), ctx);
  }

  /* Allow Enter key in entry to add relay */
  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(ctx->builder, "entry_index_relay"));
  if (entry) {
    g_signal_connect_swapped(entry, "activate", G_CALLBACK(on_index_relay_add), ctx);
  }

  /* Populate relay list */
  settings_dialog_refresh_index_relay_list(ctx);
}

/* ---- Blossom Server Panel ---- */

/* Context for blossom server row */
typedef struct {
  SettingsDialogCtx *dialog_ctx;
  gsize server_index;
  char *server_url;
} BlossomServerRowCtx;

static void blossom_server_row_ctx_free(gpointer data) {
  BlossomServerRowCtx *ctx = (BlossomServerRowCtx *)data;
  if (!ctx) return;
  g_free(ctx->server_url);
  g_free(ctx);
}

/* Refresh the blossom server list */
static void settings_dialog_refresh_blossom_list(SettingsDialogCtx *ctx);

/* Callback for remove button on a blossom server row */
static void on_blossom_server_remove(GtkButton *btn, gpointer user_data) {
  (void)btn;
  BlossomServerRowCtx *row_ctx = (BlossomServerRowCtx *)user_data;
  if (!row_ctx || !row_ctx->dialog_ctx) return;

  gnostr_blossom_settings_remove_server(row_ctx->server_url);
  settings_dialog_refresh_blossom_list(row_ctx->dialog_ctx);

  if (row_ctx->dialog_ctx->main_window) {
    show_toast(row_ctx->dialog_ctx->main_window, "Server removed");
  }
}

/* Callback for move up button on a blossom server row */
static void on_blossom_server_move_up(GtkButton *btn, gpointer user_data) {
  (void)btn;
  BlossomServerRowCtx *row_ctx = (BlossomServerRowCtx *)user_data;
  if (!row_ctx || !row_ctx->dialog_ctx || row_ctx->server_index == 0) return;

  gnostr_blossom_settings_reorder_server(row_ctx->server_index, row_ctx->server_index - 1);
  settings_dialog_refresh_blossom_list(row_ctx->dialog_ctx);
}

/* Callback for move down button on a blossom server row */
static void on_blossom_server_move_down(GtkButton *btn, gpointer user_data) {
  (void)btn;
  BlossomServerRowCtx *row_ctx = (BlossomServerRowCtx *)user_data;
  if (!row_ctx || !row_ctx->dialog_ctx) return;

  gsize count = gnostr_blossom_settings_get_server_count();
  if (row_ctx->server_index >= count - 1) return;

  gnostr_blossom_settings_reorder_server(row_ctx->server_index, row_ctx->server_index + 1);
  settings_dialog_refresh_blossom_list(row_ctx->dialog_ctx);
}

/* Callback for add server button */
static void on_blossom_add_server(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  if (!ctx || !ctx->builder) return;

  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(ctx->builder, "w_blossom_server"));
  if (!entry) return;

  GtkEntryBuffer *buffer = gtk_entry_get_buffer(entry);
  const char *url = gtk_entry_buffer_get_text(buffer);
  if (!url || !*url) {
    if (ctx->main_window) show_toast(ctx->main_window, "Enter a server URL");
    return;
  }

  /* Validate URL starts with https:// */
  if (!g_str_has_prefix(url, "https://") && !g_str_has_prefix(url, "http://")) {
    if (ctx->main_window) show_toast(ctx->main_window, "URL must start with https://");
    return;
  }

  if (gnostr_blossom_settings_add_server(url)) {
    gtk_entry_buffer_set_text(buffer, "", 0);
    settings_dialog_refresh_blossom_list(ctx);
    if (ctx->main_window) show_toast(ctx->main_window, "Server added");
  } else {
    if (ctx->main_window) show_toast(ctx->main_window, "Server already exists");
  }
}

/* Callback for publish button (kind 10063) */
static void on_blossom_publish_complete(gboolean success, GError *error, gpointer user_data) {
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  if (!ctx || !ctx->main_window) return;

  if (success) {
    show_toast(ctx->main_window, "Server list published to relays");
  } else {
    g_autofree char *msg = g_strdup_printf("Publish failed: %s",
                                 error ? error->message : "unknown error");
    show_toast(ctx->main_window, msg);
  }
}

static void on_blossom_publish_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  if (!ctx || !ctx->main_window) return;

  /* Check if logged in */
  if (!ctx->main_window->user_pubkey_hex || !ctx->main_window->user_pubkey_hex[0]) {
    show_toast(ctx->main_window, "Sign in to publish server list");
    return;
  }

  show_toast(ctx->main_window, "Publishing server list...");
  gnostr_blossom_settings_publish_async(on_blossom_publish_complete, ctx);
}

/* Refresh the blossom server list in settings */
static void settings_dialog_refresh_blossom_list(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkListBox *list = GTK_LIST_BOX(gtk_builder_get_object(ctx->builder, "blossom_server_list"));
  if (!list) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL) {
    gtk_list_box_remove(list, child);
  }

  /* Load servers */
  gsize count = 0;
  GnostrBlossomServer **servers = gnostr_blossom_settings_get_servers(&count);

  for (gsize i = 0; i < count; i++) {
    GnostrBlossomServer *server = servers[i];

    /* Create row context */
    BlossomServerRowCtx *row_ctx = g_new0(BlossomServerRowCtx, 1);
    row_ctx->dialog_ctx = ctx;
    row_ctx->server_index = i;
    row_ctx->server_url = g_strdup(server->url);

    /* Create row widget */
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* Priority indicator (first is default) */
    GtkWidget *priority = gtk_label_new(i == 0 ? "1" : NULL);
    if (i == 0) {
      gtk_widget_set_size_request(priority, 20, -1);
      gtk_widget_add_css_class(priority, "accent");
      gtk_widget_set_tooltip_text(priority, "Primary server");
    } else {
      char num[8];
      g_snprintf(num, sizeof(num), "%zu", i + 1);
      gtk_label_set_text(GTK_LABEL(priority), num);
      gtk_widget_set_size_request(priority, 20, -1);
      gtk_widget_add_css_class(priority, "dim-label");
    }
    gtk_box_append(GTK_BOX(box), priority);

    /* URL label */
    GtkWidget *label = gtk_label_new(server->url);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(box), label);

    /* Move up button */
    GtkWidget *btn_up = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_add_css_class(btn_up, "flat");
    gtk_widget_set_sensitive(btn_up, i > 0);
    gtk_widget_set_tooltip_text(btn_up, "Move up (higher priority)");
    g_signal_connect(btn_up, "clicked", G_CALLBACK(on_blossom_server_move_up), row_ctx);
    gtk_box_append(GTK_BOX(box), btn_up);

    /* Move down button */
    GtkWidget *btn_down = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_add_css_class(btn_down, "flat");
    gtk_widget_set_sensitive(btn_down, i < count - 1);
    gtk_widget_set_tooltip_text(btn_down, "Move down (lower priority)");
    g_signal_connect(btn_down, "clicked", G_CALLBACK(on_blossom_server_move_down), row_ctx);
    gtk_box_append(GTK_BOX(box), btn_down);

    /* Remove button */
    GtkWidget *btn_remove = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(btn_remove, "flat");
    gtk_widget_add_css_class(btn_remove, "error");
    gtk_widget_set_tooltip_text(btn_remove, "Remove server");
    g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_blossom_server_remove), row_ctx);
    gtk_box_append(GTK_BOX(box), btn_remove);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(list, row);

    /* Free row context when row is destroyed */
    g_object_set_data_full(G_OBJECT(row), "row-ctx", row_ctx, blossom_server_row_ctx_free);
  }

  gnostr_blossom_servers_free(servers, count);
}

/* Setup blossom server panel */
static void settings_dialog_setup_blossom_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  /* Connect add button */
  GtkButton *btn_add = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_blossom_add"));
  if (btn_add) {
    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_blossom_add_server), ctx);
  }

  /* Connect publish button */
  GtkButton *btn_publish = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_blossom_publish"));
  if (btn_publish) {
    g_signal_connect(btn_publish, "clicked", G_CALLBACK(on_blossom_publish_clicked), ctx);
  }

  /* Allow Enter key in entry to add server */
  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(ctx->builder, "w_blossom_server"));
  if (entry) {
    g_signal_connect_swapped(entry, "activate", G_CALLBACK(on_blossom_add_server), ctx);
  }

  /* Populate server list */
  settings_dialog_refresh_blossom_list(ctx);
}

/* Signal handlers for media settings switches */
static void on_load_remote_media_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->client_settings) {
    g_settings_set_boolean(ctx->client_settings, "load-remote-media", active);
  }
}

static void on_video_autoplay_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->client_settings) {
    g_settings_set_boolean(ctx->client_settings, "video-autoplay", active);
  }
}

static void on_video_loop_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->client_settings) {
    g_settings_set_boolean(ctx->client_settings, "video-loop", active);
  }
}

/* nostrc-61s.6: Background mode switch handler */
static void on_background_mode_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);

  /* Save to GSettings */
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  g_settings_set_boolean(settings, "background-mode", active);

  /* Update the main window's background_mode_enabled flag live */
  if (ctx && ctx->main_window && GNOSTR_IS_MAIN_WINDOW(ctx->main_window)) {
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(ctx->main_window);
    gboolean was_enabled = self->background_mode_enabled;
    self->background_mode_enabled = active;

    /* Manage application hold/release based on change */
    GtkApplication *app = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)));
    if (app) {
      if (active && !was_enabled) {
        g_application_hold(G_APPLICATION(app));
        g_debug("[SETTINGS] Background mode enabled - application held");
      } else if (!active && was_enabled) {
        g_application_release(G_APPLICATION(app));
        g_debug("[SETTINGS] Background mode disabled - application released");
      }
    }
  }
}

/* Setup media (playback) settings panel */
static void settings_dialog_setup_media_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  /* Cache client_settings in context if not already cached */
  if (!ctx->client_settings) {
    ctx->client_settings = g_settings_new("org.gnostr.Client");
    if (!ctx->client_settings) return;
  }

  /* Load remote media switch */
  GtkSwitch *w_load_media = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_load_remote_media"));
  if (w_load_media) {
    gtk_switch_set_active(w_load_media, g_settings_get_boolean(ctx->client_settings, "load-remote-media"));
    g_signal_connect(w_load_media, "notify::active", G_CALLBACK(on_load_remote_media_changed), ctx);
  }

  /* Video autoplay switch */
  GtkSwitch *w_autoplay = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_video_autoplay"));
  if (w_autoplay) {
    gtk_switch_set_active(w_autoplay, g_settings_get_boolean(ctx->client_settings, "video-autoplay"));
    g_signal_connect(w_autoplay, "notify::active", G_CALLBACK(on_video_autoplay_changed), ctx);
  }

  /* Video loop switch */
  GtkSwitch *w_loop = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_video_loop"));
  if (w_loop) {
    gtk_switch_set_active(w_loop, g_settings_get_boolean(ctx->client_settings, "video-loop"));
    g_signal_connect(w_loop, "notify::active", G_CALLBACK(on_video_loop_changed), ctx);
  }
}

/* Signal handlers for notification settings switches */
static void on_notif_enabled_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->notif_settings) {
    g_settings_set_boolean(ctx->notif_settings, "enabled", active);
  }
}

static void on_notif_mention_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->notif_settings) {
    g_settings_set_boolean(ctx->notif_settings, "notify-mention-enabled", active);
  }
}

static void on_notif_dm_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->notif_settings) {
    g_settings_set_boolean(ctx->notif_settings, "notify-dm-enabled", active);
  }
}

static void on_notif_zap_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->notif_settings) {
    g_settings_set_boolean(ctx->notif_settings, "notify-zap-enabled", active);
  }
}

static void on_notif_reply_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->notif_settings) {
    g_settings_set_boolean(ctx->notif_settings, "notify-reply-enabled", active);
  }
}

static void on_notif_sound_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->notif_settings) {
    g_settings_set_boolean(ctx->notif_settings, "sound-enabled", active);
  }
}

static void on_notif_tray_badge_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->notif_settings) {
    g_settings_set_boolean(ctx->notif_settings, "tray-badge-enabled", active);
  }
}

static void on_notif_desktop_popup_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  gboolean active = gtk_switch_get_active(sw);
  if (ctx && ctx->notif_settings) {
    g_settings_set_boolean(ctx->notif_settings, "desktop-popup-enabled", active);
  }
}

/* Setup notifications settings panel */
static void settings_dialog_setup_notifications_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  /* Cache notif_settings in context if not already cached */
  if (!ctx->notif_settings) {
    ctx->notif_settings = g_settings_new("org.gnostr.Notifications");
    if (!ctx->notif_settings) return;
  }

  /* Global enable switch */
  GtkSwitch *w_enabled = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_enabled"));
  if (w_enabled) {
    gtk_switch_set_active(w_enabled, g_settings_get_boolean(ctx->notif_settings, "enabled"));
    g_signal_connect(w_enabled, "notify::active", G_CALLBACK(on_notif_enabled_changed), ctx);
  }

  /* Per-type toggles */
  GtkSwitch *w_mention = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_mention"));
  if (w_mention) {
    gtk_switch_set_active(w_mention, g_settings_get_boolean(ctx->notif_settings, "notify-mention-enabled"));
    g_signal_connect(w_mention, "notify::active", G_CALLBACK(on_notif_mention_changed), ctx);
  }

  GtkSwitch *w_dm = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_dm"));
  if (w_dm) {
    gtk_switch_set_active(w_dm, g_settings_get_boolean(ctx->notif_settings, "notify-dm-enabled"));
    g_signal_connect(w_dm, "notify::active", G_CALLBACK(on_notif_dm_changed), ctx);
  }

  GtkSwitch *w_zap = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_zap"));
  if (w_zap) {
    gtk_switch_set_active(w_zap, g_settings_get_boolean(ctx->notif_settings, "notify-zap-enabled"));
    g_signal_connect(w_zap, "notify::active", G_CALLBACK(on_notif_zap_changed), ctx);
  }

  GtkSwitch *w_reply = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_reply"));
  if (w_reply) {
    gtk_switch_set_active(w_reply, g_settings_get_boolean(ctx->notif_settings, "notify-reply-enabled"));
    g_signal_connect(w_reply, "notify::active", G_CALLBACK(on_notif_reply_changed), ctx);
  }

  /* Presentation switches */
  GtkSwitch *w_sound = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_sound"));
  if (w_sound) {
    gtk_switch_set_active(w_sound, g_settings_get_boolean(ctx->notif_settings, "sound-enabled"));
    g_signal_connect(w_sound, "notify::active", G_CALLBACK(on_notif_sound_changed), ctx);
  }

  GtkSwitch *w_tray_badge = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_tray_badge"));
  if (w_tray_badge) {
    gtk_switch_set_active(w_tray_badge, g_settings_get_boolean(ctx->notif_settings, "tray-badge-enabled"));
    g_signal_connect(w_tray_badge, "notify::active", G_CALLBACK(on_notif_tray_badge_changed), ctx);
  }

  GtkSwitch *w_desktop_popup = GTK_SWITCH(gtk_builder_get_object(ctx->builder, "w_notif_desktop_popup"));
  if (w_desktop_popup) {
    gtk_switch_set_active(w_desktop_popup, g_settings_get_boolean(ctx->notif_settings, "desktop-popup-enabled"));
    g_signal_connect(w_desktop_popup, "notify::active", G_CALLBACK(on_notif_desktop_popup_changed), ctx);
  }
}

/* Plugin panel signal handlers */
static void on_plugin_settings_signal(GnostrPluginManagerPanel *panel,
                                      const char *plugin_id,
                                      gpointer user_data) {
  (void)user_data;
  gnostr_plugin_manager_panel_show_plugin_settings(panel, plugin_id);
}

static void on_plugin_info_signal(GnostrPluginManagerPanel *panel,
                                  const char *plugin_id,
                                  gpointer user_data) {
  (void)user_data;
  gnostr_plugin_manager_panel_show_plugin_info(panel, plugin_id);
}

/* --- Metrics Panel (6.3) --- */

typedef struct {
  GtkLabel *lbl_connected_relays;
  GtkLabel *lbl_active_subs;
  GtkLabel *lbl_queue_depth;
  GtkLabel *lbl_events_received;
  GtkLabel *lbl_events_dispatched;
  GtkLabel *lbl_events_dropped;
  GtkLabel *lbl_drop_rate;
  GtkLabel *lbl_dispatch_p50;
  GtkLabel *lbl_dispatch_p99;
  GtkLabel *lbl_status_icon;
  /* NDB storage stats (nostrc-o6w) */
  GtkLabel *lbl_ndb_notes;
  GtkLabel *lbl_ndb_profiles;
  GtkLabel *lbl_ndb_storage;
  GtkLabel *lbl_ndb_text;
  GtkLabel *lbl_ndb_reactions;
  GtkLabel *lbl_ndb_zaps;
  GtkLabel *lbl_ndb_ingest;
  GtkWidget *panel;
  guint timer_id;
} MetricsPanelCtx;

static GtkWidget *metrics_add_row(GtkListBox *list, const char *title, const char *initial) {
  GtkWidget *row = gtk_list_box_row_new();
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);

  GtkWidget *lbl_title = gtk_label_new(title);
  gtk_widget_set_hexpand(lbl_title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(lbl_title), 0);

  GtkWidget *lbl_value = gtk_label_new(initial);
  gtk_widget_add_css_class(lbl_value, "dim-label");

  gtk_box_append(GTK_BOX(box), lbl_title);
  gtk_box_append(GTK_BOX(box), lbl_value);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_list_box_append(list, row);
  return lbl_value;
}

static void metrics_panel_refresh(MetricsPanelCtx *mctx) {
  if (!mctx || !mctx->panel) return;
  if (!gtk_widget_get_mapped(mctx->panel)) return;

  NostrMetricsSnapshot snap;
  memset(&snap, 0, sizeof(snap));
  if (!nostr_metrics_collector_latest(&snap)) {
    /* Collector not running or no data — try an immediate collect */
    nostr_metrics_snapshot_collect(&snap);
  }

  char buf[64];

  /* Find specific metrics by name */
  int64_t connected = 0, active_subs = 0, queue_depth = 0;
  uint64_t events_recv = 0, events_disp = 0, events_drop = 0;
  uint64_t recv_delta = 0, disp_delta = 0, drop_delta = 0;
  uint64_t disp_p50 = 0, disp_p99 = 0;

  for (size_t i = 0; i < snap.gauge_count; i++) {
    if (!snap.gauges[i].name) continue;
    if (strcmp(snap.gauges[i].name, METRIC_CONNECTED_RELAYS) == 0)
      connected = snap.gauges[i].value;
    else if (strcmp(snap.gauges[i].name, METRIC_ACTIVE_SUBSCRIPTIONS) == 0)
      active_subs = snap.gauges[i].value;
    else if (strcmp(snap.gauges[i].name, METRIC_QUEUE_DEPTH) == 0)
      queue_depth = snap.gauges[i].value;
  }

  for (size_t i = 0; i < snap.counter_count; i++) {
    if (!snap.counters[i].name) continue;
    if (strcmp(snap.counters[i].name, METRIC_EVENTS_RECEIVED) == 0) {
      events_recv = snap.counters[i].total;
      recv_delta = snap.counters[i].delta_60s;
    } else if (strcmp(snap.counters[i].name, METRIC_EVENTS_DISPATCHED) == 0) {
      events_disp = snap.counters[i].total;
      disp_delta = snap.counters[i].delta_60s;
    } else if (strcmp(snap.counters[i].name, METRIC_EVENTS_DROPPED) == 0) {
      events_drop = snap.counters[i].total;
      drop_delta = snap.counters[i].delta_60s;
    }
  }

  for (size_t i = 0; i < snap.histogram_count; i++) {
    if (!snap.histograms[i].name) continue;
    if (strcmp(snap.histograms[i].name, METRIC_DISPATCH_LATENCY_NS) == 0) {
      disp_p50 = snap.histograms[i].p50_ns;
      disp_p99 = snap.histograms[i].p99_ns;
    }
  }

  /* Update labels */
  snprintf(buf, sizeof(buf), "%lld", (long long)connected);
  gtk_label_set_text(mctx->lbl_connected_relays, buf);

  snprintf(buf, sizeof(buf), "%lld", (long long)active_subs);
  gtk_label_set_text(mctx->lbl_active_subs, buf);

  snprintf(buf, sizeof(buf), "%lld", (long long)queue_depth);
  gtk_label_set_text(mctx->lbl_queue_depth, buf);

  snprintf(buf, sizeof(buf), "%llu (+%llu/min)", (unsigned long long)events_recv,
           (unsigned long long)recv_delta);
  gtk_label_set_text(mctx->lbl_events_received, buf);

  snprintf(buf, sizeof(buf), "%llu (+%llu/min)", (unsigned long long)events_disp,
           (unsigned long long)disp_delta);
  gtk_label_set_text(mctx->lbl_events_dispatched, buf);

  snprintf(buf, sizeof(buf), "%llu (+%llu/min)", (unsigned long long)events_drop,
           (unsigned long long)drop_delta);
  gtk_label_set_text(mctx->lbl_events_dropped, buf);

  /* Drop rate */
  double drop_rate = events_recv > 0
    ? (double)events_drop / (double)events_recv * 100.0 : 0.0;
  snprintf(buf, sizeof(buf), "%.2f%%", drop_rate);
  gtk_label_set_text(mctx->lbl_drop_rate, buf);

  /* Dispatch latency (convert ns to us for readability) */
  snprintf(buf, sizeof(buf), "%.1f \xC2\xB5s", disp_p50 / 1000.0);
  gtk_label_set_text(mctx->lbl_dispatch_p50, buf);

  snprintf(buf, sizeof(buf), "%.1f \xC2\xB5s", disp_p99 / 1000.0);
  gtk_label_set_text(mctx->lbl_dispatch_p99, buf);

  /* Status indicator: green if drop_rate < 1%, yellow < 5%, red >= 5% */
  if (drop_rate >= 5.0)
    gtk_label_set_text(mctx->lbl_status_icon, "Degraded");
  else if (drop_rate >= 1.0)
    gtk_label_set_text(mctx->lbl_status_icon, "Warning");
  else
    gtk_label_set_text(mctx->lbl_status_icon, "Healthy");

  nostr_metrics_snapshot_free(&snap);

  /* NDB storage stats (nostrc-o6w) */
  StorageNdbStat nst;
  if (storage_ndb_get_stat(&nst) == 0) {
    snprintf(buf, sizeof(buf), "%zu", nst.note_count);
    gtk_label_set_text(mctx->lbl_ndb_notes, buf);

    snprintf(buf, sizeof(buf), "%zu", nst.profile_count);
    gtk_label_set_text(mctx->lbl_ndb_profiles, buf);

    if (nst.total_bytes >= 1024 * 1024)
      snprintf(buf, sizeof(buf), "%.1f MB", nst.total_bytes / (1024.0 * 1024.0));
    else if (nst.total_bytes >= 1024)
      snprintf(buf, sizeof(buf), "%.1f KB", nst.total_bytes / 1024.0);
    else
      snprintf(buf, sizeof(buf), "%zu B", nst.total_bytes);
    gtk_label_set_text(mctx->lbl_ndb_storage, buf);

    snprintf(buf, sizeof(buf), "%zu", nst.kind_text);
    gtk_label_set_text(mctx->lbl_ndb_text, buf);

    snprintf(buf, sizeof(buf), "%zu", nst.kind_reaction);
    gtk_label_set_text(mctx->lbl_ndb_reactions, buf);

    snprintf(buf, sizeof(buf), "%zu", nst.kind_zap);
    gtk_label_set_text(mctx->lbl_ndb_zaps, buf);

    uint64_t ic = storage_ndb_get_ingest_count();
    uint64_t ib = storage_ndb_get_ingest_bytes();
    if (ib >= 1024 * 1024)
      snprintf(buf, sizeof(buf), "%llu events / %.1f MB",
               (unsigned long long)ic, ib / (1024.0 * 1024.0));
    else
      snprintf(buf, sizeof(buf), "%llu events / %llu B",
               (unsigned long long)ic, (unsigned long long)ib);
    gtk_label_set_text(mctx->lbl_ndb_ingest, buf);

    /* Also push to metrics gauges for Prometheus export */
    storage_ndb_update_metrics();
  }
}

static gboolean metrics_panel_tick(gpointer user_data) {
  MetricsPanelCtx *mctx = user_data;
  metrics_panel_refresh(mctx);
  return G_SOURCE_CONTINUE;
}

static void metrics_panel_ctx_free(gpointer data) {
  MetricsPanelCtx *mctx = data;
  if (!mctx) return;
  if (mctx->timer_id) g_source_remove(mctx->timer_id);
  g_free(mctx);
}

static void settings_dialog_setup_metrics_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkBox *panel = GTK_BOX(gtk_builder_get_object(ctx->builder, "metrics_panel"));
  if (!panel) return;

  MetricsPanelCtx *mctx = g_new0(MetricsPanelCtx, 1);
  mctx->panel = GTK_WIDGET(panel);

  /* Connection Health section */
  GtkWidget *health_label = gtk_label_new("Connection Health");
  gtk_label_set_xalign(GTK_LABEL(health_label), 0);
  gtk_widget_add_css_class(health_label, "heading");
  gtk_box_append(panel, health_label);

  GtkWidget *health_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(health_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(health_list, "boxed-list");
  gtk_box_append(panel, health_list);

  mctx->lbl_status_icon = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(health_list), "Status", "Healthy"));
  mctx->lbl_connected_relays = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(health_list), "Connected Relays", "0"));
  mctx->lbl_active_subs = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(health_list), "Active Subscriptions", "0"));
  mctx->lbl_queue_depth = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(health_list), "Queue Depth", "0"));

  /* Event Flow section */
  GtkWidget *flow_label = gtk_label_new("Event Flow");
  gtk_label_set_xalign(GTK_LABEL(flow_label), 0);
  gtk_widget_add_css_class(flow_label, "heading");
  gtk_box_append(panel, flow_label);

  GtkWidget *flow_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(flow_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(flow_list, "boxed-list");
  gtk_box_append(panel, flow_list);

  mctx->lbl_events_received = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(flow_list), "Events Received", "0"));
  mctx->lbl_events_dispatched = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(flow_list), "Events Dispatched", "0"));
  mctx->lbl_events_dropped = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(flow_list), "Events Dropped", "0"));
  mctx->lbl_drop_rate = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(flow_list), "Drop Rate", "0.00%"));

  /* Latency section */
  GtkWidget *lat_label = gtk_label_new("Dispatch Latency");
  gtk_label_set_xalign(GTK_LABEL(lat_label), 0);
  gtk_widget_add_css_class(lat_label, "heading");
  gtk_box_append(panel, lat_label);

  GtkWidget *lat_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(lat_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(lat_list, "boxed-list");
  gtk_box_append(panel, lat_list);

  mctx->lbl_dispatch_p50 = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(lat_list), "p50", "0.0 \xC2\xB5s"));
  mctx->lbl_dispatch_p99 = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(lat_list), "p99", "0.0 \xC2\xB5s"));

  /* NDB Storage section (nostrc-o6w) */
  GtkWidget *ndb_label = gtk_label_new("Storage");
  gtk_label_set_xalign(GTK_LABEL(ndb_label), 0);
  gtk_widget_add_css_class(ndb_label, "heading");
  gtk_box_append(panel, ndb_label);

  GtkWidget *ndb_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(ndb_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(ndb_list, "boxed-list");
  gtk_box_append(panel, ndb_list);

  mctx->lbl_ndb_notes     = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Notes", "0"));
  mctx->lbl_ndb_profiles  = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Profiles", "0"));
  mctx->lbl_ndb_storage   = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "DB Size", "0 B"));
  mctx->lbl_ndb_text      = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Text Notes", "0"));
  mctx->lbl_ndb_reactions = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Reactions", "0"));
  mctx->lbl_ndb_zaps      = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Zaps", "0"));
  mctx->lbl_ndb_ingest    = GTK_LABEL(metrics_add_row(GTK_LIST_BOX(ndb_list), "Ingested", "0 events / 0 B"));

  /* Initial refresh */
  metrics_panel_refresh(mctx);

  /* Timer: refresh every 2 seconds while dialog is open */
  mctx->timer_id = g_timeout_add_seconds(2, metrics_panel_tick, mctx);

  /* Clean up when dialog is destroyed */
  g_object_set_data_full(G_OBJECT(ctx->win), "metrics-panel-ctx", mctx, metrics_panel_ctx_free);
}

/* --- NIP-47 Wallet (Lightning) settings panel --- */

typedef struct {
  SettingsDialogCtx *dialog_ctx;
} WalletPanelCtx;

static void on_nwc_balance_finish(GObject *source, GAsyncResult *result, gpointer user_data) {
  GnostrNwcService *nwc = GNOSTR_NWC_SERVICE(source);
  GtkLabel *lbl = GTK_LABEL(user_data);

  GError *error = NULL;
  gint64 balance_msat = 0;

  if (gnostr_nwc_service_get_balance_finish(nwc, result, &balance_msat, &error)) {
    gchar *formatted = gnostr_nwc_format_balance(balance_msat);
    if (GTK_IS_LABEL(lbl)) gtk_label_set_text(lbl, formatted);
    g_free(formatted);
  } else {
    if (GTK_IS_LABEL(lbl)) gtk_label_set_text(lbl, "Unable to fetch");
    g_clear_error(&error);
  }
  g_object_unref(lbl);
}

static void on_nwc_refresh_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GtkLabel *lbl = GTK_LABEL(user_data);
  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  if (!gnostr_nwc_service_is_connected(nwc)) return;
  gnostr_nwc_service_get_balance_async(nwc, NULL, on_nwc_balance_finish, g_object_ref(lbl));
}

static void on_nwc_connect_clicked_settings(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  GnostrNwcConnect *dialog = gnostr_nwc_connect_new(ctx->win);
  gtk_window_present(GTK_WINDOW(dialog));
}

static void on_nwc_disconnect_clicked_settings(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  gnostr_nwc_service_disconnect(nwc);
  gnostr_nwc_service_save_to_settings(nwc);

  /* Update UI to disconnected state */
  GtkBuilder *b = ctx->builder;
  GtkLabel *lbl_status = GTK_LABEL(gtk_builder_get_object(b, "lbl_nwc_status"));
  if (lbl_status) gtk_label_set_text(lbl_status, "Not connected");

  GtkWidget *balance_row = GTK_WIDGET(gtk_builder_get_object(b, "nwc_balance_row"));
  GtkWidget *wallet_row = GTK_WIDGET(gtk_builder_get_object(b, "nwc_wallet_row"));
  GtkWidget *relay_row = GTK_WIDGET(gtk_builder_get_object(b, "nwc_relay_row"));
  GtkWidget *btn_disconnect = GTK_WIDGET(gtk_builder_get_object(b, "btn_nwc_disconnect"));
  GtkWidget *btn_connect = GTK_WIDGET(gtk_builder_get_object(b, "btn_nwc_connect"));

  if (balance_row) gtk_widget_set_visible(balance_row, FALSE);
  if (wallet_row) gtk_widget_set_visible(wallet_row, FALSE);
  if (relay_row) gtk_widget_set_visible(relay_row, FALSE);
  if (btn_disconnect) gtk_widget_set_visible(btn_disconnect, FALSE);
  if (btn_connect) gtk_widget_set_visible(btn_connect, TRUE);
}

static void settings_dialog_setup_wallet_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkBuilder *b = ctx->builder;
  GnostrNwcService *nwc = gnostr_nwc_service_get_default();
  gboolean connected = gnostr_nwc_service_is_connected(nwc);

  /* Status label */
  GtkLabel *lbl_status = GTK_LABEL(gtk_builder_get_object(b, "lbl_nwc_status"));
  if (lbl_status) {
    gtk_label_set_text(lbl_status, connected ? "Connected" : "Not connected");
    if (connected) {
      gtk_widget_remove_css_class(GTK_WIDGET(lbl_status), "dim-label");
      gtk_widget_add_css_class(GTK_WIDGET(lbl_status), "success");
    }
  }

  /* Show/hide connected details */
  GtkWidget *balance_row = GTK_WIDGET(gtk_builder_get_object(b, "nwc_balance_row"));
  GtkWidget *wallet_row = GTK_WIDGET(gtk_builder_get_object(b, "nwc_wallet_row"));
  GtkWidget *relay_row = GTK_WIDGET(gtk_builder_get_object(b, "nwc_relay_row"));
  GtkWidget *btn_disconnect = GTK_WIDGET(gtk_builder_get_object(b, "btn_nwc_disconnect"));
  GtkWidget *btn_connect_w = GTK_WIDGET(gtk_builder_get_object(b, "btn_nwc_connect"));

  if (balance_row) gtk_widget_set_visible(balance_row, connected);
  if (wallet_row) gtk_widget_set_visible(wallet_row, connected);
  if (relay_row) gtk_widget_set_visible(relay_row, connected);
  if (btn_disconnect) gtk_widget_set_visible(btn_disconnect, connected);
  if (btn_connect_w) gtk_widget_set_visible(btn_connect_w, !connected);

  if (connected) {
    /* Wallet pubkey */
    GtkLabel *lbl_wallet = GTK_LABEL(gtk_builder_get_object(b, "lbl_nwc_wallet"));
    if (lbl_wallet) {
      const gchar *pk = gnostr_nwc_service_get_wallet_pubkey(nwc);
      if (pk && strlen(pk) >= 64) {
        g_autofree gchar *trunc = g_strdup_printf("%.8s...%.8s", pk, pk + 56);
        gtk_label_set_text(lbl_wallet, trunc);
      }
    }

    /* Relay */
    GtkLabel *lbl_relay = GTK_LABEL(gtk_builder_get_object(b, "lbl_nwc_relay"));
    if (lbl_relay) {
      const gchar *relay = gnostr_nwc_service_get_relay(nwc);
      gtk_label_set_text(lbl_relay, relay ? relay : "Not specified");
    }
  }

  /* Connect button handlers */
  GtkButton *btn_connect = GTK_BUTTON(gtk_builder_get_object(b, "btn_nwc_connect"));
  if (btn_connect) {
    g_signal_connect(btn_connect, "clicked", G_CALLBACK(on_nwc_connect_clicked_settings), ctx);
  }

  GtkButton *btn_disc = GTK_BUTTON(gtk_builder_get_object(b, "btn_nwc_disconnect"));
  if (btn_disc) {
    g_signal_connect(btn_disc, "clicked", G_CALLBACK(on_nwc_disconnect_clicked_settings), ctx);
  }

  /* Balance refresh button */
  GtkButton *btn_refresh = GTK_BUTTON(gtk_builder_get_object(b, "btn_nwc_refresh"));
  GtkLabel *lbl_balance = GTK_LABEL(gtk_builder_get_object(b, "lbl_nwc_balance"));
  if (btn_refresh && lbl_balance) {
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_nwc_refresh_clicked), lbl_balance);
  }
}

/* ---- Mute List Panel ---- */

typedef struct {
  SettingsDialogCtx *dialog_ctx;
  char *canonical_value;
  GnostrMuteRowType type;
} MutePanelRowData;

static void mute_panel_row_data_free(gpointer data) {
  MutePanelRowData *rd = (MutePanelRowData *)data;
  if (rd) { g_free(rd->canonical_value); g_free(rd); }
}

static void mute_panel_refresh(SettingsDialogCtx *ctx);

static void on_mute_panel_remove_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  MutePanelRowData *rd = (MutePanelRowData *)user_data;
  if (!rd || !rd->dialog_ctx) return;
  GNostrMuteList *mute = gnostr_mute_list_get_default();
  switch (rd->type) {
    case GNOSTR_MUTE_ROW_USER:    gnostr_mute_list_remove_pubkey(mute, rd->canonical_value);  break;
    case GNOSTR_MUTE_ROW_WORD:    gnostr_mute_list_remove_word(mute, rd->canonical_value);    break;
    case GNOSTR_MUTE_ROW_HASHTAG: gnostr_mute_list_remove_hashtag(mute, rd->canonical_value); break;
  }
  mute_panel_refresh(rd->dialog_ctx);
}

static GtkWidget *mute_panel_create_row(SettingsDialogCtx *ctx,
                                         const char *display, const char *canonical,
                                         GnostrMuteRowType type) {
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);

  GtkWidget *label = gtk_label_new(display);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), label);

  GtkWidget *remove_btn = gtk_button_new_from_icon_name("list-remove-symbolic");
  gtk_widget_add_css_class(remove_btn, "flat");
  gtk_widget_set_tooltip_text(remove_btn, "Remove");
  gtk_box_append(GTK_BOX(box), remove_btn);

  MutePanelRowData *rd = g_new0(MutePanelRowData, 1);
  rd->dialog_ctx = ctx;
  rd->canonical_value = g_strdup(canonical);
  rd->type = type;
  g_signal_connect_data(remove_btn, "clicked",
                        G_CALLBACK(on_mute_panel_remove_clicked),
                        rd, (GClosureNotify)mute_panel_row_data_free, 0);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  return row;
}

static void mute_panel_clear_list(GtkListBox *lb) {
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(lb));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(lb, child);
    child = next;
  }
}

static void mute_panel_refresh(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;
  GNostrMuteList *mute = gnostr_mute_list_get_default();

  /* Users */
  GtkListBox *list_users = GTK_LIST_BOX(gtk_builder_get_object(ctx->builder, "list_mute_users"));
  if (list_users) {
    mute_panel_clear_list(list_users);
    size_t count = 0;
    const char **pubkeys = gnostr_mute_list_get_pubkeys(mute, &count);
    for (size_t i = 0; i < count; i++) {
      /* Try to show npub for readability */
      g_autofree char *display = NULL;
      g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_npub(pubkeys[i], NULL);
      if (n19) {
        display = g_strdup(gnostr_nip19_get_bech32(n19));
      }
      GtkWidget *row = mute_panel_create_row(ctx,
                                              display ? display : pubkeys[i],
                                              pubkeys[i],
                                              GNOSTR_MUTE_ROW_USER);
      gtk_list_box_append(list_users, row);
    }
    g_free((void *)pubkeys);
  }

  /* Words */
  GtkListBox *list_words = GTK_LIST_BOX(gtk_builder_get_object(ctx->builder, "list_mute_words"));
  if (list_words) {
    mute_panel_clear_list(list_words);
    size_t count = 0;
    const char **words = gnostr_mute_list_get_words(mute, &count);
    for (size_t i = 0; i < count; i++) {
      GtkWidget *row = mute_panel_create_row(ctx, words[i], words[i], GNOSTR_MUTE_ROW_WORD);
      gtk_list_box_append(list_words, row);
    }
    g_free((void *)words);
  }

  /* Hashtags */
  GtkListBox *list_hashtags = GTK_LIST_BOX(gtk_builder_get_object(ctx->builder, "list_mute_hashtags"));
  if (list_hashtags) {
    mute_panel_clear_list(list_hashtags);
    size_t count = 0;
    const char **hashtags = gnostr_mute_list_get_hashtags(mute, &count);
    for (size_t i = 0; i < count; i++) {
      g_autofree char *display = g_strdup_printf("#%s", hashtags[i]);
      GtkWidget *row = mute_panel_create_row(ctx, display, hashtags[i], GNOSTR_MUTE_ROW_HASHTAG);
      gtk_list_box_append(list_hashtags, row);
    }
    g_free((void *)hashtags);
  }

  /* Update save button sensitivity */
  GtkWidget *btn_save = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "btn_mute_save"));
  GtkWidget *status_bar = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "mute_status_bar"));
  if (btn_save) {
    gboolean dirty = gnostr_mute_list_is_dirty(mute);
    gtk_widget_set_sensitive(btn_save, dirty);
    if (status_bar) gtk_widget_set_visible(status_bar, dirty);
  }
}

static void on_mute_fetch_done(GNostrMuteList *mute, gboolean success, gpointer user_data) {
  (void)mute;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  if (!ctx || !ctx->builder) return;
  if (success) {
    mute_panel_refresh(ctx);
  } else {
    g_warning("mute_list: settings panel fetch failed");
  }
}

static void on_mute_save_done(GNostrMuteList *mute, gboolean success,
                               const char *error_msg, gpointer user_data) {
  (void)mute;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  if (!ctx || !ctx->builder) return;

  GtkLabel *status_lbl = GTK_LABEL(gtk_builder_get_object(ctx->builder, "mute_status_label"));
  GtkWidget *status_bar = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "mute_status_bar"));

  if (success) {
    if (status_lbl) gtk_label_set_text(status_lbl, "Mute list saved!");
    if (status_bar) gtk_widget_set_visible(status_bar, TRUE);
    mute_panel_refresh(ctx);
  } else {
    g_autofree char *msg = g_strdup_printf("Save failed: %s", error_msg ? error_msg : "unknown");
    if (status_lbl) gtk_label_set_text(status_lbl, msg);
    if (status_bar) gtk_widget_set_visible(status_bar, TRUE);
  }
}

static void on_mute_panel_save_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  gnostr_mute_list_save_async(gnostr_mute_list_get_default(), on_mute_save_done, ctx);
}

static void on_mute_panel_add_user(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  GtkEditable *entry = GTK_EDITABLE(gtk_builder_get_object(ctx->builder, "entry_mute_user"));
  if (!entry) return;
  const char *input = gtk_editable_get_text(entry);
  if (!input || !*input) return;

  char pubkey_hex[65] = {0};
  gboolean valid = FALSE;

  if (g_str_has_prefix(input, "npub1")) {
    g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(input, NULL);
    if (n19) {
      g_strlcpy(pubkey_hex, gnostr_nip19_get_pubkey(n19), sizeof(pubkey_hex));
      valid = TRUE;
    }
  } else if (strlen(input) == 64) {
    gboolean all_hex = TRUE;
    for (int i = 0; i < 64 && all_hex; i++) {
      if (!g_ascii_isxdigit(input[i])) all_hex = FALSE;
    }
    if (all_hex) { g_strlcpy(pubkey_hex, input, sizeof(pubkey_hex)); valid = TRUE; }
  }

  if (!valid) return;
  gnostr_mute_list_add_pubkey(gnostr_mute_list_get_default(), pubkey_hex, FALSE);
  gtk_editable_set_text(entry, "");
  mute_panel_refresh(ctx);
}

static void on_mute_panel_add_word(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  GtkEditable *entry = GTK_EDITABLE(gtk_builder_get_object(ctx->builder, "entry_mute_word"));
  if (!entry) return;
  const char *word = gtk_editable_get_text(entry);
  if (!word || !*word) return;
  gnostr_mute_list_add_word(gnostr_mute_list_get_default(), word, FALSE);
  gtk_editable_set_text(entry, "");
  mute_panel_refresh(ctx);
}

static void on_mute_panel_add_hashtag(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SettingsDialogCtx *ctx = (SettingsDialogCtx *)user_data;
  GtkEditable *entry = GTK_EDITABLE(gtk_builder_get_object(ctx->builder, "entry_mute_hashtag"));
  if (!entry) return;
  const char *hashtag = gtk_editable_get_text(entry);
  if (!hashtag || !*hashtag) return;
  /* Strip leading # if present */
  if (hashtag[0] == '#') hashtag++;
  if (!*hashtag) return;
  gnostr_mute_list_add_hashtag(gnostr_mute_list_get_default(), hashtag, FALSE);
  gtk_editable_set_text(GTK_EDITABLE(gtk_builder_get_object(ctx->builder, "entry_mute_hashtag")), "");
  mute_panel_refresh(ctx);
}

static void settings_dialog_setup_mute_panel(SettingsDialogCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  /* Wire up buttons */
  GtkButton *btn_save = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_mute_save"));
  GtkButton *btn_add_user = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_mute_add_user"));
  GtkButton *btn_add_word = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_mute_add_word"));
  GtkButton *btn_add_hashtag = GTK_BUTTON(gtk_builder_get_object(ctx->builder, "btn_mute_add_hashtag"));

  if (btn_save) g_signal_connect(btn_save, "clicked", G_CALLBACK(on_mute_panel_save_clicked), ctx);
  if (btn_add_user) g_signal_connect(btn_add_user, "clicked", G_CALLBACK(on_mute_panel_add_user), ctx);
  if (btn_add_word) g_signal_connect(btn_add_word, "clicked", G_CALLBACK(on_mute_panel_add_word), ctx);
  if (btn_add_hashtag) g_signal_connect(btn_add_hashtag, "clicked", G_CALLBACK(on_mute_panel_add_hashtag), ctx);

  /* Fetch and populate if logged in */
  const char *pubkey = ctx->main_window->user_pubkey_hex;
  if (pubkey && *pubkey) {
    GNostrMuteList *mute = gnostr_mute_list_get_default();
    gint64 last_time = gnostr_mute_list_get_last_event_time(mute);
    if (last_time > 0) {
      /* Already have data - just populate */
      mute_panel_refresh(ctx);
    } else {
      /* Need to fetch from relays */
      gnostr_mute_list_fetch_async(mute, pubkey, NULL, on_mute_fetch_done, ctx);
    }
  }
}

static void on_settings_dialog_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  SettingsDialogCtx *ctx = (SettingsDialogCtx*)user_data;
  settings_dialog_ctx_free(ctx);
}

void gnostr_main_window_on_settings_clicked_internal(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
  GtkBuilder *builder = gtk_builder_new_from_resource("/org/gnostr/ui/ui/dialogs/gnostr-settings-dialog.ui");
  if (!builder) { show_toast(self, "Settings UI missing"); return; }
  GtkWindow *win = GTK_WINDOW(gtk_builder_get_object(builder, "settings_window"));
  if (!win) { g_object_unref(builder); show_toast(self, "Settings window missing"); return; }
  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  gtk_window_set_modal(win, TRUE);

  /* Create context for the dialog */
  SettingsDialogCtx *ctx = g_new0(SettingsDialogCtx, 1);
  ctx->win = win;
  ctx->builder = builder;
  ctx->main_window = self;

  /* Check if user is logged in and update mute list visibility.
   * Use GSettings directly for consistency with update_login_ui_state() */
  GSettings *mute_settings = g_settings_new("org.gnostr.Client");
  char *mute_npub = mute_settings ? g_settings_get_string(mute_settings, "current-npub") : NULL;
  gboolean is_logged_in = (mute_npub && *mute_npub);
  g_free(mute_npub);
  if (mute_settings) g_object_unref(mute_settings);

  GtkWidget *mute_login_required = GTK_WIDGET(gtk_builder_get_object(builder, "mute_login_required"));
  GtkWidget *mute_content = GTK_WIDGET(gtk_builder_get_object(builder, "mute_content"));
  if (mute_login_required) gtk_widget_set_visible(mute_login_required, !is_logged_in);
  if (mute_content) gtk_widget_set_visible(mute_content, is_logged_in);

  /* Load current settings values (Advanced panel - technical options) */
  GtkSpinButton *w_limit = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_limit"));
  GtkSpinButton *w_batch = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_batch"));
  GtkSpinButton *w_interval = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_interval"));
  GtkSpinButton *w_quiet = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_quiet"));
  GtkSwitch *w_use_since = GTK_SWITCH(gtk_builder_get_object(builder, "w_use_since"));
  GtkSpinButton *w_since = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_since"));
  GtkSpinButton *w_backfill = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "w_backfill"));

  if (w_limit) gtk_spin_button_set_value(w_limit, self->default_limit);
  if (w_batch) gtk_spin_button_set_value(w_batch, self->batch_max);
  if (w_interval) gtk_spin_button_set_value(w_interval, self->post_interval_ms);
  if (w_quiet) gtk_spin_button_set_value(w_quiet, self->eose_quiet_ms);
  if (w_use_since) gtk_switch_set_active(w_use_since, self->use_since);
  if (w_since) gtk_spin_button_set_value(w_since, self->since_seconds);
  if (w_backfill) gtk_spin_button_set_value(w_backfill, self->backfill_interval_sec);

  /* Setup new panels */
  settings_dialog_setup_general_panel(ctx);  /* nostrc-61s.6: background mode */
  settings_dialog_setup_relay_panel(ctx);
  settings_dialog_setup_index_relay_panel(ctx);
  settings_dialog_setup_display_panel(ctx);
  settings_dialog_setup_notifications_panel(ctx);
  settings_dialog_setup_account_panel(ctx);
  settings_dialog_setup_blossom_panel(ctx);
  settings_dialog_setup_media_panel(ctx);
  settings_dialog_setup_metrics_panel(ctx);
  settings_dialog_setup_mute_panel(ctx);
  settings_dialog_setup_wallet_panel(ctx);

  /* Connect plugin manager panel signals */
  GnostrPluginManagerPanel *plugin_panel = GNOSTR_PLUGIN_MANAGER_PANEL(
      gtk_builder_get_object(builder, "plugin_manager_panel"));
  if (plugin_panel) {
    g_signal_connect(plugin_panel, "plugin-settings",
                     G_CALLBACK(on_plugin_settings_signal), NULL);
    g_signal_connect(plugin_panel, "plugin-info",
                     G_CALLBACK(on_plugin_info_signal), NULL);
  }

  /* Context is freed when window is destroyed */
  g_signal_connect(win, "destroy", G_CALLBACK(on_settings_dialog_destroy), ctx);
  gtk_window_present(win);
}

