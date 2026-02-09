#define G_LOG_DOMAIN "gnostr-session-view"

#include "gnostr-session-view.h"

#include "../gnostr-plugin-api.h"
#include "gnostr-classifieds-view.h"
#include "gnostr-dm-inbox-view.h"
#include "gnostr-notifications-view.h"
#include "gnostr-profile-pane.h"
#include "gnostr-profile-provider.h"
#include "gnostr-avatar-cache.h"
#include "gnostr-repo-browser.h"
#include "gnostr-search-results-view.h"
#include "gnostr-thread-view.h"
#include "gnostr-article-reader.h"
#include "gnostr-timeline-view.h"
#include "page-discover.h"

#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "nostr_nip19.h"
#include "../util/utils.h"

/* This must match the compiled resource path for the blueprint template */
#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-session-view.ui"

typedef enum {
  PROP_0,
  PROP_COMPACT,
  PROP_AUTHENTICATED,
  N_PROPS
} GnostrSessionViewProperty;

static GParamSpec *props[N_PROPS];

typedef enum {
  SIGNAL_PAGE_SELECTED,
  SIGNAL_SETTINGS_REQUESTED,
  SIGNAL_RELAYS_REQUESTED,
  SIGNAL_RECONNECT_REQUESTED,
  SIGNAL_LOGIN_REQUESTED,
  SIGNAL_LOGOUT_REQUESTED,
  SIGNAL_ACCOUNT_SWITCH_REQUESTED,
  SIGNAL_NEW_NOTES_CLICKED,
  SIGNAL_COMPOSE_REQUESTED,
  SIGNAL_SEARCH_CHANGED,
  SIGNAL_VIEW_PROFILE_REQUESTED,
  N_SIGNALS
} GnostrSessionViewSignal;

static guint signals[N_SIGNALS];

struct _GnostrSessionView {
  AdwBin parent_instance;

  /* Template root/container */
  GtkOverlay *session_overlay;
  AdwNavigationSplitView *split_view;

  /* Sidebar */
  AdwNavigationPage *sidebar_page;
  AdwHeaderBar *sidebar_header_bar;
  GtkButton *btn_settings;
  GtkMenuButton *btn_menu;
  GtkScrolledWindow *sidebar_scroller;
  GtkListBox *sidebar_list;

  GtkListBoxRow *row_timeline;
  GtkListBoxRow *row_notifications;
  GtkListBoxRow *row_messages;
  GtkListBoxRow *row_discover;
  GtkListBoxRow *row_search;
  GtkListBoxRow *row_classifieds;
  GtkListBoxRow *row_repos;

  /* Content */
  AdwNavigationPage *content_page;
  AdwToolbarView *toolbar_view;
  AdwHeaderBar *header_bar;
  GtkMenuButton *btn_relays;
  GtkImage *relay_status_icon;
  GtkLabel *relay_status_label;
  GtkPopover *relay_popover;
  GtkLabel *lbl_connected_count;
  GtkLabel *lbl_total_count;
  GtkButton *btn_manage_relays;
  GtkButton *btn_reconnect;
  GtkMenuButton *btn_avatar;
  GtkButton *btn_compose;
  GtkButton *btn_search;

  /* Search bar */
  GtkSearchBar *search_bar;
  GtkSearchEntry *search_entry;

  GtkPopover *avatar_popover;
  GtkLabel *lbl_signin_status;
  GtkLabel *lbl_profile_name;
  GtkButton *btn_view_profile;
  GtkButton *btn_login;
  GtkButton *btn_logout;
  GtkButton *btn_add_account;
  GtkListBox *account_list;
  GtkWidget *account_separator;
  char *current_npub;  /* Cache for popover rebuild */

  /* User avatar in popover */
  GtkWidget *popover_avatar_image;    /* GtkPicture for user avatar */
  GtkWidget *popover_avatar_initials; /* GtkLabel fallback with initials */
  char *current_pubkey_hex;           /* Cached pubkey for profile lookups */

  /* Header bar avatar button content */
  GtkWidget *header_avatar_image;     /* GtkPicture for header avatar */
  GtkWidget *header_avatar_initials;  /* GtkLabel fallback with initials */

  GtkOverlay *content_root;

  GtkRevealer *new_notes_revealer;
  GtkButton *btn_new_notes;
  GtkImage *img_new_notes_arrow;
  GtkSpinner *spinner_new_notes;
  GtkLabel *lbl_new_notes_count;
  guint pending_new_notes_count;  /* Track pending count for re-showing toast */
  gboolean new_notes_loading;     /* TRUE while loading new notes */

  AdwOverlaySplitView *panel_split;
  GtkBox *panel_container;
  GtkWidget *profile_pane;
  GtkWidget *thread_view;
  GtkWidget *article_reader;

  AdwViewStack *stack;
  GtkWidget *timeline;
  GtkWidget *notifications_view;
  GtkStack  *dm_stack;
  GtkWidget *dm_inbox;
  GtkWidget *dm_conversation;
  GtkWidget *discover_page;
  GtkWidget *search_results_view;
  GtkWidget *classifieds_view;
  GtkWidget *repo_browser;

  AdwViewSwitcherBar *bottom_bar;

  /* State */
  gboolean compact;
  gboolean authenticated;
  gboolean showing_profile;

  /* Plugin panels */
  GHashTable *plugin_panels;      /* panel_id -> GtkWidget* */
  GHashTable *plugin_rows;        /* panel_id -> GtkListBoxRow* */
  GHashTable *plugin_extensions;  /* panel_id -> GnostrUIExtension* */
  GHashTable *plugin_contexts;    /* panel_id -> GnostrPluginContext* */
  GHashTable *plugin_labels;      /* panel_id -> char* (display label) */
  GHashTable *plugin_auth_required; /* panel_id -> GINT_TO_POINTER(gboolean) */
  GtkWidget *plugin_separator;    /* Separator before plugin items */

  /* Optional toast forwarding (weak) */
  GWeakRef toast_overlay_ref;
};

G_DEFINE_FINAL_TYPE(GnostrSessionView, gnostr_session_view, ADW_TYPE_BIN)

static const char *page_name_for_row(GnostrSessionView *self, GtkListBoxRow *row) {
  if (!self || !row) return NULL;
  if (row == self->row_timeline) return "timeline";
  if (row == self->row_notifications) return "notifications";
  if (row == self->row_messages) return "messages";
  if (row == self->row_discover) return "discover";
  if (row == self->row_search) return "search";
  if (row == self->row_classifieds) return "classifieds";
  if (row == self->row_repos) return "repos";

  /* Check plugin rows */
  if (self->plugin_rows) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->plugin_rows);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      if (GTK_LIST_BOX_ROW(value) == row) {
        return (const char *)key;
      }
    }
  }

  return NULL;
}

static GtkListBoxRow *row_for_page_name(GnostrSessionView *self, const char *page_name) {
  if (!self || !page_name) return NULL;
  if (g_strcmp0(page_name, "timeline") == 0) return self->row_timeline;
  if (g_strcmp0(page_name, "notifications") == 0) return self->row_notifications;
  if (g_strcmp0(page_name, "messages") == 0) return self->row_messages;
  if (g_strcmp0(page_name, "discover") == 0) return self->row_discover;
  if (g_strcmp0(page_name, "search") == 0) return self->row_search;
  if (g_strcmp0(page_name, "classifieds") == 0) return self->row_classifieds;
  if (g_strcmp0(page_name, "repos") == 0) return self->row_repos;

  /* Check plugin rows */
  if (self->plugin_rows) {
    GtkListBoxRow *row = g_hash_table_lookup(self->plugin_rows, page_name);
    if (row) return row;
  }

  return NULL;
}

