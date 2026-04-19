/**
 * gnostr-main-window-relay-manager.c — Relay Manager Dialog
 *
 * Extracted from gnostr-main-window.c. Contains the relay manager dialog
 * (with NIP-11 relay info display) and the NIP-66 relay discovery sub-dialog.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-main-window-private.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include "../util/nip66_relay_discovery.h"
#include "../util/relay_info.h"
#include <gtk/gtk.h>
#include <libadwaita-1/adwaita.h>
#include <pango/pango.h>
#include <string.h>

/* Use show_toast from main window */
#define show_toast(self, msg) gnostr_main_window_show_toast_internal(self, msg)

/* ---- Relay Manager Dialog with NIP-11 support ---- */

/* Forward declarations for relay manager */
typedef struct _RelayManagerCtx RelayManagerCtx;
static void relay_manager_fetch_info(RelayManagerCtx *ctx, const gchar *url);

struct _RelayManagerCtx {
  GtkWindow *window;
  GtkBuilder *builder;
  GtkStringList *relay_model;
  GtkSingleSelection *selection;
  GCancellable *fetch_cancellable;
  gchar *selected_url;  /* Currently selected relay URL */
  gboolean modified;    /* Track if relays list was modified */
  GHashTable *relay_types; /* URL -> GnostrRelayType (as GINT_TO_POINTER) */
  gboolean destroyed;   /* Set to TRUE when window is destroyed */
  gint ref_count;       /* Reference count for safe async cleanup */
  GnostrMainWindow *main_window; /* Reference to main window for pool access */
  GtkListView *list_view; /* For live relay-state refresh */
  gulong pool_state_handler_id; /* relay-state-changed signal handler */
};

static void relay_manager_ctx_ref(RelayManagerCtx *ctx) {
  if (ctx) g_atomic_int_inc(&ctx->ref_count);
}

static void relay_manager_ctx_unref(RelayManagerCtx *ctx) {
  if (!ctx) return;
  if (!g_atomic_int_dec_and_test(&ctx->ref_count)) return;

  /* Last reference - actually free */
  if (ctx->fetch_cancellable) {
    g_object_unref(ctx->fetch_cancellable);
  }
  if (ctx->relay_types) {
    g_hash_table_destroy(ctx->relay_types);
  }
  g_free(ctx->selected_url);
  g_free(ctx);
}

static void relay_manager_ctx_free(RelayManagerCtx *ctx) {
  if (!ctx) return;
  if (ctx->fetch_cancellable) {
    g_cancellable_cancel(ctx->fetch_cancellable);
  }
  /* Don't unref cancellable here - let unref handle it */
  relay_manager_ctx_unref(ctx);
}

static void relay_manager_update_status(RelayManagerCtx *ctx) {
  GtkLabel *status = GTK_LABEL(gtk_builder_get_object(ctx->builder, "status_label"));
  if (!status) return;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_model));
  g_autofree gchar *text = g_strdup_printf("<small>%u relay%s%s</small>", n, n == 1 ? "" : "s",
                                 ctx->modified ? " (modified)" : "");
  gtk_label_set_markup(status, text);
}

/* Helper to clear all children from a container */
static void relay_manager_clear_container(GtkWidget *container) {
  if (!container || !GTK_IS_WIDGET(container)) return;

  /* For GtkFlowBox, use gtk_flow_box_remove to properly handle GtkFlowBoxChild wrappers */
  if (GTK_IS_FLOW_BOX(container)) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(container)) != NULL) {
      gtk_flow_box_remove(GTK_FLOW_BOX(container), child);
    }
  } else if (GTK_IS_BOX(container)) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(container)) != NULL) {
      gtk_box_remove(GTK_BOX(container), child);
    }
  } else {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(container)) != NULL) {
      gtk_widget_unparent(child);
    }
  }
}

/* Helper to create a NIP badge button */
static GtkWidget *relay_manager_create_nip_badge(gint nip_num) {
  g_autofree gchar *label = g_strdup_printf("NIP-%02d", nip_num);
  GtkWidget *btn = gtk_button_new_with_label(label);

  gtk_widget_add_css_class(btn, "pill");
  gtk_widget_add_css_class(btn, "flat");
  gtk_widget_set_can_focus(btn, FALSE);

  /* Tooltip with NIP description for common NIPs */
  const gchar *tooltip = NULL;
  switch (nip_num) {
    case 1: tooltip = "Basic protocol flow"; break;
    case 2: tooltip = "Follow List"; break;
    case 4: tooltip = "Encrypted Direct Messages (deprecated)"; break;
    case 5: tooltip = "Event Deletion Request"; break;
    case 9: tooltip = "Event Deletion"; break;
    case 10: tooltip = "Conventions for clients' use of e and p tags"; break;
    case 11: tooltip = "Relay Information Document"; break;
    case 13: tooltip = "Proof of Work"; break;
    case 15: tooltip = "Nostr Marketplace"; break;
    case 17: tooltip = "Private Direct Messages"; break;
    case 20: tooltip = "Expiration"; break;
    case 22: tooltip = "Comment"; break;
    case 25: tooltip = "Reactions"; break;
    case 26: tooltip = "Delegated Event Signing"; break;
    case 28: tooltip = "Public Chat"; break;
    case 29: tooltip = "Relay-based Groups"; break;
    case 40: tooltip = "Relay Authentication"; break;
    case 42: tooltip = "Authentication of clients to relays"; break;
    case 44: tooltip = "Versioned encryption"; break;
    case 45: tooltip = "Counting results"; break;
    case 50: tooltip = "Search Capability"; break;
    case 51: tooltip = "Lists"; break;
    case 56: tooltip = "Reporting"; break;
    case 57: tooltip = "Lightning Zaps"; break;
    case 58: tooltip = "Badges"; break;
    case 59: tooltip = "Gift Wrap"; break;
    case 65: tooltip = "Relay List Metadata"; break;
    case 70: tooltip = "Protected Events"; break;
    case 78: tooltip = "Arbitrary custom app data"; break;
    case 89: tooltip = "Recommended Application Handlers"; break;
    case 90: tooltip = "Data Vending Machine"; break;
    case 94: tooltip = "File Metadata"; break;
    case 96: tooltip = "HTTP File Storage Integration"; break;
    case 98: tooltip = "HTTP Auth"; break;
    case 99: tooltip = "Classified Listings"; break;
  }
  if (tooltip) {
    g_autofree gchar *full_tooltip = g_strdup_printf("NIP-%02d: %s", nip_num, tooltip);
    gtk_widget_set_tooltip_text(btn, full_tooltip);
  }

  return btn;
}

/* Helper to create a warning badge */
static GtkWidget *relay_manager_create_warning_badge(const gchar *icon_name, const gchar *label, const gchar *tooltip) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(box, "warning");

  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_box_append(GTK_BOX(box), icon);

  GtkWidget *lbl = gtk_label_new(label);
  gtk_widget_add_css_class(lbl, "warning");
  gtk_box_append(GTK_BOX(box), lbl);

  if (tooltip) gtk_widget_set_tooltip_text(box, tooltip);

  return box;
}

/* Callback for copy pubkey button */
static void relay_manager_on_pubkey_copy(GtkButton *btn, gpointer user_data) {
  (void)btn;
  const gchar *pubkey = (const gchar*)user_data;
  if (!pubkey) return;

  GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
  gdk_clipboard_set_text(clipboard, pubkey);
}

static void relay_manager_populate_info(RelayManagerCtx *ctx, GnostrRelayInfo *info) {
  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
  if (!stack) return;

  /* Basic labels */
  GtkLabel *name = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_name"));
  GtkLabel *desc = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_description"));
  GtkLabel *software = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_software"));
  GtkLabel *contact = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_contact"));
  GtkLabel *limitations = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_limitations"));
  GtkLabel *pubkey = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_pubkey"));

  /* Enhanced UI elements */
  GtkWidget *warnings_box = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_warnings_box"));
  GtkWidget *nips_flowbox = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_nips_flowbox"));
  GtkWidget *nips_empty = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_nips_empty"));
  GtkWidget *contact_link = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_contact_link"));
  GtkWidget *pubkey_copy = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_pubkey_copy"));
  GtkWidget *policy_box = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_policy_box"));
  GtkWidget *posting_policy_link = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_posting_policy_link"));
  GtkWidget *payments_url_link = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_payments_url_link"));

  /* Populate basic fields */
  if (name) gtk_label_set_text(name, info->name ? info->name : "(not provided)");
  if (desc) gtk_label_set_text(desc, info->description ? info->description : "(not provided)");

  if (software) {
    g_autofree gchar *sw_text = info->software && info->version
      ? g_strdup_printf("%s v%s", info->software, info->version)
      : (info->software ? g_strdup(info->software) : g_strdup("(not provided)"));
    gtk_label_set_text(software, sw_text);
  }

  /* Contact with clickable link */
  if (contact) {
    gtk_label_set_text(contact, info->contact ? info->contact : "(not provided)");
  }
  if (contact_link) {
    if (info->contact && (g_str_has_prefix(info->contact, "mailto:") ||
                          g_str_has_prefix(info->contact, "http://") ||
                          g_str_has_prefix(info->contact, "https://"))) {
      gtk_link_button_set_uri(GTK_LINK_BUTTON(contact_link), info->contact);
      gtk_widget_set_visible(contact_link, TRUE);
    } else if (info->contact && strchr(info->contact, '@')) {
      /* Looks like an email without mailto: prefix */
      g_autofree gchar *mailto = g_strdup_printf("mailto:%s", info->contact);
      gtk_link_button_set_uri(GTK_LINK_BUTTON(contact_link), mailto);
      gtk_widget_set_visible(contact_link, TRUE);
    } else {
      gtk_widget_set_visible(contact_link, FALSE);
    }
  }

  /* Pubkey with copy button */
  if (pubkey) {
    if (info->pubkey) {
      /* Truncate display but keep full value for copy */
      gchar *truncated = g_strndup(info->pubkey, 16);
      g_autofree gchar *display = g_strdup_printf("%s...", truncated);
      gtk_label_set_text(pubkey, display);
      gtk_widget_set_tooltip_text(GTK_WIDGET(pubkey), info->pubkey);
      g_free(truncated);
    } else {
      gtk_label_set_text(pubkey, "(not provided)");
      gtk_widget_set_tooltip_text(GTK_WIDGET(pubkey), NULL);
    }
  }
  if (pubkey_copy) {
    if (info->pubkey) {
      gtk_widget_set_visible(pubkey_copy, TRUE);
      /* Store pubkey as object data for the callback */
      g_object_set_data_full(G_OBJECT(pubkey_copy), "pubkey", g_strdup(info->pubkey), g_free);
      g_signal_handlers_disconnect_by_func(pubkey_copy, G_CALLBACK(relay_manager_on_pubkey_copy), NULL);
      g_signal_connect(pubkey_copy, "clicked", G_CALLBACK(relay_manager_on_pubkey_copy),
                       g_object_get_data(G_OBJECT(pubkey_copy), "pubkey"));
    } else {
      gtk_widget_set_visible(pubkey_copy, FALSE);
    }
  }

  /* Populate NIP badges */
  if (nips_flowbox && GTK_IS_FLOW_BOX(nips_flowbox)) {
    relay_manager_clear_container(nips_flowbox);

    if (info->supported_nips && info->supported_nips_count > 0) {
      for (gsize i = 0; i < info->supported_nips_count; i++) {
        GtkWidget *badge = relay_manager_create_nip_badge(info->supported_nips[i]);
        if (badge && GTK_IS_WIDGET(badge)) {
          gtk_flow_box_append(GTK_FLOW_BOX(nips_flowbox), badge);
        }
      }
      gtk_widget_set_visible(nips_flowbox, TRUE);
      if (nips_empty) gtk_widget_set_visible(nips_empty, FALSE);
    } else {
      gtk_widget_set_visible(nips_flowbox, FALSE);
      if (nips_empty) gtk_widget_set_visible(nips_empty, TRUE);
    }
  }

  /* Limitations */
  if (limitations) {
    gchar *lim_str = gnostr_relay_info_format_limitations(info);
    gtk_label_set_text(limitations, lim_str);
    g_free(lim_str);
  }

  /* Warning indicators */
  if (warnings_box) {
    relay_manager_clear_container(warnings_box);
    gboolean has_warnings = FALSE;

    if (info->auth_required) {
      GtkWidget *badge = relay_manager_create_warning_badge("dialog-password-symbolic", "Auth Required",
        "This relay requires authentication (NIP-42). You may need to sign in to use it.");
      gtk_box_append(GTK_BOX(warnings_box), badge);
      has_warnings = TRUE;
    }

    if (info->payment_required) {
      GtkWidget *badge = relay_manager_create_warning_badge("emblem-money-symbolic", "Payment Required",
        "This relay requires payment to use.");
      gtk_box_append(GTK_BOX(warnings_box), badge);
      has_warnings = TRUE;
    }

    if (info->restricted_writes) {
      GtkWidget *badge = relay_manager_create_warning_badge("action-unavailable-symbolic", "Restricted Writes",
        "This relay has write restrictions. Not all events may be accepted.");
      gtk_box_append(GTK_BOX(warnings_box), badge);
      has_warnings = TRUE;
    }

    gtk_widget_set_visible(warnings_box, has_warnings);
  }

  /* Policy links */
  gboolean has_policy_links = FALSE;
  if (posting_policy_link) {
    if (info->posting_policy) {
      gtk_link_button_set_uri(GTK_LINK_BUTTON(posting_policy_link), info->posting_policy);
      gtk_widget_set_visible(posting_policy_link, TRUE);
      has_policy_links = TRUE;
    } else {
      gtk_widget_set_visible(posting_policy_link, FALSE);
    }
  }
  if (payments_url_link) {
    if (info->payments_url) {
      gtk_link_button_set_uri(GTK_LINK_BUTTON(payments_url_link), info->payments_url);
      gtk_widget_set_visible(payments_url_link, TRUE);
      has_policy_links = TRUE;
    } else {
      gtk_widget_set_visible(payments_url_link, FALSE);
    }
  }
  if (policy_box) {
    gtk_widget_set_visible(policy_box, has_policy_links);
  }

  /* NIP-65 Permission display */
  GtkWidget *nip65_icon = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "info_nip65_icon"));
  GtkLabel *nip65_label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_nip65_label"));
  if (nip65_icon && nip65_label && ctx->selected_url && ctx->relay_types) {
    gpointer type_ptr = g_hash_table_lookup(ctx->relay_types, ctx->selected_url);
    GnostrRelayType type = type_ptr ? GPOINTER_TO_INT(type_ptr) : GNOSTR_RELAY_READWRITE;
    const gchar *icon_name;
    const gchar *label_text;
    switch (type) {
      case GNOSTR_RELAY_READ:
        icon_name = "go-down-symbolic";
        label_text = "Read Only";
        break;
      case GNOSTR_RELAY_WRITE:
        icon_name = "go-up-symbolic";
        label_text = "Write Only";
        break;
      case GNOSTR_RELAY_READWRITE:
      default:
        icon_name = "network-transmit-receive-symbolic";
        label_text = "Read + Write";
        break;
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(nip65_icon), icon_name);
    gtk_label_set_text(nip65_label, label_text);
  }

  gtk_stack_set_visible_child_name(stack, "info");
}

static void on_relay_info_fetched(GObject *source, GAsyncResult *result, gpointer user_data) {
  (void)source;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;

  /* Always consume the result to avoid leaks */
  GError *err = NULL;
  GnostrRelayInfo *info = gnostr_relay_info_fetch_finish(result, &err);

  /* CRITICAL: Check if context was destroyed before accessing any GTK widgets.
   * The context is kept alive via ref counting until this callback completes. */
  if (!ctx || ctx->destroyed || !ctx->builder) {
    /* Context was destroyed - clean up and release our reference */
    if (info) gnostr_relay_info_free(info);
    g_clear_error(&err);
    if (ctx) relay_manager_ctx_unref(ctx);
    return;
  }

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
  if (!stack) {
    if (info) gnostr_relay_info_free(info);
    g_clear_error(&err);
    relay_manager_ctx_unref(ctx);
    return;
  }

  if (err) {
    GtkLabel *error_label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_error_label"));
    if (error_label) {
      g_autofree gchar *msg = g_strdup_printf("Failed to fetch relay info:\n%s", err->message);
      gtk_label_set_text(error_label, msg);
    }
    gtk_stack_set_visible_child_name(stack, "error");
    g_clear_error(&err);
    relay_manager_ctx_unref(ctx);
    return;
  }

  if (!info) {
    GtkLabel *error_label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "info_error_label"));
    if (error_label) gtk_label_set_text(error_label, "Failed to parse relay info");
    gtk_stack_set_visible_child_name(stack, "error");
    relay_manager_ctx_unref(ctx);
    return;
  }

  relay_manager_populate_info(ctx, info);
  gnostr_relay_info_free(info);
  relay_manager_ctx_unref(ctx);
}

static void relay_manager_fetch_info(RelayManagerCtx *ctx, const gchar *url) {
  if (!ctx || !url) return;

  /* Cancel any pending fetch - the callback will handle its own unref */
  if (ctx->fetch_cancellable) {
    g_cancellable_cancel(ctx->fetch_cancellable);
    g_object_unref(ctx->fetch_cancellable);
    /* Note: Don't unref here - the cancelled callback will do it */
  }
  ctx->fetch_cancellable = g_cancellable_new();

  /* Copy url first since it might be ctx->selected_url (from retry button) */
  gchar *url_copy = g_strdup(url);
  g_free(ctx->selected_url);
  ctx->selected_url = url_copy;

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
  if (stack) gtk_stack_set_visible_child_name(stack, "loading");

  /* Take a reference for the async callback */
  relay_manager_ctx_ref(ctx);
  gnostr_relay_info_fetch_async(ctx->selected_url, ctx->fetch_cancellable, on_relay_info_fetched, ctx);
}

static void on_relay_selection_changed(GtkSelectionModel *model, guint position, guint n_items, gpointer user_data) {
  (void)position; (void)n_items;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx) return;

  GtkSingleSelection *sel = GTK_SINGLE_SELECTION(model);
  guint selected_pos = gtk_single_selection_get_selected(sel);
  GtkStringObject *obj = GTK_STRING_OBJECT(gtk_single_selection_get_selected_item(sel));

  if (obj) {
    const gchar *url = gtk_string_object_get_string(obj);
    if (url && *url) {
      relay_manager_fetch_info(ctx, url);
    }
  } else {
    GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
    if (stack) gtk_stack_set_visible_child_name(stack, "empty");
  }
}