static const char *title_for_page_name(GnostrSessionView *self, const char *page_name) {
  if (g_strcmp0(page_name, "timeline") == 0) return _("Timeline");
  if (g_strcmp0(page_name, "notifications") == 0) return _("Notifications");
  if (g_strcmp0(page_name, "messages") == 0) return _("Messages");
  if (g_strcmp0(page_name, "discover") == 0) return _("Discover");
  if (g_strcmp0(page_name, "search") == 0) return _("Search");
  if (g_strcmp0(page_name, "classifieds") == 0) return _("Marketplace");
  if (g_strcmp0(page_name, "repos") == 0) return _("Git Repos");

  /* Check plugin labels */
  if (self && self->plugin_labels) {
    const char *label = g_hash_table_lookup(self->plugin_labels, page_name);
    if (label) return label;
  }

  return NULL;
}

/* Forward declarations for signal handlers used in ensure_avatar_popover */
static void on_btn_view_profile_clicked(GtkButton *btn, gpointer user_data);
static void on_btn_login_clicked(GtkButton *btn, gpointer user_data);
static void on_btn_logout_clicked(GtkButton *btn, gpointer user_data);
static void on_btn_add_account_clicked(GtkButton *btn, gpointer user_data);
static void on_account_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);

/* Helper: Truncate npub for display (e.g., "npub1abc...xyz") */
static char *truncate_npub(const char *npub) {
  if (!npub || strlen(npub) < 20) return g_strdup(npub ? npub : "");
  /* Show first 10 and last 4 characters: npub1abcd...wxyz */
  return g_strdup_printf("%.10s...%s", npub, npub + strlen(npub) - 4);
}

/* Helper: Convert npub to hex pubkey. Returns newly allocated string or NULL. */
static char *npub_to_pubkey_hex(const char *npub) {
  if (!npub || strncmp(npub, "npub1", 5) != 0) return NULL;
  g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(npub, NULL);
  if (!n19) return NULL;
  return g_strdup(gnostr_nip19_get_pubkey(n19));
}

/* Helper: Generate initials from display name or handle */
static void set_initials_label(GtkWidget *label, const char *display_name, const char *handle) {
  if (!GTK_IS_LABEL(label)) return;
  const char *src = (display_name && *display_name) ? display_name : (handle && *handle ? handle : "AN");
  char initials[3] = {0};
  int i = 0;
  for (const char *p = src; *p && i < 2; p++) {
    if (g_ascii_isalnum(*p)) initials[i++] = g_ascii_toupper(*p);
  }
  if (i == 0) { initials[0] = 'A'; initials[1] = 'N'; }
  gtk_label_set_text(GTK_LABEL(label), initials);
}

/* Helper: Create avatar overlay with image and initials fallback for small (24px) avatars */
static GtkWidget *create_small_avatar_overlay(GtkWidget **out_image, GtkWidget **out_initials) {
  GtkWidget *overlay = gtk_overlay_new();
  gtk_widget_set_size_request(overlay, 24, 24);

  /* Initials fallback (shown when no avatar image) */
  GtkWidget *initials = gtk_label_new("AN");
  gtk_widget_add_css_class(initials, "avatar-initials");
  gtk_widget_set_halign(initials, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(initials, GTK_ALIGN_CENTER);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), initials);

  /* Avatar image (overlays initials when loaded) */
  GtkWidget *image = gtk_picture_new();
  gtk_widget_set_size_request(image, 24, 24);
  gtk_picture_set_content_fit(GTK_PICTURE(image), GTK_CONTENT_FIT_COVER);
  gtk_widget_add_css_class(image, "avatar");
  gtk_widget_set_visible(image, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), image);

  if (out_image) *out_image = image;
  if (out_initials) *out_initials = initials;

  return overlay;
}

/* Helper: Create a row for an account in the account list */
static GtkWidget *create_account_row(const char *npub, gboolean is_current) {
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);
  gtk_widget_set_margin_start(box, 4);
  gtk_widget_set_margin_end(box, 4);

  /* Create avatar overlay with image and initials */
  GtkWidget *avatar_image = NULL;
  GtkWidget *avatar_initials = NULL;
  GtkWidget *avatar_overlay = create_small_avatar_overlay(&avatar_image, &avatar_initials);
  gtk_box_append(GTK_BOX(box), avatar_overlay);

  /* Try to load profile and avatar for this account */
  char *pubkey_hex = npub_to_pubkey_hex(npub);
  const char *display_name = NULL;
  if (pubkey_hex) {
    GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey_hex);
    if (meta) {
      display_name = meta->display_name ? meta->display_name : meta->name;
      set_initials_label(avatar_initials, display_name, NULL);

      /* Load avatar image if available */
      if (meta->picture && *meta->picture) {
        GdkTexture *cached = gnostr_avatar_try_load_cached(meta->picture);
        if (cached) {
          gtk_picture_set_paintable(GTK_PICTURE(avatar_image), GDK_PAINTABLE(cached));
          gtk_widget_set_visible(avatar_image, TRUE);
          gtk_widget_set_visible(avatar_initials, FALSE);
          g_object_unref(cached);
        } else {
          /* Download avatar asynchronously */
          gnostr_avatar_download_async(meta->picture, avatar_image, avatar_initials);
        }
      }
      gnostr_profile_meta_free(meta);
    } else {
      set_initials_label(avatar_initials, NULL, npub);
    }
    g_free(pubkey_hex);
  } else {
    set_initials_label(avatar_initials, NULL, npub);
  }

  /* Display name or truncated npub */
  GtkWidget *label;
  if (display_name && *display_name) {
    label = gtk_label_new(display_name);
  } else {
    char *display = truncate_npub(npub);
    label = gtk_label_new(display);
    g_free(display);
  }
  gtk_widget_set_hexpand(label, TRUE);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  if (is_current) {
    gtk_widget_add_css_class(label, "heading");
  }
  gtk_box_append(GTK_BOX(box), label);

  /* Checkmark for current account */
  if (is_current) {
    GtkWidget *check = gtk_image_new_from_icon_name("object-select-symbolic");
    gtk_box_append(GTK_BOX(box), check);
  }

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

  /* Store npub as row data for switch handler */
  g_object_set_data_full(G_OBJECT(row), "npub", g_strdup(npub), g_free);

  /* Disable activation for current account */
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), !is_current);

  return row;
}

/* Rebuild the account list in the popover */
static void rebuild_account_list(GnostrSessionView *self) {
  if (!self || !self->account_list) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->account_list))) != NULL) {
    gtk_list_box_remove(self->account_list, child);
  }

  /* Get known accounts from GSettings */
  GSettings *settings = g_settings_new("org.gnostr.Client");
  if (!settings) return;

  char *current_npub = g_settings_get_string(settings, "current-npub");
  char **accounts = g_settings_get_strv(settings, "known-accounts");
  g_object_unref(settings);

  /* Add rows for each account */
  gboolean has_accounts = FALSE;
  if (accounts) {
    for (int i = 0; accounts[i]; i++) {
      if (!accounts[i][0]) continue;  /* Skip empty strings */
      gboolean is_current = (current_npub && g_strcmp0(accounts[i], current_npub) == 0);
      GtkWidget *row = create_account_row(accounts[i], is_current);
      gtk_list_box_append(self->account_list, row);
      has_accounts = TRUE;
    }
    g_strfreev(accounts);
  }

  /* Show/hide account list and separator based on whether we have accounts */
  gtk_widget_set_visible(GTK_WIDGET(self->account_list), has_accounts);
  if (self->account_separator) {
    gtk_widget_set_visible(self->account_separator, has_accounts);
  }

  g_free(current_npub);
}

/* Helper: Create large avatar overlay for popover (48px) */
static GtkWidget *create_large_avatar_overlay(GtkWidget **out_image, GtkWidget **out_initials) {
  GtkWidget *overlay = gtk_overlay_new();
  gtk_widget_set_size_request(overlay, 48, 48);
  gtk_widget_set_halign(overlay, GTK_ALIGN_CENTER);

  /* Initials fallback (shown when no avatar image) */
  GtkWidget *initials = gtk_label_new("?");
  gtk_widget_add_css_class(initials, "avatar-initials");
  gtk_widget_add_css_class(initials, "title-2");
  gtk_widget_set_halign(initials, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(initials, GTK_ALIGN_CENTER);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), initials);

  /* Avatar image (overlays initials when loaded) */
  GtkWidget *image = gtk_picture_new();
  gtk_widget_set_size_request(image, 48, 48);
  gtk_picture_set_content_fit(GTK_PICTURE(image), GTK_CONTENT_FIT_COVER);
  gtk_widget_add_css_class(image, "avatar");
  gtk_widget_add_css_class(image, "avatar-large");
  gtk_widget_set_visible(image, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), image);

  if (out_image) *out_image = image;
  if (out_initials) *out_initials = initials;

  return overlay;
}

/* Create avatar popover lazily to avoid GTK4 crash on Linux */
static void ensure_avatar_popover(GnostrSessionView *self) {
  if (!self || !self->btn_avatar || self->avatar_popover) return;

  self->avatar_popover = GTK_POPOVER(gtk_popover_new());

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_size_request(box, 220, -1);

  /* User avatar section (shown when signed in) */
  GtkWidget *avatar_overlay = create_large_avatar_overlay(
      &self->popover_avatar_image, &self->popover_avatar_initials);
  gtk_widget_set_margin_bottom(avatar_overlay, 4);
  gtk_box_append(GTK_BOX(box), avatar_overlay);

  /* Status label (shown when not signed in) */
  self->lbl_signin_status = GTK_LABEL(gtk_label_new(_("Not signed in")));
  gtk_widget_add_css_class(GTK_WIDGET(self->lbl_signin_status), "dim-label");
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->lbl_signin_status));

  /* Profile name (shown when signed in) */
  self->lbl_profile_name = GTK_LABEL(gtk_label_new(""));
  gtk_widget_add_css_class(GTK_WIDGET(self->lbl_profile_name), "title-3");
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_profile_name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_visible(GTK_WIDGET(self->lbl_profile_name), FALSE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->lbl_profile_name));

  /* View Profile button (shown when signed in) — nostrc-bkor */
  self->btn_view_profile = GTK_BUTTON(gtk_button_new_with_label(_("View Profile")));
  gtk_widget_set_visible(GTK_WIDGET(self->btn_view_profile), FALSE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->btn_view_profile));

  /* Separator after profile section */
  GtkWidget *profile_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top(profile_separator, 8);
  gtk_widget_set_margin_bottom(profile_separator, 4);
  gtk_box_append(GTK_BOX(box), profile_separator);

  /* Account list for switching (hidden when empty) */
  GtkWidget *accounts_label = gtk_label_new(_("Accounts"));
  gtk_widget_add_css_class(accounts_label, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(accounts_label), 0);
  gtk_widget_set_margin_top(accounts_label, 4);
  gtk_widget_set_visible(accounts_label, FALSE);
  gtk_box_append(GTK_BOX(box), accounts_label);

  self->account_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_widget_add_css_class(GTK_WIDGET(self->account_list), "boxed-list");
  gtk_widget_set_visible(GTK_WIDGET(self->account_list), FALSE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->account_list));

  /* Separator before buttons */
  self->account_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top(self->account_separator, 8);
  gtk_widget_set_margin_bottom(self->account_separator, 4);
  gtk_widget_set_visible(self->account_separator, FALSE);
  gtk_box_append(GTK_BOX(box), self->account_separator);

  /* Sign In button (shown when not signed in) */
  self->btn_login = GTK_BUTTON(gtk_button_new_with_label(_("Sign In")));
  gtk_widget_add_css_class(GTK_WIDGET(self->btn_login), "suggested-action");
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->btn_login));

  /* Add Account button (always shown) */
  self->btn_add_account = GTK_BUTTON(gtk_button_new_with_label(_("Add Account")));
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->btn_add_account));

  /* Sign Out button (shown when signed in) */
  self->btn_logout = GTK_BUTTON(gtk_button_new_with_label(_("Sign Out")));
  gtk_widget_add_css_class(GTK_WIDGET(self->btn_logout), "destructive-action");
  gtk_widget_set_visible(GTK_WIDGET(self->btn_logout), FALSE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->btn_logout));

  gtk_popover_set_child(self->avatar_popover, box);
  gtk_menu_button_set_popover(self->btn_avatar, GTK_WIDGET(self->avatar_popover));

  /* Connect signals */
  g_signal_connect(self->btn_view_profile, "clicked", G_CALLBACK(on_btn_view_profile_clicked), self);
  g_signal_connect(self->btn_login, "clicked", G_CALLBACK(on_btn_login_clicked), self);
  g_signal_connect(self->btn_logout, "clicked", G_CALLBACK(on_btn_logout_clicked), self);
  g_signal_connect(self->btn_add_account, "clicked", G_CALLBACK(on_btn_add_account_clicked), self);
  g_signal_connect(self->account_list, "row-activated", G_CALLBACK(on_account_row_activated), self);

  /* Build initial account list */
  rebuild_account_list(self);
}