static void relay_manager_on_retry_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx || !ctx->selected_url) return;
  relay_manager_fetch_info(ctx, ctx->selected_url);
}

static void relay_manager_on_add_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx || !ctx->builder) return;

  GtkEntry *entry = GTK_ENTRY(gtk_builder_get_object(ctx->builder, "relay_entry"));
  if (!entry) return;

  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (!text || !*text) return;

  gchar *normalized = gnostr_normalize_relay_url(text);
  if (!normalized) {
    /* Invalid URL */
    return;
  }

  /* Check for duplicates */
  guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_model));
  for (guint i = 0; i < n; i++) {
    GtkStringObject *obj = GTK_STRING_OBJECT(g_list_model_get_item(G_LIST_MODEL(ctx->relay_model), i));
    if (obj) {
      const gchar *existing = gtk_string_object_get_string(obj);
      if (existing && g_strcmp0(existing, normalized) == 0) {
        g_object_unref(obj);
        g_free(normalized);
        return; /* Already exists */
      }
      g_object_unref(obj);
    }
  }

  gtk_string_list_append(ctx->relay_model, normalized);
  /* New relays default to read+write */
  g_hash_table_insert(ctx->relay_types, g_strdup(normalized), GINT_TO_POINTER(GNOSTR_RELAY_READWRITE));
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  ctx->modified = TRUE;
  relay_manager_update_status(ctx);
  g_free(normalized);
}

static void relay_manager_on_entry_activate(GtkEntry *entry, gpointer user_data) {
  relay_manager_on_add_clicked(NULL, user_data);
}

/* Per-row delete button handler */
static void on_relay_row_delete_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx *)user_data;
  if (!ctx || !ctx->relay_model) return;

  const gchar *url = g_object_get_data(G_OBJECT(btn), "relay_url");
  if (!url) return;

  /* Find and remove the relay by URL */
  guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_model));
  for (guint i = 0; i < n; i++) {
    GtkStringObject *obj = GTK_STRING_OBJECT(g_list_model_get_item(G_LIST_MODEL(ctx->relay_model), i));
    if (obj) {
      const gchar *item_url = gtk_string_object_get_string(obj);
      gboolean match = (item_url && g_strcmp0(item_url, url) == 0);
      g_object_unref(obj);
      if (match) {
        /* Remove from type hash table too */
        if (ctx->relay_types) {
          g_hash_table_remove(ctx->relay_types, url);
        }
        gtk_string_list_remove(ctx->relay_model, i);
        ctx->modified = TRUE;
        relay_manager_update_status(ctx);

        /* Clear info pane if we removed the selected relay */
        if (ctx->selected_url && g_strcmp0(ctx->selected_url, url) == 0) {
          GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
          if (stack) gtk_stack_set_visible_child_name(stack, "empty");
        }
        break;
      }
    }
  }
}

static void relay_manager_on_remove_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx || !ctx->selection) return;

  guint pos = gtk_single_selection_get_selected(ctx->selection);
  if (pos == GTK_INVALID_LIST_POSITION) return;

  gtk_string_list_remove(ctx->relay_model, pos);
  ctx->modified = TRUE;
  relay_manager_update_status(ctx);

  /* Clear info pane */
  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "info_stack"));
  if (stack) gtk_stack_set_visible_child_name(stack, "empty");
}

static void relay_manager_on_save_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx) return;

  /* Collect all relays with their types as NIP-65 entries */
  GPtrArray *relays = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip65_relay_free);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_model));
  for (guint i = 0; i < n; i++) {
    GtkStringObject *obj = GTK_STRING_OBJECT(g_list_model_get_item(G_LIST_MODEL(ctx->relay_model), i));
    if (obj) {
      const gchar *url = gtk_string_object_get_string(obj);
      if (url && *url) {
        GnostrNip65Relay *relay = g_new0(GnostrNip65Relay, 1);
        relay->url = g_strdup(url);
        /* Get type from hash table, default to READWRITE */
        gpointer stored = g_hash_table_lookup(ctx->relay_types, url);
        relay->type = stored ? GPOINTER_TO_INT(stored) : GNOSTR_RELAY_READWRITE;
        g_ptr_array_add(relays, relay);
      }
      g_object_unref(obj);
    }
  }

  gnostr_save_nip65_relays(relays);

  /* Publish NIP-65 relay list event to relays */
  g_debug("[RELAYS] Publishing NIP-65 relay list with %u relays", relays->len);
  gnostr_nip65_publish_async(relays, NULL, NULL);

  g_ptr_array_unref(relays);

  ctx->modified = FALSE;
  relay_manager_update_status(ctx);
  gtk_window_close(ctx->window);
}

static void relay_manager_on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx || !ctx->window) return;
  gtk_window_close(ctx->window);
}

/* Idle callback to refresh relay connection indicators on main thread */
static gboolean
relay_manager_refresh_connection_icons_idle(gpointer user_data)
{
  RelayManagerCtx *ctx = (RelayManagerCtx *)user_data;
  if (ctx->destroyed || !ctx->relay_model) {
    relay_manager_ctx_unref(ctx);
    return G_SOURCE_REMOVE;
  }

  /* Force the list to re-bind all items so connection icons pick up new state */
  guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_model));
  if (n > 0) {
    g_list_model_items_changed(G_LIST_MODEL(ctx->relay_model), 0, n, n);
  }

  relay_manager_ctx_unref(ctx);
  return G_SOURCE_REMOVE;
}

/* Called from worker thread when any relay in the pool changes state */
static void
on_pool_relay_state_changed_for_manager(GNostrPool       *pool,
                                        GNostrRelay      *relay,
                                        GNostrRelayState  new_state,
                                        gpointer          user_data)
{
  (void)pool; (void)relay; (void)new_state;
  RelayManagerCtx *ctx = (RelayManagerCtx *)user_data;
  if (ctx->destroyed) return;

  /* Schedule UI update on main thread */
  relay_manager_ctx_ref(ctx);
  g_idle_add(relay_manager_refresh_connection_icons_idle, ctx);
}

static void relay_manager_on_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx) return;

  /* Disconnect pool state-changed signal */
  if (ctx->pool_state_handler_id && ctx->main_window && ctx->main_window->pool) {
    g_signal_handler_disconnect(ctx->main_window->pool, ctx->pool_state_handler_id);
    ctx->pool_state_handler_id = 0;
  }

  /* Mark as destroyed so pending callbacks know not to access widgets */
  ctx->destroyed = TRUE;
  ctx->window = NULL;
  ctx->list_view = NULL;

  if (ctx->builder) {
    g_object_unref(ctx->builder);
    ctx->builder = NULL;
  }

  /* Cancel any pending fetch - this will cause the callback to be invoked
   * with a cancellation error, at which point it will see destroyed=TRUE
   * and clean up properly */
  if (ctx->fetch_cancellable) {
    g_cancellable_cancel(ctx->fetch_cancellable);
  }

  /* Free context now - the callback checks destroyed flag before accessing fields */
  relay_manager_ctx_free(ctx);
}

/* ============== NIP-66 Relay Discovery Dialog ============== */

typedef struct {
  GtkWindow *window;
  GtkBuilder *builder;
  GtkListStore *list_store;
  GtkSingleSelection *selection;
  GCancellable *cancellable;
  GPtrArray *discovered_relays;  /* GnostrNip66RelayMeta* array */
  GHashTable *selected_urls;  /* URLs selected for addition */
  GHashTable *seen_urls;  /* URLs already added (for deduplication) */
  RelayManagerCtx *relay_manager_ctx;  /* Parent relay manager context */
  guint filter_timeout_id;  /* Debounce timer for filter updates */
  gboolean filter_pending;  /* Flag indicating filter update is pending */
} RelayDiscoveryCtx;

static void relay_discovery_ctx_free(RelayDiscoveryCtx *ctx) {
  if (!ctx) return;
  if (ctx->filter_timeout_id) {
    g_source_remove(ctx->filter_timeout_id);
    ctx->filter_timeout_id = 0;
  }
  if (ctx->cancellable) {
    g_cancellable_cancel(ctx->cancellable);
    g_object_unref(ctx->cancellable);
  }
  if (ctx->discovered_relays) {
    g_ptr_array_unref(ctx->discovered_relays);
  }
  if (ctx->selected_urls) {
    g_hash_table_destroy(ctx->selected_urls);
  }
  if (ctx->seen_urls) {
    g_hash_table_destroy(ctx->seen_urls);
  }
  g_free(ctx);
}

/* Map dropdown selections to filter values */
static const gchar *s_region_values[] = {
  NULL,  /* All Regions */
  "North America",
  "Europe",
  "Asia Pacific",
  "South America",
  "Middle East",
  "Africa",
  "Other"
};

static const gint s_nip_values[] = {
  0,   /* Any NIPs */
  1,   /* NIP-01 */
  11,  /* NIP-11 */
  17,  /* NIP-17 */
  42,  /* NIP-42 */
  50,  /* NIP-50 */
  57,  /* NIP-57 */
  59,  /* NIP-59 */
  65   /* NIP-65 */
};

/* Forward declaration */
static void relay_discovery_apply_filter(RelayDiscoveryCtx *ctx);