static void update_auth_gating(GnostrSessionView *self) {
  if (!self) return;

  /* NOTE: Do NOT call ensure_avatar_popover here - it will be created lazily
   * when the user clicks the avatar button. Creating it during init causes
   * GTK4 crash on Linux. */

  if (self->row_notifications)
    gtk_widget_set_sensitive(GTK_WIDGET(self->row_notifications), self->authenticated);
  if (self->row_messages)
    gtk_widget_set_sensitive(GTK_WIDGET(self->row_messages), self->authenticated);

  /* Update plugin sidebar rows that require authentication */
  if (self->plugin_auth_required && self->plugin_rows) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->plugin_auth_required);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      if (GPOINTER_TO_INT(value)) {
        GtkWidget *row = g_hash_table_lookup(self->plugin_rows, key);
        if (row) {
          gtk_widget_set_sensitive(row, self->authenticated);
        }
      }
    }
  }

  /* Update signin status label in avatar popover if it exists */
  if (self->lbl_signin_status) {
    gtk_label_set_text(self->lbl_signin_status,
                       self->authenticated ? _("Signed in") : _("Not signed in"));
  }

  /* Update login/logout button visibility if popover exists */
  if (self->btn_login) {
    gtk_widget_set_visible(GTK_WIDGET(self->btn_login), !self->authenticated);
  }
  if (self->btn_logout) {
    gtk_widget_set_visible(GTK_WIDGET(self->btn_logout), self->authenticated);
  }
  if (self->btn_view_profile) {
    gtk_widget_set_visible(GTK_WIDGET(self->btn_view_profile), self->authenticated);
  }

  /* If we became unauthenticated while on a gated page, go back to timeline */
  if (!self->authenticated && self->stack) {
    const char *visible = adw_view_stack_get_visible_child_name(self->stack);
    gboolean is_gated_page = (g_strcmp0(visible, "notifications") == 0 ||
                              g_strcmp0(visible, "messages") == 0);

    /* Also check if current page is a plugin page that requires auth */
    if (!is_gated_page && self->plugin_auth_required) {
      gpointer auth_value = g_hash_table_lookup(self->plugin_auth_required, visible);
      if (GPOINTER_TO_INT(auth_value)) {
        is_gated_page = TRUE;
      }
    }

    if (is_gated_page) {
      gnostr_session_view_show_page(self, "timeline");
    }
  }
}

static gboolean on_key_pressed(GtkEventControllerKey *controller,
                               guint keyval,
                               guint keycode,
                               GdkModifierType state,
                               gpointer user_data) {
  (void)controller;
  (void)keycode;

  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return GDK_EVENT_PROPAGATE;

  if (keyval == GDK_KEY_Escape) {
    /* Close search bar first if open */
    if (self->search_bar && gtk_search_bar_get_search_mode(self->search_bar)) {
      gtk_search_bar_set_search_mode(self->search_bar, FALSE);
      return GDK_EVENT_STOP;
    }
    if (gnostr_session_view_is_side_panel_visible(self)) {
      gnostr_session_view_hide_side_panel(self);
      return GDK_EVENT_STOP;
    }
  }

  /* Ctrl+F to navigate to search tab (nostrc-29) */
  if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_f) {
    if (self->stack) {
      adw_view_stack_set_visible_child_name(self->stack, "search");
      if (self->row_search && self->sidebar_list) {
        gtk_list_box_select_row(self->sidebar_list, self->row_search);
      }
      return GDK_EVENT_STOP;
    }
  }

  return GDK_EVENT_PROPAGATE;
}

static void on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;

  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self) || !row) return;

  const char *page_name = page_name_for_row(self, row);
  if (!page_name) return;

  /* Check if this is a built-in auth-gated page */
  gboolean requires_auth = (g_strcmp0(page_name, "notifications") == 0 ||
                            g_strcmp0(page_name, "messages") == 0);

  /* Check if this is a plugin page that requires auth */
  if (!requires_auth && self->plugin_auth_required) {
    gpointer value = g_hash_table_lookup(self->plugin_auth_required, page_name);
    if (GPOINTER_TO_INT(value)) {
      requires_auth = TRUE;
    }
  }

  if (!self->authenticated && requires_auth) {
    g_signal_emit(self, signals[SIGNAL_LOGIN_REQUESTED], 0);
    gnostr_session_view_show_toast(self, _("Sign in to view this page."));
    gnostr_session_view_show_page(self, "timeline");
    return;
  }

  gnostr_session_view_show_page(self, page_name);
  g_signal_emit(self, signals[SIGNAL_PAGE_SELECTED], 0, page_name);
}

static void on_btn_settings_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  g_signal_emit(self, signals[SIGNAL_SETTINGS_REQUESTED], 0);
}

static void on_btn_manage_relays_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  /* Close the popover before opening relay manager */
  if (self->relay_popover)
    gtk_popover_popdown(self->relay_popover);
  g_signal_emit(self, signals[SIGNAL_RELAYS_REQUESTED], 0);
}

static void on_btn_reconnect_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  g_signal_emit(self, signals[SIGNAL_RECONNECT_REQUESTED], 0);
}

static void on_btn_view_profile_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;

  /* Close popover before navigating */
  if (self->avatar_popover) {
    gtk_popover_popdown(self->avatar_popover);
  }

  g_signal_emit(self, signals[SIGNAL_VIEW_PROFILE_REQUESTED], 0);
}

static void on_btn_login_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  g_signal_emit(self, signals[SIGNAL_LOGIN_REQUESTED], 0);
}

static void on_btn_logout_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  g_signal_emit(self, signals[SIGNAL_LOGOUT_REQUESTED], 0);
}

static void on_btn_add_account_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;

  /* Close popover before opening login dialog */
  if (self->avatar_popover) {
    gtk_popover_popdown(self->avatar_popover);
  }

  /* Emit login-requested - main window will open login dialog */
  g_signal_emit(self, signals[SIGNAL_LOGIN_REQUESTED], 0);
}

static void on_account_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self) || !row) return;

  /* Get the npub stored in the row */
  const char *npub = g_object_get_data(G_OBJECT(row), "npub");
  if (!npub || !*npub) return;

  /* Close popover */
  if (self->avatar_popover) {
    gtk_popover_popdown(self->avatar_popover);
  }

  /* Emit account-switch-requested with the npub */
  g_signal_emit(self, signals[SIGNAL_ACCOUNT_SWITCH_REQUESTED], 0, npub);
}

static void on_btn_new_notes_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;

  /* Show spinner, hide arrow while loading new notes (nostrc-3r8k) */
  self->new_notes_loading = TRUE;
  if (self->img_new_notes_arrow)
    gtk_widget_set_visible(GTK_WIDGET(self->img_new_notes_arrow), FALSE);
  if (self->spinner_new_notes) {
    gtk_spinner_set_spinning(self->spinner_new_notes, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->spinner_new_notes), TRUE);
  }
  if (self->lbl_new_notes_count)
    gtk_label_set_text(self->lbl_new_notes_count, _("Loading\u2026"));

  g_signal_emit(self, signals[SIGNAL_NEW_NOTES_CLICKED], 0);
}

static void on_btn_compose_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;
  g_signal_emit(self, signals[SIGNAL_COMPOSE_REQUESTED], 0);
}

static void on_btn_search_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;

  /* Navigate to search tab (nostrc-29) */
  if (self->stack) {
    adw_view_stack_set_visible_child_name(self->stack, "search");
    GtkListBoxRow *row = self->row_search;
    if (row && self->sidebar_list) {
      gtk_list_box_select_row(self->sidebar_list, row);
    }
  }
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(user_data);
  if (!GNOSTR_IS_SESSION_VIEW(self)) return;

  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  g_signal_emit(self, signals[SIGNAL_SEARCH_CHANGED], 0, text);
}

static void gnostr_session_view_dispose(GObject *object) {
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(object);

  g_weak_ref_clear(&self->toast_overlay_ref);

  /* Clean up cached pubkey */
  g_clear_pointer(&self->current_pubkey_hex, g_free);

  /* Clean up plugin hash tables */
  g_clear_pointer(&self->plugin_panels, g_hash_table_unref);
  g_clear_pointer(&self->plugin_rows, g_hash_table_unref);
  g_clear_pointer(&self->plugin_extensions, g_hash_table_unref);
  g_clear_pointer(&self->plugin_contexts, g_hash_table_unref);
  g_clear_pointer(&self->plugin_labels, g_hash_table_unref);
  g_clear_pointer(&self->plugin_auth_required, g_hash_table_unref);

  G_OBJECT_CLASS(gnostr_session_view_parent_class)->dispose(object);
}

static void gnostr_session_view_get_property(GObject *object,
                                             guint prop_id,
                                             GValue *value,
                                             GParamSpec *pspec) {
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(object);

  switch ((GnostrSessionViewProperty)prop_id) {
    case PROP_COMPACT:
      g_value_set_boolean(value, self->compact);
      break;
    case PROP_AUTHENTICATED:
      g_value_set_boolean(value, self->authenticated);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gnostr_session_view_set_property(GObject *object,
                                             guint prop_id,
                                             const GValue *value,
                                             GParamSpec *pspec) {
  GnostrSessionView *self = GNOSTR_SESSION_VIEW(object);

  switch ((GnostrSessionViewProperty)prop_id) {
    case PROP_COMPACT:
      gnostr_session_view_set_compact(self, g_value_get_boolean(value));
      break;
    case PROP_AUTHENTICATED:
      gnostr_session_view_set_authenticated(self, g_value_get_boolean(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gnostr_session_view_class_init(GnostrSessionViewClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_session_view_dispose;
  object_class->get_property = gnostr_session_view_get_property;
  object_class->set_property = gnostr_session_view_set_property;

  props[PROP_COMPACT] = g_param_spec_boolean(
      "compact",
      "Compact",
      "Whether the session view is in compact mode (responsive layout)",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  props[PROP_AUTHENTICATED] = g_param_spec_boolean(
      "authenticated",
      "Authenticated",
      "Whether the user is authenticated (enables Notifications and Messages)",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, props);

  signals[SIGNAL_PAGE_SELECTED] = g_signal_new(
      "page-selected",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__STRING,
      G_TYPE_NONE,
      1,
      G_TYPE_STRING);

  signals[SIGNAL_SETTINGS_REQUESTED] = g_signal_new(
      "settings-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_RELAYS_REQUESTED] = g_signal_new(
      "relays-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_RECONNECT_REQUESTED] = g_signal_new(
      "reconnect-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_LOGIN_REQUESTED] = g_signal_new(
      "login-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_LOGOUT_REQUESTED] = g_signal_new(
      "logout-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_ACCOUNT_SWITCH_REQUESTED] = g_signal_new(
      "account-switch-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__STRING,
      G_TYPE_NONE,
      1,
      G_TYPE_STRING);

  signals[SIGNAL_NEW_NOTES_CLICKED] = g_signal_new(
      "new-notes-clicked",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_COMPOSE_REQUESTED] = g_signal_new(
      "compose-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE,
      0);

  signals[SIGNAL_SEARCH_CHANGED] = g_signal_new(
      "search-changed",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__STRING,
      G_TYPE_NONE,
      1,
      G_TYPE_STRING);

  signals[SIGNAL_VIEW_PROFILE_REQUESTED] = g_signal_new(
      "view-profile-requested",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      NULL,
      G_TYPE_NONE,
      0);

  /* Ensure custom widget types used in the template are registered */
  g_type_ensure(GNOSTR_TYPE_TIMELINE_VIEW);
  g_type_ensure(GNOSTR_TYPE_NOTIFICATIONS_VIEW);
  g_type_ensure(GNOSTR_TYPE_DM_INBOX_VIEW);
  g_type_ensure(GNOSTR_TYPE_PAGE_DISCOVER);
  g_type_ensure(GNOSTR_TYPE_CLASSIFIEDS_VIEW);
  g_type_ensure(GNOSTR_TYPE_REPO_BROWSER);
  g_type_ensure(GNOSTR_TYPE_PROFILE_PANE);
  g_type_ensure(GNOSTR_TYPE_THREAD_VIEW);
  g_type_ensure(GNOSTR_TYPE_ARTICLE_READER);

  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, session_overlay);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, split_view);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, sidebar_page);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, sidebar_header_bar);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_settings);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_menu);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, sidebar_scroller);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, sidebar_list);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_timeline);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_notifications);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_messages);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_discover);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_search);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_classifieds);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, row_repos);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, content_page);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, toolbar_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, header_bar);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_relays);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, relay_status_icon);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, relay_status_label);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, relay_popover);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, lbl_connected_count);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, lbl_total_count);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_manage_relays);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_reconnect);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_avatar);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_compose);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_search);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, search_bar);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, search_entry);

  /* avatar_popover and its children are now created programmatically to avoid
   * GTK4 crash on Linux where GtkPopover in template causes gtk_widget_root assertion */

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, content_root);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, new_notes_revealer);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, btn_new_notes);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, img_new_notes_arrow);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, spinner_new_notes);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, lbl_new_notes_count);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, panel_split);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, panel_container);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, profile_pane);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, thread_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, article_reader);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, timeline);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, notifications_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, dm_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, dm_inbox);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, dm_conversation);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, discover_page);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, search_results_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, classifieds_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, repo_browser);

  gtk_widget_class_bind_template_child(widget_class, GnostrSessionView, bottom_bar);
}

static void gnostr_session_view_init(GnostrSessionView *self) {
  g_weak_ref_init(&self->toast_overlay_ref, NULL);

  /* Defaults */
  self->compact = FALSE;
  self->authenticated = FALSE;
  self->showing_profile = TRUE;

  /* Initialize plugin hash tables */
  self->plugin_panels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->plugin_rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->plugin_extensions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->plugin_contexts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->plugin_labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->plugin_auth_required = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->plugin_separator = NULL;

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Replace header avatar button icon with avatar overlay */
  if (self->btn_avatar) {
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_size_request(overlay, 24, 24);

    /* Initials fallback (default to "?" for not signed in) */
    self->header_avatar_initials = gtk_label_new("?");
    gtk_widget_add_css_class(self->header_avatar_initials, "avatar-initials");
    gtk_widget_set_halign(self->header_avatar_initials, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->header_avatar_initials, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), self->header_avatar_initials);

    /* Avatar image (overlays initials when loaded) */
    self->header_avatar_image = gtk_picture_new();
    gtk_widget_set_size_request(self->header_avatar_image, 24, 24);
    gtk_picture_set_content_fit(GTK_PICTURE(self->header_avatar_image), GTK_CONTENT_FIT_COVER);
    gtk_widget_add_css_class(self->header_avatar_image, "avatar");
    gtk_widget_set_visible(self->header_avatar_image, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), self->header_avatar_image);

    gtk_menu_button_set_child(self->btn_avatar, overlay);
  }

  /* Avatar popover created after template init to avoid GTK4 crash on Linux
   * where GtkPopover creation during template init causes gtk_widget_root
   * assertion failure. Safe to call here since template init is complete. */
  self->avatar_popover = NULL;
  self->lbl_signin_status = NULL;
  self->lbl_profile_name = NULL;
  self->btn_login = NULL;
  self->btn_logout = NULL;
  self->btn_view_profile = NULL;
  ensure_avatar_popover(self);

  /* ESC closes profile/thread side panel when visible */
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), keys);

  /* Wire up interactions */
  if (self->sidebar_list) {
    g_signal_connect(self->sidebar_list, "row-activated", G_CALLBACK(on_sidebar_row_activated), self);
  }

  if (self->btn_settings) {
    g_signal_connect(self->btn_settings, "clicked", G_CALLBACK(on_btn_settings_clicked), self);
  }

  if (self->btn_manage_relays) {
    g_signal_connect(self->btn_manage_relays, "clicked", G_CALLBACK(on_btn_manage_relays_clicked), self);
  }

  if (self->btn_reconnect) {
    g_signal_connect(self->btn_reconnect, "clicked", G_CALLBACK(on_btn_reconnect_clicked), self);
  }

  if (self->btn_login) {
    g_signal_connect(self->btn_login, "clicked", G_CALLBACK(on_btn_login_clicked), self);
  }

  if (self->btn_logout) {
    g_signal_connect(self->btn_logout, "clicked", G_CALLBACK(on_btn_logout_clicked), self);
  }

  if (self->btn_new_notes) {
    g_signal_connect(self->btn_new_notes, "clicked", G_CALLBACK(on_btn_new_notes_clicked), self);
  }

  if (self->btn_compose) {
    g_signal_connect(self->btn_compose, "clicked", G_CALLBACK(on_btn_compose_clicked), self);
  }

  if (self->btn_search) {
    g_signal_connect(self->btn_search, "clicked", G_CALLBACK(on_btn_search_clicked), self);
  }

  /* Connect search entry signal (search bar and entry come from template) */
  if (self->search_bar && self->search_entry) {
    gtk_search_bar_connect_entry(self->search_bar, GTK_EDITABLE(self->search_entry));
    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);
  }

  /* Start on Timeline by default */
  if (self->sidebar_list && self->row_timeline) {
    gtk_list_box_select_row(self->sidebar_list, self->row_timeline);
  }
  if (self->stack) {
    adw_view_stack_set_visible_child_name(self->stack, "timeline");
  }

  update_auth_gating(self);
}