static void relay_discovery_update_results_label(RelayDiscoveryCtx *ctx, guint count) {
  GtkLabel *label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "results_label"));
  if (label) {
    g_autofree gchar *text = g_strdup_printf("<small>Found %u relay%s</small>", count, count == 1 ? "" : "s");
    gtk_label_set_markup(label, text);
  }
}

/* Row widget references for discovery list */
typedef struct {
  GtkWidget *check;
  GtkWidget *name_label;
  GtkWidget *url_label;
  GtkWidget *region_label;
  GtkWidget *status_icon;
  GtkWidget *nips_label;
  GtkWidget *uptime_label;
  GtkWidget *latency_label;
} DiscoveryRowWidgets;

static void relay_discovery_setup_factory_cb(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory; (void)user_data;

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(row, 6);
  gtk_widget_set_margin_bottom(row, 6);
  gtk_widget_set_margin_start(row, 8);
  gtk_widget_set_margin_end(row, 8);

  /* Checkbox for selection */
  GtkWidget *check = gtk_check_button_new();
  gtk_box_append(GTK_BOX(row), check);

  /* Status icon */
  GtkWidget *status_icon = gtk_image_new_from_icon_name("network-transmit-receive-symbolic");
  gtk_widget_set_size_request(status_icon, 16, 16);
  gtk_box_append(GTK_BOX(row), status_icon);

  /* Main content */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(content, TRUE);

  GtkWidget *name_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(name_label, "heading");
  gtk_box_append(GTK_BOX(content), name_label);

  GtkWidget *url_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(url_label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(url_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class(url_label, "dim-label");
  gtk_widget_add_css_class(url_label, "caption");
  gtk_box_append(GTK_BOX(content), url_label);

  GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(info_box, 2);

  GtkWidget *region_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(region_label, "caption");
  gtk_widget_add_css_class(region_label, "dim-label");
  gtk_box_append(GTK_BOX(info_box), region_label);

  GtkWidget *nips_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(nips_label, "caption");
  gtk_widget_add_css_class(nips_label, "dim-label");
  gtk_box_append(GTK_BOX(info_box), nips_label);

  gtk_box_append(GTK_BOX(content), info_box);
  gtk_box_append(GTK_BOX(row), content);

  /* Stats column */
  GtkWidget *stats = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_halign(stats, GTK_ALIGN_END);

  GtkWidget *uptime_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(uptime_label, "caption");
  gtk_box_append(GTK_BOX(stats), uptime_label);

  GtkWidget *latency_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(latency_label, "caption");
  gtk_widget_add_css_class(latency_label, "dim-label");
  gtk_box_append(GTK_BOX(stats), latency_label);

  gtk_box_append(GTK_BOX(row), stats);

  /* Store widget references */
  DiscoveryRowWidgets *widgets = g_new0(DiscoveryRowWidgets, 1);
  widgets->check = check;
  widgets->name_label = name_label;
  widgets->url_label = url_label;
  widgets->region_label = region_label;
  widgets->status_icon = status_icon;
  widgets->nips_label = nips_label;
  widgets->uptime_label = uptime_label;
  widgets->latency_label = latency_label;
  g_object_set_data_full(G_OBJECT(row), "widgets", widgets, g_free);

  gtk_list_item_set_child(list_item, row);
}

static void on_discovery_check_toggled(GtkCheckButton *check, gpointer user_data);

static void relay_discovery_bind_factory_cb(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  GtkWidget *row = gtk_list_item_get_child(list_item);
  GObject *item = gtk_list_item_get_item(list_item);

  if (!row || !item) return;

  DiscoveryRowWidgets *widgets = g_object_get_data(G_OBJECT(row), "widgets");
  if (!widgets) return;

  /* nostrc-dth1: Get relay URL from the GtkStringObject in the filtered list,
   * then look up the meta by URL. Using position was wrong because the list
   * is filtered - positions don't match the original discovered_relays array. */
  const gchar *relay_url = gtk_string_object_get_string(GTK_STRING_OBJECT(item));
  if (!relay_url) return;

  /* Find the meta for this URL */
  GnostrNip66RelayMeta *meta = NULL;
  for (guint i = 0; i < ctx->discovered_relays->len; i++) {
    GnostrNip66RelayMeta *m = g_ptr_array_index(ctx->discovered_relays, i);
    if (m && m->relay_url && g_strcmp0(m->relay_url, relay_url) == 0) {
      meta = m;
      break;
    }
  }
  if (!meta) return;

  /* Bind data */
  if (meta->name && *meta->name) {
    gtk_label_set_text(GTK_LABEL(widgets->name_label), meta->name);
  } else {
    /* Use hostname from URL */
    gchar *hostname = NULL;
    if (meta->relay_url) {
      const gchar *start = meta->relay_url;
      if (g_str_has_prefix(start, "wss://")) start += 6;
      else if (g_str_has_prefix(start, "ws://")) start += 5;
      const gchar *end = start;
      while (*end && *end != '/' && *end != ':') end++;
      hostname = g_strndup(start, end - start);
    }
    gtk_label_set_text(GTK_LABEL(widgets->name_label), hostname ? hostname : "(unknown)");
    g_free(hostname);
  }

  gtk_label_set_text(GTK_LABEL(widgets->url_label), meta->relay_url ? meta->relay_url : "");

  /* Region */
  g_autofree gchar *region_text = g_strdup_printf("%s%s%s",
    meta->region ? meta->region : "",
    (meta->region && meta->country_code) ? " " : "",
    meta->country_code ? meta->country_code : "");
  gtk_label_set_text(GTK_LABEL(widgets->region_label), region_text);

  /* NIPs summary */
  if (meta->supported_nips_count > 0) {
    g_autofree gchar *nips_text = g_strdup_printf("%zu NIPs", meta->supported_nips_count);
    gtk_label_set_text(GTK_LABEL(widgets->nips_label), nips_text);
  } else {
    gtk_label_set_text(GTK_LABEL(widgets->nips_label), "");
  }

  /* Status */
  if (meta->is_online) {
    gtk_image_set_from_icon_name(GTK_IMAGE(widgets->status_icon), "network-transmit-receive-symbolic");
    gtk_widget_remove_css_class(widgets->status_icon, "error");
    gtk_widget_add_css_class(widgets->status_icon, "success");
    gtk_widget_set_tooltip_text(widgets->status_icon, "Online");
  } else {
    gtk_image_set_from_icon_name(GTK_IMAGE(widgets->status_icon), "network-offline-symbolic");
    gtk_widget_remove_css_class(widgets->status_icon, "success");
    gtk_widget_add_css_class(widgets->status_icon, "error");
    gtk_widget_set_tooltip_text(widgets->status_icon, "Offline");
  }

  /* Uptime */
  gchar *uptime_str = gnostr_nip66_format_uptime(meta->uptime_percent);
  gtk_label_set_text(GTK_LABEL(widgets->uptime_label), uptime_str);
  if (meta->uptime_percent >= 99.0) {
    gtk_widget_add_css_class(widgets->uptime_label, "success");
  } else if (meta->uptime_percent >= 90.0) {
    gtk_widget_remove_css_class(widgets->uptime_label, "success");
  } else {
    gtk_widget_add_css_class(widgets->uptime_label, "warning");
  }
  g_free(uptime_str);

  /* Latency */
  gchar *latency_str = gnostr_nip66_format_latency(meta->latency_ms);
  gtk_label_set_text(GTK_LABEL(widgets->latency_label), latency_str);
  g_free(latency_str);

  /* Checkbox state */
  gboolean is_selected = ctx->selected_urls &&
    g_hash_table_contains(ctx->selected_urls, meta->relay_url);
  g_signal_handlers_block_by_func(widgets->check, G_CALLBACK(on_discovery_check_toggled), ctx);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->check), is_selected);
  g_signal_handlers_unblock_by_func(widgets->check, G_CALLBACK(on_discovery_check_toggled), ctx);

  /* Store URL for checkbox callback */
  g_object_set_data_full(G_OBJECT(widgets->check), "relay_url",
    g_strdup(meta->relay_url), g_free);

  /* Connect checkbox signal */
  g_signal_handlers_disconnect_by_func(widgets->check, G_CALLBACK(on_discovery_check_toggled), ctx);
  g_signal_connect(widgets->check, "toggled", G_CALLBACK(on_discovery_check_toggled), ctx);
}

static void on_discovery_check_toggled(GtkCheckButton *check, gpointer user_data) {
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (!ctx || !ctx->selected_urls) return;

  const gchar *url = g_object_get_data(G_OBJECT(check), "relay_url");
  if (!url) return;

  gboolean active = gtk_check_button_get_active(check);
  if (active) {
    g_hash_table_add(ctx->selected_urls, g_strdup(url));
  } else {
    g_hash_table_remove(ctx->selected_urls, url);
  }

  /* Update Add Selected button sensitivity */
  GtkWidget *btn = GTK_WIDGET(gtk_builder_get_object(ctx->builder, "btn_add_selected"));
  if (btn) {
    guint count = g_hash_table_size(ctx->selected_urls);
    gtk_widget_set_sensitive(btn, count > 0);
    g_autofree gchar *label = count > 0 ? g_strdup_printf("_Add %u Selected", count) : g_strdup("_Add Selected");
    gtk_button_set_label(GTK_BUTTON(btn), label);
  }
}