/* ---- Public API ---- */

GnostrSessionView *gnostr_session_view_new(void) {
  return g_object_new(GNOSTR_TYPE_SESSION_VIEW, NULL);
}

gboolean gnostr_session_view_get_compact(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), FALSE);
  return self->compact;
}

void gnostr_session_view_set_compact(GnostrSessionView *self, gboolean compact) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));

  compact = !!compact;
  if (self->compact == compact) return;

  self->compact = compact;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_COMPACT]);
}

gboolean gnostr_session_view_get_authenticated(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), FALSE);
  return self->authenticated;
}

void gnostr_session_view_set_authenticated(GnostrSessionView *self, gboolean authenticated) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));

  authenticated = !!authenticated;
  if (self->authenticated == authenticated) return;

  self->authenticated = authenticated;
  update_auth_gating(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_AUTHENTICATED]);
}

void gnostr_session_view_show_page(GnostrSessionView *self, const char *page_name) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  g_return_if_fail(page_name != NULL);

  if (!self->stack) return;

  /* Check if this is a plugin page that requires auth */
  gboolean requires_auth = (g_strcmp0(page_name, "notifications") == 0 ||
                            g_strcmp0(page_name, "messages") == 0);
  if (!requires_auth && self->plugin_auth_required) {
    gpointer value = g_hash_table_lookup(self->plugin_auth_required, page_name);
    if (GPOINTER_TO_INT(value)) {
      requires_auth = TRUE;
    }
  }

  /* If caller requests a gated page in guest mode, bounce to timeline */
  if (!self->authenticated && requires_auth) {
    g_signal_emit(self, signals[SIGNAL_LOGIN_REQUESTED], 0);
    gnostr_session_view_show_toast(self, _("Sign in to view this page."));
    page_name = "timeline";
  }

  /* Hide new notes toast when switching away from timeline,
   * show it when switching back if there are pending notes */
  if (self->new_notes_revealer) {
    if (g_strcmp0(page_name, "timeline") != 0) {
      gtk_revealer_set_reveal_child(self->new_notes_revealer, FALSE);
    } else if (self->pending_new_notes_count > 0) {
      gtk_revealer_set_reveal_child(self->new_notes_revealer, TRUE);
    }
  }

  /* Check if this is a plugin page and lazily create its panel if needed */
  if (self->plugin_extensions && g_hash_table_contains(self->plugin_extensions, page_name)) {
    GtkWidget *panel = g_hash_table_lookup(self->plugin_panels, page_name);
    if (!panel) {
      /* Lazily create the panel widget from the extension */
      GnostrUIExtension *extension = g_hash_table_lookup(self->plugin_extensions, page_name);
      GnostrPluginContext *context = g_hash_table_lookup(self->plugin_contexts, page_name);
      if (extension && GNOSTR_IS_UI_EXTENSION(extension)) {
        panel = gnostr_ui_extension_create_panel_widget(extension, context, page_name);
        if (panel) {
          /* Add to the stack */
          const char *label = g_hash_table_lookup(self->plugin_labels, page_name);
          adw_view_stack_add_titled(self->stack, panel, page_name, label ? label : page_name);
          g_hash_table_insert(self->plugin_panels, g_strdup(page_name), panel);
        }
      }
    }
  }

  adw_view_stack_set_visible_child_name(self->stack, page_name);

  if (self->content_page) {
    const char *title = title_for_page_name(self, page_name);
    if (title) adw_navigation_page_set_title(self->content_page, title);
  }

  if (self->sidebar_list) {
    GtkListBoxRow *row = row_for_page_name(self, page_name);
    if (row) gtk_list_box_select_row(self->sidebar_list, row);
  }
}

void gnostr_session_view_show_profile_panel(GnostrSessionView *self) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  if (!self->panel_split) return;

  if (self->thread_view) gtk_widget_set_visible(self->thread_view, FALSE);
  if (self->article_reader) gtk_widget_set_visible(self->article_reader, FALSE);
  if (self->profile_pane) gtk_widget_set_visible(self->profile_pane, TRUE);

  self->showing_profile = TRUE;
  adw_overlay_split_view_set_show_sidebar(self->panel_split, TRUE);
}

void gnostr_session_view_show_thread_panel(GnostrSessionView *self) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  if (!self->panel_split) return;

  if (self->profile_pane) gtk_widget_set_visible(self->profile_pane, FALSE);
  if (self->article_reader) gtk_widget_set_visible(self->article_reader, FALSE);
  if (self->thread_view) gtk_widget_set_visible(self->thread_view, TRUE);

  self->showing_profile = FALSE;
  adw_overlay_split_view_set_show_sidebar(self->panel_split, TRUE);
}

void gnostr_session_view_show_article_panel(GnostrSessionView *self) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  if (!self->panel_split) return;

  if (self->profile_pane) gtk_widget_set_visible(self->profile_pane, FALSE);
  if (self->thread_view) gtk_widget_set_visible(self->thread_view, FALSE);
  if (self->article_reader) gtk_widget_set_visible(self->article_reader, TRUE);

  self->showing_profile = FALSE;
  adw_overlay_split_view_set_show_sidebar(self->panel_split, TRUE);
}

void gnostr_session_view_hide_side_panel(GnostrSessionView *self) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  if (!self->panel_split) return;

  adw_overlay_split_view_set_show_sidebar(self->panel_split, FALSE);
}

gboolean gnostr_session_view_is_side_panel_visible(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), FALSE);
  if (!self->panel_split) return FALSE;

  return adw_overlay_split_view_get_show_sidebar(self->panel_split);
}

void gnostr_session_view_set_toast_overlay(GnostrSessionView *self, AdwToastOverlay *overlay) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  g_weak_ref_set(&self->toast_overlay_ref, overlay);
}

void gnostr_session_view_show_toast(GnostrSessionView *self, const char *message) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  if (!message || *message == '\0') return;

  AdwToastOverlay *overlay = g_weak_ref_get(&self->toast_overlay_ref);
  if (!overlay) return;

  AdwToast *toast = adw_toast_new(message);
  adw_toast_set_timeout(toast, 2);
  adw_toast_overlay_add_toast(overlay, toast);

  g_object_unref(overlay);
}

GtkWidget *gnostr_session_view_get_timeline(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->timeline;
}

GtkWidget *gnostr_session_view_get_notifications_view(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->notifications_view;
}

GtkWidget *gnostr_session_view_get_dm_inbox(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->dm_inbox;
}

GtkStack *gnostr_session_view_get_dm_stack(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->dm_stack;
}

GtkWidget *gnostr_session_view_get_dm_conversation(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->dm_conversation;
}

GtkWidget *gnostr_session_view_get_discover_page(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->discover_page;
}

GtkWidget *gnostr_session_view_get_search_results_view(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->search_results_view;
}

GtkWidget *gnostr_session_view_get_classifieds_view(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->classifieds_view;
}

GtkWidget *gnostr_session_view_get_repo_browser(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->repo_browser;
}

GtkWidget *gnostr_session_view_get_profile_pane(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->profile_pane;
}

GtkWidget *gnostr_session_view_get_thread_view(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->thread_view;
}

GtkWidget *gnostr_session_view_get_article_reader(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);
  return self->article_reader;
}

gboolean gnostr_session_view_is_showing_profile(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), FALSE);
  return self->showing_profile;
}

void gnostr_session_view_set_new_notes_count(GnostrSessionView *self, guint count) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));

  /* Store the count for re-showing when switching back to timeline */
  self->pending_new_notes_count = count;

  if (count > 0) {
    char *label_text = g_strdup_printf(ngettext("%u New Note", "%u New Notes", count), count);
    if (self->lbl_new_notes_count) {
      gtk_label_set_text(self->lbl_new_notes_count, label_text);
    }
    g_free(label_text);
    if (self->new_notes_revealer) {
      /* Only show new notes toast on timeline view */
      const char *visible_name = self->stack ? adw_view_stack_get_visible_child_name(self->stack) : NULL;
      gboolean on_timeline = visible_name && g_strcmp0(visible_name, "timeline") == 0;
      gtk_revealer_set_reveal_child(self->new_notes_revealer, on_timeline);
    }
  } else {
    /* Reset loading state - show arrow, hide spinner (nostrc-3r8k) */
    if (self->new_notes_loading) {
      self->new_notes_loading = FALSE;
      if (self->img_new_notes_arrow)
        gtk_widget_set_visible(GTK_WIDGET(self->img_new_notes_arrow), TRUE);
      if (self->spinner_new_notes) {
        gtk_spinner_set_spinning(self->spinner_new_notes, FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->spinner_new_notes), FALSE);
      }
    }
    if (self->new_notes_revealer) {
      gtk_revealer_set_reveal_child(self->new_notes_revealer, FALSE);
    }
  }
}

void gnostr_session_view_set_relay_status(GnostrSessionView *self,
                                          guint connected_count,
                                          guint total_count) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));

  /* Update the status label */
  if (self->relay_status_label) {
    char *label = g_strdup_printf("%u/%u", connected_count, total_count);
    gtk_label_set_text(self->relay_status_label, label);
    g_free(label);
  }

  /* Update the count labels in popover */
  if (self->lbl_connected_count) {
    char *count = g_strdup_printf("%u", connected_count);
    gtk_label_set_text(self->lbl_connected_count, count);
    g_free(count);
  }

  if (self->lbl_total_count) {
    char *count = g_strdup_printf("%u", total_count);
    gtk_label_set_text(self->lbl_total_count, count);
    g_free(count);
  }

  /* Update status icon based on connection state */
  if (self->relay_status_icon) {
    /* Remove existing style classes */
    gtk_widget_remove_css_class(GTK_WIDGET(self->relay_status_icon), "success");
    gtk_widget_remove_css_class(GTK_WIDGET(self->relay_status_icon), "warning");
    gtk_widget_remove_css_class(GTK_WIDGET(self->relay_status_icon), "error");
    gtk_widget_remove_css_class(GTK_WIDGET(self->relay_status_icon), "dim-label");

    if (total_count == 0) {
      /* No relays configured */
      gtk_image_set_from_icon_name(self->relay_status_icon, "network-offline-symbolic");
      gtk_widget_add_css_class(GTK_WIDGET(self->relay_status_icon), "dim-label");
    } else if (connected_count == 0) {
      /* All relays disconnected */
      gtk_image_set_from_icon_name(self->relay_status_icon, "network-offline-symbolic");
      gtk_widget_add_css_class(GTK_WIDGET(self->relay_status_icon), "error");
    } else if (connected_count < total_count) {
      /* Some relays connected */
      gtk_image_set_from_icon_name(self->relay_status_icon, "network-wired-symbolic");
      gtk_widget_add_css_class(GTK_WIDGET(self->relay_status_icon), "warning");
    } else {
      /* All relays connected */
      gtk_image_set_from_icon_name(self->relay_status_icon, "network-wired-symbolic");
      gtk_widget_add_css_class(GTK_WIDGET(self->relay_status_icon), "success");
    }
  }

  /* Show/hide reconnect button based on connection state */
  if (self->btn_reconnect) {
    gboolean show_reconnect = (total_count > 0 && connected_count < total_count);
    gtk_widget_set_visible(GTK_WIDGET(self->btn_reconnect), show_reconnect);
  }
}

void gnostr_session_view_set_search_mode(GnostrSessionView *self, gboolean enabled) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));

  if (self->search_bar) {
    gtk_search_bar_set_search_mode(self->search_bar, enabled);
    if (enabled && self->search_entry) {
      gtk_widget_grab_focus(GTK_WIDGET(self->search_entry));
    }
  }
}

gboolean gnostr_session_view_get_search_mode(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), FALSE);

  if (self->search_bar) {
    return gtk_search_bar_get_search_mode(self->search_bar);
  }
  return FALSE;
}

const char *gnostr_session_view_get_search_text(GnostrSessionView *self) {
  g_return_val_if_fail(GNOSTR_IS_SESSION_VIEW(self), NULL);

  if (self->search_entry) {
    return gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
  }
  return NULL;
}

void gnostr_session_view_refresh_account_list(GnostrSessionView *self) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  rebuild_account_list(self);
}