/* Debounce callback for filter updates during streaming */
static gboolean relay_discovery_debounced_filter(gpointer user_data) {
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (!ctx) return G_SOURCE_REMOVE;

  ctx->filter_timeout_id = 0;
  ctx->filter_pending = FALSE;
  relay_discovery_apply_filter(ctx);
  return G_SOURCE_REMOVE;
}

/* LEGITIMATE TIMEOUT - Debounced filter update (100ms delay).
 * Batches rapid relay discovery events into single UI updates.
 * nostrc-b0h: Audited - debounce for batching is appropriate. */
static void relay_discovery_schedule_filter_update(RelayDiscoveryCtx *ctx) {
  if (!ctx) return;

  ctx->filter_pending = TRUE;

  /* Reset timer if already scheduled (restartable debounce) */
  if (ctx->filter_timeout_id) {
    g_source_remove(ctx->filter_timeout_id);
  }

  ctx->filter_timeout_id = g_timeout_add(100, relay_discovery_debounced_filter, ctx);
}

/* Streaming callback: called for each relay as it's discovered */
static void relay_discovery_on_relay_found(GnostrNip66RelayMeta *meta, gpointer user_data) {
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (!ctx || !ctx->builder || !meta || !meta->relay_url) return;

  /* Deduplicate by URL at UI level (belt-and-suspenders with streaming dedup) */
  if (ctx->seen_urls) {
    gchar *url_lower = g_ascii_strdown(meta->relay_url, -1);
    if (g_hash_table_contains(ctx->seen_urls, url_lower)) {
      g_free(url_lower);
      return;  /* Skip duplicate */
    }
    g_hash_table_add(ctx->seen_urls, url_lower);  /* Takes ownership */
  }

  /* Copy the meta since we need to own it */
  GnostrNip66RelayMeta *meta_copy = g_new0(GnostrNip66RelayMeta, 1);
  meta_copy->relay_url = g_strdup(meta->relay_url);
  meta_copy->name = meta->name ? g_strdup(meta->name) : NULL;
  meta_copy->description = meta->description ? g_strdup(meta->description) : NULL;
  meta_copy->region = meta->region ? g_strdup(meta->region) : NULL;
  meta_copy->country_code = meta->country_code ? g_strdup(meta->country_code) : NULL;
  meta_copy->software = meta->software ? g_strdup(meta->software) : NULL;
  meta_copy->version = meta->version ? g_strdup(meta->version) : NULL;
  meta_copy->has_status = meta->has_status;
  meta_copy->is_online = meta->is_online;
  meta_copy->payment_required = meta->payment_required;
  meta_copy->auth_required = meta->auth_required;
  meta_copy->uptime_percent = meta->uptime_percent;
  meta_copy->latency_ms = meta->latency_ms;
  meta_copy->network = meta->network;
  if (meta->supported_nips && meta->supported_nips_count > 0) {
    meta_copy->supported_nips = g_new(gint, meta->supported_nips_count);
    memcpy(meta_copy->supported_nips, meta->supported_nips, meta->supported_nips_count * sizeof(gint));
    meta_copy->supported_nips_count = meta->supported_nips_count;
  }

  /* Add to discovered relays */
  g_ptr_array_add(ctx->discovered_relays, meta_copy);

  /* Switch from loading to results on first relay */
  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "discovery_stack"));
  if (stack) {
    const gchar *current = gtk_stack_get_visible_child_name(stack);
    if (g_strcmp0(current, "loading") == 0) {
      gtk_stack_set_visible_child_name(stack, "results");
    }
  }

  /* Schedule debounced filter update instead of updating immediately */
  relay_discovery_schedule_filter_update(ctx);
}

static void relay_discovery_on_complete(GPtrArray *relays, GPtrArray *monitors,
                                         GError *error, gpointer user_data) {
  (void)relays;  /* We already have relays from streaming callbacks */
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (!ctx || !ctx->builder) return;

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "discovery_stack"));
  if (!stack) return;

  if (error) {
    GtkLabel *err_label = GTK_LABEL(gtk_builder_get_object(ctx->builder, "error_label"));
    if (err_label) gtk_label_set_text(err_label, error->message);
    gtk_stack_set_visible_child_name(stack, "error");
    g_error_free(error);
    return;
  }

  /* Cancel any pending debounced update */
  if (ctx->filter_timeout_id) {
    g_source_remove(ctx->filter_timeout_id);
    ctx->filter_timeout_id = 0;
  }

  /* Final update - apply filter immediately */
  relay_discovery_apply_filter(ctx);

  /* If no relays found, show empty state */
  if (ctx->discovered_relays->len == 0) {
    gtk_stack_set_visible_child_name(stack, "empty");
  }

  /* Clean up monitors array */
  if (monitors) g_ptr_array_unref(monitors);
}

static void relay_discovery_apply_filter(RelayDiscoveryCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "discovery_stack"));
  if (!stack) return;

  /* Get filter values */
  GtkDropDown *region_dd = GTK_DROP_DOWN(gtk_builder_get_object(ctx->builder, "filter_region"));
  GtkDropDown *nip_dd = GTK_DROP_DOWN(gtk_builder_get_object(ctx->builder, "filter_nip"));
  GtkCheckButton *online_check = GTK_CHECK_BUTTON(gtk_builder_get_object(ctx->builder, "filter_online"));
  GtkCheckButton *free_check = GTK_CHECK_BUTTON(gtk_builder_get_object(ctx->builder, "filter_free"));

  guint region_idx = region_dd ? gtk_drop_down_get_selected(region_dd) : 0;
  guint nip_idx = nip_dd ? gtk_drop_down_get_selected(nip_dd) : 0;
  gboolean online_only = online_check ? gtk_check_button_get_active(online_check) : TRUE;
  gboolean free_only = free_check ? gtk_check_button_get_active(free_check) : FALSE;

  const gchar *region_filter = (region_idx < G_N_ELEMENTS(s_region_values)) ?
    s_region_values[region_idx] : NULL;
  gint nip_filter = (nip_idx < G_N_ELEMENTS(s_nip_values)) ?
    s_nip_values[nip_idx] : 0;

  /* Build filtered list model */
  GtkStringList *filtered_model = gtk_string_list_new(NULL);
  guint match_count = 0;

  for (guint i = 0; i < ctx->discovered_relays->len; i++) {
    GnostrNip66RelayMeta *meta = g_ptr_array_index(ctx->discovered_relays, i);
    if (!meta || !meta->relay_url) continue;

    gboolean matches = TRUE;

    /* Online filter - only exclude relays explicitly marked offline.
     * Treat unknown status (no l tag) as possibly online. */
    if (online_only && meta->has_status && !meta->is_online) matches = FALSE;

    /* Free filter */
    if (free_only && meta->payment_required) matches = FALSE;

    /* Region filter */
    if (region_filter && *region_filter) {
      if (!meta->region || g_ascii_strcasecmp(meta->region, region_filter) != 0) {
        matches = FALSE;
      }
    }

    /* NIP filter */
    if (nip_filter > 0) {
      if (!gnostr_nip66_relay_supports_nip(meta, nip_filter)) {
        matches = FALSE;
      }
    }

    if (matches) {
      gtk_string_list_append(filtered_model, meta->relay_url);
      match_count++;
    }
  }

  /* Update list view */
  GtkListView *list_view = GTK_LIST_VIEW(gtk_builder_get_object(ctx->builder, "relay_list"));
  if (list_view) {
    GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(filtered_model));
    gtk_single_selection_set_autoselect(selection, FALSE);
    gtk_single_selection_set_can_unselect(selection, TRUE);

    /* Create factory if needed */
    if (!gtk_list_view_get_factory(list_view)) {
      GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
      g_signal_connect(factory, "setup", G_CALLBACK(relay_discovery_setup_factory_cb), ctx);
      g_signal_connect(factory, "bind", G_CALLBACK(relay_discovery_bind_factory_cb), ctx);
      gtk_list_view_set_factory(list_view, factory);
      g_object_unref(factory);
    }

    gtk_list_view_set_model(list_view, GTK_SELECTION_MODEL(selection));
    g_object_unref(selection);
  }

  /* Update results label and show appropriate page */
  relay_discovery_update_results_label(ctx, match_count);

  if (match_count > 0) {
    gtk_stack_set_visible_child_name(stack, "results");
  } else if (ctx->discovered_relays->len > 0) {
    /* Have relays but none match filter */
    gtk_stack_set_visible_child_name(stack, "empty");
  } else {
    gtk_stack_set_visible_child_name(stack, "empty");
  }
}

static void relay_discovery_start_fetch(RelayDiscoveryCtx *ctx) {
  if (!ctx || !ctx->builder) return;

  GtkStack *stack = GTK_STACK(gtk_builder_get_object(ctx->builder, "discovery_stack"));
  if (stack) gtk_stack_set_visible_child_name(stack, "loading");

  /* Cancel any pending filter update */
  if (ctx->filter_timeout_id) {
    g_source_remove(ctx->filter_timeout_id);
    ctx->filter_timeout_id = 0;
  }

  /* Cancel any pending operation */
  if (ctx->cancellable) {
    g_cancellable_cancel(ctx->cancellable);
    g_object_unref(ctx->cancellable);
  }
  ctx->cancellable = g_cancellable_new();

  /* Clear previous results for fresh fetch */
  if (ctx->discovered_relays) {
    g_ptr_array_set_size(ctx->discovered_relays, 0);
  }

  /* Clear seen URLs for deduplication */
  if (ctx->seen_urls) {
    g_hash_table_remove_all(ctx->seen_urls);
  }

  /* Start streaming discovery - relays appear as they're discovered */
  gnostr_nip66_discover_relays_streaming_async(
    relay_discovery_on_relay_found,
    relay_discovery_on_complete,
    ctx,
    ctx->cancellable);
}

static void relay_discovery_on_filter_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)obj; (void)pspec;
  relay_discovery_apply_filter((RelayDiscoveryCtx*)user_data);
}

static void relay_discovery_on_check_toggled(GtkCheckButton *btn, gpointer user_data) {
  (void)btn;
  relay_discovery_apply_filter((RelayDiscoveryCtx*)user_data);
}

static void relay_discovery_on_refresh_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  relay_discovery_start_fetch((RelayDiscoveryCtx*)user_data);
}

static void relay_discovery_on_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (ctx && ctx->window) gtk_window_close(ctx->window);
}

static void relay_discovery_on_add_selected_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (!ctx || !ctx->selected_urls || !ctx->relay_manager_ctx) return;

  GHashTableIter iter;
  gpointer key, value;
  guint added = 0;

  g_hash_table_iter_init(&iter, ctx->selected_urls);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const gchar *url = (const gchar*)key;
    if (!url || !*url) continue;

    /* Check if already in relay manager */
    gboolean exists = FALSE;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_manager_ctx->relay_model));
    for (guint i = 0; i < n; i++) {
      GtkStringObject *obj = GTK_STRING_OBJECT(g_list_model_get_item(
        G_LIST_MODEL(ctx->relay_manager_ctx->relay_model), i));
      if (obj) {
        const gchar *existing = gtk_string_object_get_string(obj);
        if (existing && g_ascii_strcasecmp(existing, url) == 0) {
          exists = TRUE;
        }
        g_object_unref(obj);
        if (exists) break;
      }
    }

    if (!exists) {
      gtk_string_list_append(ctx->relay_manager_ctx->relay_model, url);
      g_hash_table_insert(ctx->relay_manager_ctx->relay_types,
        g_strdup(url), GINT_TO_POINTER(GNOSTR_RELAY_READWRITE));
      added++;
    }
  }

  if (added > 0) {
    ctx->relay_manager_ctx->modified = TRUE;
    relay_manager_update_status(ctx->relay_manager_ctx);
  }

  /* Close discovery dialog */
  if (ctx->window) gtk_window_close(ctx->window);
}

static void relay_discovery_on_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  RelayDiscoveryCtx *ctx = (RelayDiscoveryCtx*)user_data;
  if (ctx && ctx->builder) {
    g_object_unref(ctx->builder);
    ctx->builder = NULL;
  }
  relay_discovery_ctx_free(ctx);
}

static void relay_manager_on_discover_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  RelayManagerCtx *manager_ctx = (RelayManagerCtx*)user_data;
  if (!manager_ctx) return;

  GtkBuilder *builder = gtk_builder_new_from_resource("/org/gnostr/ui/ui/dialogs/gnostr-relay-discovery.ui");
  if (!builder) {
    g_warning("Failed to load relay discovery UI");
    return;
  }

  GtkWindow *win = GTK_WINDOW(gtk_builder_get_object(builder, "relay_discovery_window"));
  if (!win) {
    g_object_unref(builder);
    return;
  }

  gtk_window_set_transient_for(win, manager_ctx->window);
  gtk_window_set_modal(win, TRUE);

  /* Create discovery context */
  RelayDiscoveryCtx *ctx = g_new0(RelayDiscoveryCtx, 1);
  ctx->window = win;
  ctx->builder = builder;
  ctx->relay_manager_ctx = manager_ctx;
  ctx->selected_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  ctx->seen_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  ctx->discovered_relays = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gnostr_nip66_relay_meta_free);

  /* Wire filter controls */
  GtkDropDown *region_dd = GTK_DROP_DOWN(gtk_builder_get_object(builder, "filter_region"));
  GtkDropDown *nip_dd = GTK_DROP_DOWN(gtk_builder_get_object(builder, "filter_nip"));
  GtkCheckButton *online_check = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "filter_online"));
  GtkCheckButton *free_check = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "filter_free"));

  if (region_dd) g_signal_connect(region_dd, "notify::selected",
    G_CALLBACK(relay_discovery_on_filter_changed), ctx);
  if (nip_dd) g_signal_connect(nip_dd, "notify::selected",
    G_CALLBACK(relay_discovery_on_filter_changed), ctx);
  if (online_check) g_signal_connect(online_check, "toggled",
    G_CALLBACK(relay_discovery_on_check_toggled), ctx);
  if (free_check) g_signal_connect(free_check, "toggled",
    G_CALLBACK(relay_discovery_on_check_toggled), ctx);

  /* Wire buttons */
  GtkWidget *btn_refresh = GTK_WIDGET(gtk_builder_get_object(builder, "btn_refresh"));
  GtkWidget *btn_refresh_empty = GTK_WIDGET(gtk_builder_get_object(builder, "btn_refresh_empty"));
  GtkWidget *btn_retry = GTK_WIDGET(gtk_builder_get_object(builder, "btn_retry"));
  GtkWidget *btn_close = GTK_WIDGET(gtk_builder_get_object(builder, "btn_close"));
  GtkWidget *btn_add_selected = GTK_WIDGET(gtk_builder_get_object(builder, "btn_add_selected"));

  if (btn_refresh) g_signal_connect(btn_refresh, "clicked",
    G_CALLBACK(relay_discovery_on_refresh_clicked), ctx);
  if (btn_refresh_empty) g_signal_connect(btn_refresh_empty, "clicked",
    G_CALLBACK(relay_discovery_on_refresh_clicked), ctx);
  if (btn_retry) g_signal_connect(btn_retry, "clicked",
    G_CALLBACK(relay_discovery_on_refresh_clicked), ctx);
  if (btn_close) g_signal_connect(btn_close, "clicked",
    G_CALLBACK(relay_discovery_on_close_clicked), ctx);
  if (btn_add_selected) g_signal_connect(btn_add_selected, "clicked",
    G_CALLBACK(relay_discovery_on_add_selected_clicked), ctx);

  g_signal_connect(win, "destroy", G_CALLBACK(relay_discovery_on_destroy), ctx);

  /* Start initial fetch */
  relay_discovery_start_fetch(ctx);

  gtk_window_present(win);
}

/* Structure to hold row widget references */
typedef struct {
  GtkWidget *name_label;
  GtkWidget *url_label;
  GtkWidget *status_icon;
  GtkWidget *connection_icon;  /* Live connection status indicator */
  GtkWidget *nips_box;
  GtkWidget *warning_icon;
  GtkWidget *type_dropdown;
  GtkWidget *type_icon;
  GtkWidget *delete_button;    /* Per-row delete button */
} RelayRowWidgets;