void gnostr_session_view_set_user_profile(GnostrSessionView *self,
                                          const char *pubkey_hex,
                                          const char *display_name,
                                          const char *avatar_url) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));

  /* nostrc-akyz: defensively normalize npub/nprofile to hex */
  g_autofree gchar *hex = gnostr_ensure_hex_pubkey(pubkey_hex);
  if (!hex) return;

  /* Cache the pubkey for future profile updates */
  g_free(self->current_pubkey_hex);
  self->current_pubkey_hex = g_strdup(hex);

  /* Update popover avatar and name if popover exists */
  if (self->lbl_profile_name) {
    if (display_name && *display_name) {
      gtk_label_set_text(self->lbl_profile_name, display_name);
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_profile_name), TRUE);
    } else if (hex && *hex) {
      /* Show truncated pubkey if no display name */
      char *truncated = g_strdup_printf("%.8s...%.4s", hex, hex + 60);
      gtk_label_set_text(self->lbl_profile_name, truncated);
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_profile_name), TRUE);
      g_free(truncated);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_profile_name), FALSE);
    }
  }

  /* Update popover avatar initials */
  if (self->popover_avatar_initials) {
    set_initials_label(self->popover_avatar_initials, display_name, hex);
  }

  /* Load popover avatar image */
  if (self->popover_avatar_image && avatar_url && *avatar_url) {
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(self->popover_avatar_image), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->popover_avatar_image, TRUE);
      if (self->popover_avatar_initials) {
        gtk_widget_set_visible(self->popover_avatar_initials, FALSE);
      }
      g_object_unref(cached);
    } else {
      gnostr_avatar_download_async(avatar_url, self->popover_avatar_image, self->popover_avatar_initials);
    }
  } else if (self->popover_avatar_image) {
    /* No avatar URL - show initials */
    gtk_widget_set_visible(self->popover_avatar_image, FALSE);
    if (self->popover_avatar_initials) {
      gtk_widget_set_visible(self->popover_avatar_initials, TRUE);
    }
  }

  /* Update header bar avatar button */
  if (self->header_avatar_initials) {
    set_initials_label(self->header_avatar_initials, display_name, hex);
  }

  if (self->header_avatar_image && avatar_url && *avatar_url) {
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(self->header_avatar_image), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->header_avatar_image, TRUE);
      if (self->header_avatar_initials) {
        gtk_widget_set_visible(self->header_avatar_initials, FALSE);
      }
      g_object_unref(cached);
    } else {
      gnostr_avatar_download_async(avatar_url, self->header_avatar_image, self->header_avatar_initials);
    }
  } else if (self->header_avatar_image) {
    /* No avatar URL - show initials */
    gtk_widget_set_visible(self->header_avatar_image, FALSE);
    if (self->header_avatar_initials) {
      gtk_widget_set_visible(self->header_avatar_initials, TRUE);
    }
  }

  /* Also refresh the account list to show updated avatars */
  rebuild_account_list(self);
}

/* Helper: Create a plugin sidebar row with icon + label */
static GtkWidget *create_plugin_sidebar_row(const char *label, const char *icon_name) {
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 10);
  gtk_widget_set_margin_bottom(box, 10);

  /* Icon */
  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name ? icon_name : "application-x-addon-symbolic");
  gtk_widget_add_css_class(icon, "dim-label");
  gtk_box_append(GTK_BOX(box), icon);

  /* Label */
  GtkWidget *label_widget = gtk_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(label_widget), 0);
  gtk_box_append(GTK_BOX(box), label_widget);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

  return row;
}

/* Helper: Ensure the plugin separator exists in the sidebar */
static void ensure_plugin_separator(GnostrSessionView *self) {
  if (self->plugin_separator) return;
  if (!self->sidebar_list) return;

  /* Create and add the separator */
  self->plugin_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top(self->plugin_separator, 8);
  gtk_widget_set_margin_bottom(self->plugin_separator, 8);
  gtk_widget_set_margin_start(self->plugin_separator, 12);
  gtk_widget_set_margin_end(self->plugin_separator, 12);

  /* Wrap in a non-activatable row for consistent ListBox behavior */
  GtkWidget *sep_row = gtk_list_box_row_new();
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(sep_row), FALSE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(sep_row), FALSE);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(sep_row), self->plugin_separator);

  /* Add after the last fixed row (row_repos) */
  gtk_list_box_append(self->sidebar_list, sep_row);
}

void gnostr_session_view_add_plugin_sidebar_item(GnostrSessionView *self,
                                                  const char *panel_id,
                                                  const char *label,
                                                  const char *icon_name,
                                                  gboolean requires_auth,
                                                  int position,
                                                  gpointer extension,
                                                  gpointer context) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  g_return_if_fail(panel_id != NULL);
  g_return_if_fail(label != NULL);

  /* Check if already exists */
  if (g_hash_table_contains(self->plugin_rows, panel_id)) {
    g_warning("Plugin sidebar item '%s' already exists", panel_id);
    return;
  }

  /* Ensure plugin separator exists */
  ensure_plugin_separator(self);

  /* Create the sidebar row */
  GtkWidget *row = create_plugin_sidebar_row(label, icon_name);

  /* Store in hash tables */
  g_hash_table_insert(self->plugin_rows, g_strdup(panel_id), row);
  g_hash_table_insert(self->plugin_labels, g_strdup(panel_id), g_strdup(label));
  g_hash_table_insert(self->plugin_auth_required, g_strdup(panel_id), GINT_TO_POINTER(requires_auth));

  /* Store extension and context if provided */
  if (extension) {
    g_hash_table_insert(self->plugin_extensions, g_strdup(panel_id), g_object_ref(extension));
  }
  if (context) {
    g_hash_table_insert(self->plugin_contexts, g_strdup(panel_id), context);
  }

  /* Add to sidebar_list */
  if (self->sidebar_list) {
    /* Position: 0 means first plugin item (after separator), -1 means end */
    /* For now, we just append - position handling could be improved */
    (void)position;  /* Unused for now - append at end */
    gtk_list_box_append(self->sidebar_list, row);
  }

  /* Apply auth gating if needed */
  if (requires_auth && !self->authenticated) {
    gtk_widget_set_sensitive(row, FALSE);
  }

  g_debug("Added plugin sidebar item: %s (%s)", panel_id, label);
}

void gnostr_session_view_remove_plugin_sidebar_item(GnostrSessionView *self,
                                                     const char *panel_id) {
  g_return_if_fail(GNOSTR_IS_SESSION_VIEW(self));
  g_return_if_fail(panel_id != NULL);

  /* Remove the sidebar row */
  GtkWidget *row = g_hash_table_lookup(self->plugin_rows, panel_id);
  if (row && self->sidebar_list) {
    gtk_list_box_remove(self->sidebar_list, row);
  }

  /* Remove from stack if panel was created */
  GtkWidget *panel = g_hash_table_lookup(self->plugin_panels, panel_id);
  if (panel && self->stack) {
    adw_view_stack_remove(self->stack, panel);
  }

  /* Clean up hash table entries */
  g_hash_table_remove(self->plugin_rows, panel_id);
  g_hash_table_remove(self->plugin_panels, panel_id);
  g_hash_table_remove(self->plugin_extensions, panel_id);
  g_hash_table_remove(self->plugin_contexts, panel_id);
  g_hash_table_remove(self->plugin_labels, panel_id);
  g_hash_table_remove(self->plugin_auth_required, panel_id);

  /* If no more plugin items, remove the separator */
  if (g_hash_table_size(self->plugin_rows) == 0 && self->plugin_separator) {
    GtkWidget *sep_row = gtk_widget_get_parent(self->plugin_separator);
    if (sep_row && self->sidebar_list) {
      gtk_list_box_remove(self->sidebar_list, sep_row);
    }
    self->plugin_separator = NULL;
  }

  g_debug("Removed plugin sidebar item: %s", panel_id);
}