static void relay_manager_setup_factory_cb(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory; (void)user_data;

  /* Create a more sophisticated row layout */
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  /* Connection status indicator (live connection) */
  GtkWidget *connection_icon = gtk_image_new_from_icon_name("network-offline-symbolic");
  gtk_widget_set_size_request(connection_icon, 16, 16);
  gtk_widget_add_css_class(connection_icon, "dim-label");
  gtk_widget_set_tooltip_text(connection_icon, "Not connected");
  gtk_box_append(GTK_BOX(row), connection_icon);

  /* Status indicator (relay info fetch status) */
  GtkWidget *status_icon = gtk_image_new_from_icon_name("network-offline-symbolic");
  gtk_widget_set_size_request(status_icon, 16, 16);
  gtk_widget_add_css_class(status_icon, "dim-label");
  gtk_box_append(GTK_BOX(row), status_icon);

  /* Main content box (name + url + nips) */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(content, TRUE);

  /* Name label (primary) */
  GtkWidget *name_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(name_label, "heading");
  gtk_box_append(GTK_BOX(content), name_label);

  /* URL label (secondary, smaller) */
  GtkWidget *url_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(url_label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(url_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class(url_label, "dim-label");
  gtk_widget_add_css_class(url_label, "caption");
  gtk_box_append(GTK_BOX(content), url_label);

  /* NIP badges row (key NIPs only) */
  GtkWidget *nips_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_margin_top(nips_box, 2);
  gtk_box_append(GTK_BOX(content), nips_box);

  gtk_box_append(GTK_BOX(row), content);

  /* Type indicator icon (shows R/W/RW) */
  GtkWidget *type_icon = gtk_image_new_from_icon_name("network-transmit-receive-symbolic");
  gtk_widget_set_size_request(type_icon, 16, 16);
  gtk_widget_set_tooltip_text(type_icon, "Read + Write");
  gtk_box_append(GTK_BOX(row), type_icon);

  /* Type dropdown (Read/Write/Both) */
  const char *type_options[] = {"R+W", "Read", "Write", NULL};
  GtkWidget *type_dropdown = gtk_drop_down_new_from_strings(type_options);
  gtk_widget_set_size_request(type_dropdown, 80, -1);
  gtk_widget_set_valign(type_dropdown, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(type_dropdown, "Relay permission: Read+Write, Read-only, or Write-only");
  gtk_box_append(GTK_BOX(row), type_dropdown);

  /* Warning icon (for auth/payment required) */
  GtkWidget *warning_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
  gtk_widget_set_visible(warning_icon, FALSE);
  gtk_widget_add_css_class(warning_icon, "warning");
  gtk_box_append(GTK_BOX(row), warning_icon);

  /* Per-row delete button */
  GtkWidget *delete_button = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_add_css_class(delete_button, "flat");
  gtk_widget_add_css_class(delete_button, "circular");
  gtk_widget_set_valign(delete_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(delete_button, "Remove this relay");
  gtk_box_append(GTK_BOX(row), delete_button);

  /* Store widget references for binding */
  RelayRowWidgets *widgets = g_new0(RelayRowWidgets, 1);
  widgets->name_label = name_label;
  widgets->url_label = url_label;
  widgets->status_icon = status_icon;
  widgets->connection_icon = connection_icon;
  widgets->nips_box = nips_box;
  widgets->warning_icon = warning_icon;
  widgets->type_dropdown = type_dropdown;
  widgets->type_icon = type_icon;
  widgets->delete_button = delete_button;
  g_object_set_data_full(G_OBJECT(row), "widgets", widgets, g_free);

  gtk_list_item_set_child(list_item, row);
}

/* Helper to extract hostname from URL for display */
static gchar *relay_manager_extract_hostname(const gchar *url) {
  if (!url) return NULL;

  /* Skip protocol prefix */
  const gchar *start = url;
  if (g_str_has_prefix(url, "wss://")) {
    start = url + 6;
  } else if (g_str_has_prefix(url, "ws://")) {
    start = url + 5;
  }

  /* Find end of hostname (before path or port) */
  const gchar *end = start;
  while (*end && *end != '/' && *end != ':') {
    end++;
  }

  return g_strndup(start, end - start);
}

/* Add a small NIP badge to the nips box */
static void relay_manager_add_small_nip_badge(GtkWidget *box, gint nip) {
  g_autofree gchar *text = g_strdup_printf("%d", nip);
  GtkWidget *badge = gtk_label_new(text);

  gtk_widget_add_css_class(badge, "caption");
  gtk_widget_add_css_class(badge, "pill");
  gtk_widget_add_css_class(badge, "accent");

  /* Tooltip for NIP description */
  g_autofree gchar *tooltip = g_strdup_printf("NIP-%02d", nip);
  gtk_widget_set_tooltip_text(badge, tooltip);

  gtk_box_append(GTK_BOX(box), badge);
}

/* Helper to convert dropdown index to GnostrRelayType */
static GnostrRelayType relay_type_from_dropdown(guint index) {
  switch (index) {
    case 0: return GNOSTR_RELAY_READWRITE;
    case 1: return GNOSTR_RELAY_READ;
    case 2: return GNOSTR_RELAY_WRITE;
    default: return GNOSTR_RELAY_READWRITE;
  }
}

/* Helper to convert GnostrRelayType to dropdown index */
static guint relay_type_to_dropdown(GnostrRelayType type) {
  switch (type) {
    case GNOSTR_RELAY_READ: return 1;
    case GNOSTR_RELAY_WRITE: return 2;
    case GNOSTR_RELAY_READWRITE:
    default: return 0;
  }
}

/* Helper to update type icon based on relay type */
static void relay_manager_update_type_icon(GtkWidget *icon, GnostrRelayType type) {
  const gchar *icon_name;
  const gchar *tooltip;

  switch (type) {
    case GNOSTR_RELAY_READ:
      icon_name = "go-down-symbolic";
      tooltip = "Read-only (subscribe from this relay)";
      break;
    case GNOSTR_RELAY_WRITE:
      icon_name = "go-up-symbolic";
      tooltip = "Write-only (publish to this relay)";
      break;
    case GNOSTR_RELAY_READWRITE:
    default:
      icon_name = "network-transmit-receive-symbolic";
      tooltip = "Read + Write (subscribe and publish)";
      break;
  }

  gtk_image_set_from_icon_name(GTK_IMAGE(icon), icon_name);
  gtk_widget_set_tooltip_text(icon, tooltip);
}

/* Callback for type dropdown change */
static void on_relay_type_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  if (!ctx) return;

  const gchar *url = g_object_get_data(G_OBJECT(dropdown), "relay_url");
  if (!url) return;

  guint selected = gtk_drop_down_get_selected(dropdown);
  GnostrRelayType type = relay_type_from_dropdown(selected);

  /* Store in hash table */
  g_hash_table_replace(ctx->relay_types, g_strdup(url), GINT_TO_POINTER(type));
  ctx->modified = TRUE;
  relay_manager_update_status(ctx);

  /* Update the type icon */
  GtkWidget *type_icon = g_object_get_data(G_OBJECT(dropdown), "type_icon");
  if (type_icon) {
    relay_manager_update_type_icon(type_icon, type);
  }
}

static void relay_manager_bind_factory_cb(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  RelayManagerCtx *ctx = (RelayManagerCtx*)user_data;
  GtkWidget *row = gtk_list_item_get_child(list_item);
  GtkStringObject *obj = GTK_STRING_OBJECT(gtk_list_item_get_item(list_item));

  if (!row || !obj) return;

  RelayRowWidgets *widgets = g_object_get_data(G_OBJECT(row), "widgets");
  if (!widgets) return;

  const gchar *url = gtk_string_object_get_string(obj);
  if (!url) return;

  /* Update connection status indicator with per-relay state */
  if (widgets->connection_icon && ctx && ctx->main_window && ctx->main_window->pool) {
    GNostrRelay *relay = gnostr_pool_get_relay(ctx->main_window->pool, url);

    /* Clear all state CSS classes first */
    gtk_widget_remove_css_class(widgets->connection_icon, "dim-label");
    gtk_widget_remove_css_class(widgets->connection_icon, "success");
    gtk_widget_remove_css_class(widgets->connection_icon, "error");
    gtk_widget_remove_css_class(widgets->connection_icon, "warning");

    if (relay) {
      GNostrRelayState state = gnostr_relay_get_state(relay);
      switch (state) {
        case GNOSTR_RELAY_STATE_CONNECTED:
          gtk_image_set_from_icon_name(GTK_IMAGE(widgets->connection_icon), "network-wired-symbolic");
          gtk_widget_add_css_class(widgets->connection_icon, "success");
          gtk_widget_set_tooltip_text(widgets->connection_icon, "Connected");
          break;
        case GNOSTR_RELAY_STATE_CONNECTING:
          gtk_image_set_from_icon_name(GTK_IMAGE(widgets->connection_icon), "network-wired-symbolic");
          gtk_widget_add_css_class(widgets->connection_icon, "warning");
          gtk_widget_set_tooltip_text(widgets->connection_icon, "Connecting\u2026");
          break;
        case GNOSTR_RELAY_STATE_ERROR:
          gtk_image_set_from_icon_name(GTK_IMAGE(widgets->connection_icon), "network-error-symbolic");
          gtk_widget_add_css_class(widgets->connection_icon, "error");
          gtk_widget_set_tooltip_text(widgets->connection_icon, "Connection failed");
          break;
        case GNOSTR_RELAY_STATE_DISCONNECTED:
        default:
          gtk_image_set_from_icon_name(GTK_IMAGE(widgets->connection_icon), "network-offline-symbolic");
          gtk_widget_add_css_class(widgets->connection_icon, "dim-label");
          gtk_widget_set_tooltip_text(widgets->connection_icon, "Disconnected");
          break;
      }
    } else {
      /* Relay not in pool — not active this session */
      gtk_image_set_from_icon_name(GTK_IMAGE(widgets->connection_icon), "network-offline-symbolic");
      gtk_widget_add_css_class(widgets->connection_icon, "dim-label");
      gtk_widget_set_tooltip_text(widgets->connection_icon, "Not connected");
    }
  }

  /* Setup type dropdown for this relay */
  if (widgets->type_dropdown && ctx && ctx->relay_types) {
    /* Disconnect any previous signal handler */
    g_signal_handlers_disconnect_by_func(widgets->type_dropdown, G_CALLBACK(on_relay_type_changed), ctx);

    /* Store URL and icon reference in dropdown */
    g_object_set_data_full(G_OBJECT(widgets->type_dropdown), "relay_url", g_strdup(url), g_free);
    g_object_set_data(G_OBJECT(widgets->type_dropdown), "type_icon", widgets->type_icon);

    /* Get stored type, default to READWRITE */
    gpointer stored = g_hash_table_lookup(ctx->relay_types, url);
    GnostrRelayType type = stored ? GPOINTER_TO_INT(stored) : GNOSTR_RELAY_READWRITE;

    /* Set dropdown selection without triggering signal */
    gtk_drop_down_set_selected(GTK_DROP_DOWN(widgets->type_dropdown), relay_type_to_dropdown(type));

    /* Update type icon */
    if (widgets->type_icon) {
      relay_manager_update_type_icon(widgets->type_icon, type);
    }

    /* Connect change signal */
    g_signal_connect(widgets->type_dropdown, "notify::selected", G_CALLBACK(on_relay_type_changed), ctx);
  }

  /* Wire per-row delete button */
  if (widgets->delete_button) {
    g_signal_handlers_disconnect_by_func(widgets->delete_button, G_CALLBACK(on_relay_row_delete_clicked), ctx);
    g_object_set_data_full(G_OBJECT(widgets->delete_button), "relay_url", g_strdup(url), g_free);
    g_signal_connect(widgets->delete_button, "clicked", G_CALLBACK(on_relay_row_delete_clicked), ctx);
  }

  /* Try to get cached relay info */
  GnostrRelayInfo *info = gnostr_relay_info_cache_get(url);

  /* Set name (from cache or hostname fallback) */
  if (info && info->name && *info->name) {
    gtk_label_set_text(GTK_LABEL(widgets->name_label), info->name);
  } else {
    gchar *hostname = relay_manager_extract_hostname(url);
    gtk_label_set_text(GTK_LABEL(widgets->name_label), hostname ? hostname : url);
    g_free(hostname);
  }

  /* Set URL */
  gtk_label_set_text(GTK_LABEL(widgets->url_label), url);

  /* Update status icon based on cache availability */
  if (info && !info->fetch_failed) {
    gtk_image_set_from_icon_name(GTK_IMAGE(widgets->status_icon), "network-transmit-receive-symbolic");
    gtk_widget_remove_css_class(widgets->status_icon, "dim-label");
    gtk_widget_add_css_class(widgets->status_icon, "success");
    gtk_widget_set_tooltip_text(widgets->status_icon, "Relay info available");
  } else if (info && info->fetch_failed) {
    gtk_image_set_from_icon_name(GTK_IMAGE(widgets->status_icon), "network-error-symbolic");
    gtk_widget_remove_css_class(widgets->status_icon, "dim-label");
    gtk_widget_add_css_class(widgets->status_icon, "error");
    gtk_widget_set_tooltip_text(widgets->status_icon, "Failed to fetch relay info");
  } else {
    gtk_image_set_from_icon_name(GTK_IMAGE(widgets->status_icon), "network-offline-symbolic");
    gtk_widget_remove_css_class(widgets->status_icon, "success");
    gtk_widget_remove_css_class(widgets->status_icon, "error");
    gtk_widget_add_css_class(widgets->status_icon, "dim-label");
    gtk_widget_set_tooltip_text(widgets->status_icon, "Relay info not yet fetched");
  }

  /* Clear and populate NIP badges (show key NIPs only) */
  relay_manager_clear_container(widgets->nips_box);
  if (info && info->supported_nips && info->supported_nips_count > 0) {
    /* Show only key NIPs: 1, 11, 17, 42, 50, 59 */
    gint key_nips[] = {1, 11, 17, 42, 50, 59};
    gint shown = 0;
    for (gsize i = 0; i < info->supported_nips_count && shown < 4; i++) {
      gint nip = info->supported_nips[i];
      for (gsize j = 0; j < G_N_ELEMENTS(key_nips); j++) {
        if (nip == key_nips[j]) {
          relay_manager_add_small_nip_badge(widgets->nips_box, nip);
          shown++;
          break;
        }
      }
    }
    /* If we have more NIPs, show count */
    if (info->supported_nips_count > 4) {
      g_autofree gchar *more = g_strdup_printf("+%zu", info->supported_nips_count - shown);
      GtkWidget *more_label = gtk_label_new(more);
      gtk_widget_add_css_class(more_label, "dim-label");
      gtk_widget_add_css_class(more_label, "caption");
      gtk_box_append(GTK_BOX(widgets->nips_box), more_label);
    }
  }

  /* Show warning icon if auth/payment required */
  if (info && (info->auth_required || info->payment_required || info->restricted_writes)) {
    gtk_widget_set_visible(widgets->warning_icon, TRUE);
    GString *tooltip = g_string_new("Warning: ");
    if (info->auth_required) g_string_append(tooltip, "Auth required. ");
    if (info->payment_required) g_string_append(tooltip, "Payment required. ");
    if (info->restricted_writes) g_string_append(tooltip, "Restricted writes.");
    gtk_widget_set_tooltip_text(widgets->warning_icon, tooltip->str);
    g_string_free(tooltip, TRUE);
  } else {
    gtk_widget_set_visible(widgets->warning_icon, FALSE);
  }

  if (info) gnostr_relay_info_free(info);
}

void gnostr_main_window_on_relays_clicked_internal(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  GtkBuilder *builder = gtk_builder_new_from_resource("/org/gnostr/ui/ui/dialogs/gnostr-relay-manager.ui");
  if (!builder) {
    show_toast(self, "Relay manager UI missing");
    return;
  }

  GtkWindow *win = GTK_WINDOW(gtk_builder_get_object(builder, "relay_manager_window"));
  if (!win) {
    g_object_unref(builder);
    show_toast(self, "Relay manager window missing");
    return;
  }

  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  gtk_window_set_modal(win, TRUE);

  /* Create context */
  RelayManagerCtx *ctx = g_new0(RelayManagerCtx, 1);
  ctx->ref_count = 1;  /* Initial reference for the window */
  ctx->window = win;
  ctx->builder = builder;
  ctx->modified = FALSE;
  ctx->relay_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  ctx->main_window = self;  /* Store reference for pool access */

  /* Create relay list model */
  ctx->relay_model = gtk_string_list_new(NULL);

  /* Load saved relays with their NIP-65 types */
  GPtrArray *saved = gnostr_load_nip65_relays();
  for (guint i = 0; i < saved->len; i++) {
    GnostrNip65Relay *relay = g_ptr_array_index(saved, i);
    if (relay && relay->url && *relay->url) {
      gtk_string_list_append(ctx->relay_model, relay->url);
      g_hash_table_insert(ctx->relay_types, g_strdup(relay->url), GINT_TO_POINTER(relay->type));
    }
  }
  g_ptr_array_unref(saved);

  /* Setup selection model */
  ctx->selection = gtk_single_selection_new(G_LIST_MODEL(ctx->relay_model));
  gtk_single_selection_set_autoselect(ctx->selection, FALSE);
  gtk_single_selection_set_can_unselect(ctx->selection, TRUE);

  /* Setup list view with factory */
  GtkListView *list_view = GTK_LIST_VIEW(gtk_builder_get_object(builder, "relay_list"));
  ctx->list_view = list_view;
  if (list_view) {
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(relay_manager_setup_factory_cb), ctx);
    g_signal_connect(factory, "bind", G_CALLBACK(relay_manager_bind_factory_cb), ctx);
    gtk_list_view_set_factory(list_view, factory);
    gtk_list_view_set_model(list_view, GTK_SELECTION_MODEL(ctx->selection));
    g_object_unref(factory);
  }

  /* Connect pool relay-state-changed for live connection indicator updates */
  if (self->pool) {
    ctx->pool_state_handler_id = g_signal_connect(
        self->pool, "relay-state-changed",
        G_CALLBACK(on_pool_relay_state_changed_for_manager), ctx);
  }

  /* Connect selection change signal */
  g_signal_connect(ctx->selection, "selection-changed", G_CALLBACK(on_relay_selection_changed), ctx);

  /* Wire buttons */
  GtkWidget *btn_add = GTK_WIDGET(gtk_builder_get_object(builder, "btn_add"));
  GtkWidget *btn_remove = GTK_WIDGET(gtk_builder_get_object(builder, "btn_remove"));
  GtkWidget *btn_save = GTK_WIDGET(gtk_builder_get_object(builder, "btn_save"));
  GtkWidget *btn_cancel = GTK_WIDGET(gtk_builder_get_object(builder, "btn_cancel"));
  GtkWidget *btn_retry = GTK_WIDGET(gtk_builder_get_object(builder, "btn_retry"));
  GtkWidget *btn_discover = GTK_WIDGET(gtk_builder_get_object(builder, "btn_discover"));
  GtkWidget *relay_entry = GTK_WIDGET(gtk_builder_get_object(builder, "relay_entry"));

  if (btn_add) g_signal_connect(btn_add, "clicked", G_CALLBACK(relay_manager_on_add_clicked), ctx);
  if (btn_remove) g_signal_connect(btn_remove, "clicked", G_CALLBACK(relay_manager_on_remove_clicked), ctx);
  if (btn_save) g_signal_connect(btn_save, "clicked", G_CALLBACK(relay_manager_on_save_clicked), ctx);
  if (btn_cancel) g_signal_connect(btn_cancel, "clicked", G_CALLBACK(relay_manager_on_cancel_clicked), ctx);
  if (btn_retry) g_signal_connect(btn_retry, "clicked", G_CALLBACK(relay_manager_on_retry_clicked), ctx);
  if (btn_discover) g_signal_connect(btn_discover, "clicked", G_CALLBACK(relay_manager_on_discover_clicked), ctx);
  if (relay_entry) g_signal_connect(relay_entry, "activate", G_CALLBACK(relay_manager_on_entry_activate), ctx);

  /* Update status and cleanup on destroy */
  relay_manager_update_status(ctx);
  g_signal_connect(win, "destroy", G_CALLBACK(relay_manager_on_destroy), ctx);

  /* nostrc-50v: Select first relay and populate info pane on open */
  guint n_items = g_list_model_get_n_items(G_LIST_MODEL(ctx->relay_model));
  if (n_items > 0) {
    gtk_single_selection_set_selected(ctx->selection, 0);
  }

  gtk_window_present(win);
}
