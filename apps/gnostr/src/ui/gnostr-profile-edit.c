#include "gnostr-profile-edit.h"
#include "../ipc/signer_ipc.h"
#include "../ipc/gnostr-signer-service.h"
#include "../util/nip39_identity.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include "../util/utils.h"
#include <nostr-gobject-1.0/nostr_json.h>
#include <json.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <time.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/dialogs/gnostr-profile-edit.ui"

struct _GnostrProfileEdit {
  GtkWindow parent_instance;

  /* Template children */
  GtkWidget *entry_display_name;
  GtkWidget *entry_name;
  GtkWidget *text_about;
  GtkWidget *entry_picture;
  GtkWidget *entry_banner;
  GtkWidget *entry_nip05;
  GtkWidget *entry_website;
  GtkWidget *entry_lud16;
  GtkWidget *entry_lud06;   /* NIP-24: LNURL pay bech32 */
  GtkWidget *switch_bot;    /* NIP-24: Bot indicator */
  GtkWidget *btn_cancel;
  GtkWidget *btn_save;
  GtkWidget *spinner;
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;

  /* NIP-39 External Identities UI */
  GtkWidget *identities_section;    /* Container for identities section */
  GtkWidget *identities_list;       /* ListBox for identity rows */
  GtkWidget *btn_add_identity;      /* Button to add new identity */

  /* State */
  gboolean saving;
  gboolean disposed;                /* hq-6zc5r: prevent async callbacks post-dispose */
  char *original_json;              /* Preserve unknown fields (raw JSON string) */
  GPtrArray *external_identities;   /* GnostrExternalIdentity* array */
};

G_DEFINE_TYPE(GnostrProfileEdit, gnostr_profile_edit, GTK_TYPE_WINDOW)

/* Signals */
enum {
  SIGNAL_PROFILE_SAVED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_cancel_clicked(GtkButton *btn, gpointer user_data);
static void on_save_clicked(GtkButton *btn, gpointer user_data);
static void show_toast(GnostrProfileEdit *self, const char *msg);
static void on_add_identity_clicked(GtkButton *btn, gpointer user_data);

static void gnostr_profile_edit_dispose(GObject *obj) {
  GnostrProfileEdit *self = GNOSTR_PROFILE_EDIT(obj);
  /* hq-6zc5r: Mark disposed before cleanup to prevent async publish callbacks
   * from accessing template widgets after dispose. */
  self->disposed = TRUE;
  g_clear_pointer(&self->original_json, g_free);
  g_clear_pointer(&self->external_identities, g_ptr_array_unref);
  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_PROFILE_EDIT);
  G_OBJECT_CLASS(gnostr_profile_edit_parent_class)->dispose(obj);
}

static void gnostr_profile_edit_finalize(GObject *obj) {
  G_OBJECT_CLASS(gnostr_profile_edit_parent_class)->finalize(obj);
}

static void gnostr_profile_edit_class_init(GnostrProfileEditClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_profile_edit_dispose;
  gclass->finalize = gnostr_profile_edit_finalize;

  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);

  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_display_name);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_name);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, text_about);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_picture);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_banner);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_nip05);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_website);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_lud16);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_lud06);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, switch_bot);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, btn_cancel);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, btn_save);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, spinner);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, toast_revealer);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, toast_label);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, identities_section);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, identities_list);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, btn_add_identity);

  gtk_widget_class_bind_template_callback(wclass, on_cancel_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_save_clicked);

  /**
   * GnostrProfileEdit::profile-saved:
   * @self: the profile edit dialog
   * @profile_json: the new profile JSON content
   *
   * Emitted when the profile has been successfully signed and published.
   */
  signals[SIGNAL_PROFILE_SAVED] = g_signal_new(
    "profile-saved",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1,
    G_TYPE_STRING);
}

static void gnostr_profile_edit_init(GnostrProfileEdit *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  self->saving = FALSE;
  self->original_json = NULL;
  self->external_identities = NULL;

  /* Connect button signals */
  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), self);
  g_signal_connect(self->btn_save, "clicked", G_CALLBACK(on_save_clicked), self);

  /* Connect add identity button */
  if (self->btn_add_identity) {
    g_signal_connect(self->btn_add_identity, "clicked", G_CALLBACK(on_add_identity_clicked), self);
  }
}

GnostrProfileEdit *gnostr_profile_edit_new(GtkWindow *parent) {
  GnostrProfileEdit *self = g_object_new(GNOSTR_TYPE_PROFILE_EDIT,
                                          "transient-for", parent,
                                          "modal", TRUE,
                                          NULL);
  return self;
}

static gboolean hide_toast_timeout_cb(gpointer user_data) {
  gtk_revealer_set_reveal_child(GTK_REVEALER(user_data), FALSE);
  return G_SOURCE_REMOVE;
}

static void show_toast(GnostrProfileEdit *self, const char *msg) {
  if (!self->toast_label || !self->toast_revealer) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), TRUE);
  /* Auto-hide after 3 seconds */
  g_timeout_add_full(G_PRIORITY_DEFAULT, 3000, hide_toast_timeout_cb,
                     g_object_ref(self->toast_revealer), g_object_unref);
}

static void set_ui_sensitive(GnostrProfileEdit *self, gboolean sensitive) {
  gtk_widget_set_sensitive(self->entry_display_name, sensitive);
  gtk_widget_set_sensitive(self->entry_name, sensitive);
  gtk_widget_set_sensitive(self->text_about, sensitive);
  gtk_widget_set_sensitive(self->entry_picture, sensitive);
  gtk_widget_set_sensitive(self->entry_banner, sensitive);
  gtk_widget_set_sensitive(self->entry_nip05, sensitive);
  gtk_widget_set_sensitive(self->entry_website, sensitive);
  gtk_widget_set_sensitive(self->entry_lud16, sensitive);
  gtk_widget_set_sensitive(self->entry_lud06, sensitive);
  gtk_widget_set_sensitive(self->switch_bot, sensitive);
  gtk_widget_set_sensitive(self->btn_save, sensitive);
  gtk_widget_set_sensitive(self->btn_cancel, sensitive);

  if (self->spinner) {
    gtk_widget_set_visible(self->spinner, !sensitive);
    if (!sensitive) {
      gtk_spinner_start(GTK_SPINNER(self->spinner));
    } else {
      gtk_spinner_stop(GTK_SPINNER(self->spinner));
    }
  }
}

void gnostr_profile_edit_set_profile_json(GnostrProfileEdit *self, const char *profile_json) {
  g_return_if_fail(GNOSTR_IS_PROFILE_EDIT(self));

  /* Clear any previous original JSON */
  g_clear_pointer(&self->original_json, g_free);

  if (!profile_json || !*profile_json) {
    return;
  }

  if (!gnostr_json_is_valid(profile_json)) {
    g_warning("ProfileEdit: failed to parse profile JSON");
    return;
  }

  /* Store original for preserving unknown fields */
  self->original_json = g_strdup(profile_json);

  /* Extract and populate fields */
  char *val = NULL;

  val = gnostr_json_get_string(profile_json, "display_name", NULL);
  if (val) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_display_name), val);
    g_free(val); val = NULL;
  }
  val = gnostr_json_get_string(profile_json, "name", NULL);
  if (val) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_name), val);
    g_free(val); val = NULL;
  }
  val = gnostr_json_get_string(profile_json, "about", NULL);
  if (val) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_about));
    gtk_text_buffer_set_text(buffer, val, -1);
    g_free(val); val = NULL;
  }
  val = gnostr_json_get_string(profile_json, "picture", NULL);
  if (val) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_picture), val);
    g_free(val); val = NULL;
  }
  val = gnostr_json_get_string(profile_json, "banner", NULL);
  if (val) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_banner), val);
    g_free(val); val = NULL;
  }
  val = gnostr_json_get_string(profile_json, "nip05", NULL);
  if (val) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_nip05), val);
    g_free(val); val = NULL;
  }
  val = gnostr_json_get_string(profile_json, "website", NULL);
  if (val) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_website), val);
    g_free(val); val = NULL;
  }
  val = gnostr_json_get_string(profile_json, "lud16", NULL);
  if (val) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_lud16), val);
    g_free(val); val = NULL;
  }
  /* NIP-24: lud06 LNURL pay address */
  val = gnostr_json_get_string(profile_json, "lud06", NULL);
  if (val) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_lud06), val);
    g_free(val); val = NULL;
  }
  /* NIP-24: bot indicator - can be boolean or string "true" */
  gboolean is_bot = FALSE;
  if (gnostr_json_has_key(profile_json, "bot")) {
    GError *bot_err = NULL;
    gboolean bot_val = gnostr_json_get_boolean(profile_json, "bot", &bot_err);
    if (!bot_err) {
      is_bot = bot_val;
    } else {
      g_error_free(bot_err);
      val = gnostr_json_get_string(profile_json, "bot", NULL);
      if (val) {
        is_bot = (g_ascii_strcasecmp(val, "true") == 0);
        g_free(val); val = NULL;
      }
    }
  } else if ((val = gnostr_json_get_string(profile_json, "bot", NULL)) != NULL && val) {
    is_bot = (g_ascii_strcasecmp(val, "true") == 0);
    g_free(val); val = NULL;
  }
  gtk_switch_set_active(GTK_SWITCH(self->switch_bot), is_bot);
}

char *gnostr_profile_edit_get_profile_json(GnostrProfileEdit *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE_EDIT(self), NULL);

  /* Build profile JSON from form fields */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  /* Helper macro to add field if non-empty */
  #define ADD_IF_SET(field_name, widget) do { \
    const char *text = gtk_editable_get_text(GTK_EDITABLE(widget)); \
    if (text && *text) { \
      gnostr_json_builder_set_key(builder, field_name); \
      gnostr_json_builder_add_string(builder, text); \
    } \
  } while (0)

  ADD_IF_SET("display_name", self->entry_display_name);
  ADD_IF_SET("name", self->entry_name);
  ADD_IF_SET("picture", self->entry_picture);
  ADD_IF_SET("banner", self->entry_banner);
  ADD_IF_SET("nip05", self->entry_nip05);
  ADD_IF_SET("website", self->entry_website);
  ADD_IF_SET("lud16", self->entry_lud16);
  ADD_IF_SET("lud06", self->entry_lud06);  /* NIP-24: LNURL pay */

  #undef ADD_IF_SET

  /* NIP-24: Handle bot field - only include if true */
  gboolean is_bot = gtk_switch_get_active(GTK_SWITCH(self->switch_bot));
  if (is_bot) {
    gnostr_json_builder_set_key(builder, "bot");
    gnostr_json_builder_add_boolean(builder, true);
  }

  /* Handle about field (TextView) */
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_about));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  char *about_text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  if (about_text && *about_text) {
    gnostr_json_builder_set_key(builder, "about");
    gnostr_json_builder_add_string(builder, about_text);
  }
  g_free(about_text);

  gnostr_json_builder_end_object(builder);
  char *result = gnostr_json_builder_finish(builder);
  g_object_unref(builder);
  return result;
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrProfileEdit *self = GNOSTR_PROFILE_EDIT(user_data);
  gtk_window_close(GTK_WINDOW(self));
}

/* Context for async sign-and-publish */
typedef struct {
  GnostrProfileEdit *self;
  char *profile_content;
} ProfilePublishContext;

static void profile_publish_context_free(ProfilePublishContext *ctx) {
  if (!ctx) return;
  g_clear_pointer(&ctx->profile_content, g_free);
  g_free(ctx);
}

/* hq-6zc5r: Callback when relay publishing completes */
static void profile_publish_done(guint success_count, guint fail_count, gpointer user_data) {
  ProfilePublishContext *ctx = (ProfilePublishContext *)user_data;
  if (!ctx) return;

  g_debug("[PROFILE_EDIT] Published to %u relays, failed %u", success_count, fail_count);

  if (GNOSTR_IS_PROFILE_EDIT(ctx->self) && !ctx->self->disposed) {
    GnostrProfileEdit *self = ctx->self;
    if (success_count > 0) {
      /* hq-6zc5r: Close on confirmed relay acceptance */
      gtk_window_close(GTK_WINDOW(self));
    } else {
      show_toast(self, "Failed to publish profile. Try again.");
      set_ui_sensitive(self, TRUE);
      self->saving = FALSE;
    }
  }

  profile_publish_context_free(ctx);
}

/* Callback when unified signer service completes signing for profile */
static void on_profile_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  ProfilePublishContext *ctx = (ProfilePublishContext *)user_data;
  (void)source;
  if (!ctx) return;

  GnostrProfileEdit *self = ctx->self;
  if (!GNOSTR_IS_PROFILE_EDIT(self)) {
    profile_publish_context_free(ctx);
    return;
  }

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    char *msg = g_strdup_printf("Signing failed: %s", error ? error->message : "unknown error");
    show_toast(self, msg);
    g_free(msg);
    g_clear_error(&error);
    set_ui_sensitive(self, TRUE);
    self->saving = FALSE;
    profile_publish_context_free(ctx);
    return;
  }

  g_message("[PROFILE_EDIT] Signed event: %.100s...", signed_event_json);

  /* Emit signal with profile content for immediate local UI update */
  g_signal_emit(self, signals[SIGNAL_PROFILE_SAVED], 0, ctx->profile_content);

  /* hq-6zc5r: Publish to relays and close on confirmation, not blind timeout.
   * Parse signed event for relay publishing. */
  NostrEvent *event = nostr_event_new();
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
  g_free(signed_event_json);

  if (parse_rc != 1) {
    show_toast(self, "Failed to parse signed profile event");
    nostr_event_free(event);
    set_ui_sensitive(self, TRUE);
    self->saving = FALSE;
    profile_publish_context_free(ctx);
    return;
  }

  GPtrArray *write_relays = gnostr_get_write_relay_urls();
  if (!write_relays || write_relays->len == 0) {
    show_toast(self, "No write relays configured");
    nostr_event_free(event);
    if (write_relays) g_ptr_array_unref(write_relays);
    set_ui_sensitive(self, TRUE);
    self->saving = FALSE;
    profile_publish_context_free(ctx);
    return;
  }

  show_toast(self, "Publishing profile...");

  /* gnostr_publish_to_relays_async takes ownership of event and write_relays.
   * ctx ownership transferred to profile_publish_done callback. */
  gnostr_publish_to_relays_async(event, write_relays,
    profile_publish_done, ctx);
}

static void on_save_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrProfileEdit *self = GNOSTR_PROFILE_EDIT(user_data);

  if (self->saving) return;

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    show_toast(self, "Signer not available");
    return;
  }

  /* Disable UI while saving */
  self->saving = TRUE;
  set_ui_sensitive(self, FALSE);
  show_toast(self, "Signing profile...");

  /* Build profile content JSON */
  char *profile_content = gnostr_profile_edit_get_profile_json(self);
  if (!profile_content) {
    show_toast(self, "Failed to serialize profile");
    set_ui_sensitive(self, TRUE);
    self->saving = FALSE;
    return;
  }

  /* Build unsigned kind 0 event JSON with GNostrJsonBuilder */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, 0);

  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));

  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, profile_content);

  /* Build tags array including NIP-39 identity tags */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* Add NIP-39 external identity "i" tags */
  if (self->external_identities && self->external_identities->len > 0) {
    for (guint i = 0; i < self->external_identities->len; i++) {
      GnostrExternalIdentity *identity = g_ptr_array_index(self->external_identities, i);
      if (!identity || !identity->platform_name || !identity->identity) {
        continue;
      }

      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "i");

      /* Build "platform:identity" string */
      char *tag_value = g_strdup_printf("%s:%s", identity->platform_name, identity->identity);
      gnostr_json_builder_add_string(builder, tag_value);
      g_free(tag_value);

      /* Add proof URL if present */
      if (identity->proof_url && *identity->proof_url) {
        gnostr_json_builder_add_string(builder, identity->proof_url);
      }

      gnostr_json_builder_end_array(builder);
    }
  }

  gnostr_json_builder_end_array(builder);  /* end tags */
  gnostr_json_builder_end_object(builder); /* end event */

  char *event_json = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

  if (!event_json) {
    show_toast(self, "Failed to build event JSON");
    g_free(profile_content);
    set_ui_sensitive(self, TRUE);
    self->saving = FALSE;
    return;
  }

  g_message("[PROFILE_EDIT] Unsigned event: %s", event_json);

  /* Create async context */
  ProfilePublishContext *ctx = g_new0(ProfilePublishContext, 1);
  ctx->self = self;
  ctx->profile_content = profile_content; /* Transfer ownership */

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",        /* current_user: ignored */
    "gnostr",  /* app_id: ignored */
    NULL,      /* cancellable */
    on_profile_sign_complete,
    ctx
  );
  g_free(event_json);
}

/* ============== NIP-39 External Identity Support ============== */

/* Forward declarations for identity support */
static void on_identity_delete_clicked(GtkButton *btn, gpointer user_data);
static void rebuild_identities_list(GnostrProfileEdit *self);

/* Create an identity row widget for the list */
static GtkWidget *create_identity_edit_row(GnostrProfileEdit *self, GnostrExternalIdentity *identity, guint index) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);
  gtk_widget_set_margin_start(row, 4);
  gtk_widget_set_margin_end(row, 4);

  /* Store index for later use */
  g_object_set_data(G_OBJECT(row), "identity-index", GUINT_TO_POINTER(index));

  /* Platform icon */
  const char *icon_name = gnostr_nip39_get_platform_icon(identity->platform);
  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_box_append(GTK_BOX(row), icon);

  /* Platform name */
  const char *platform_name = gnostr_nip39_get_platform_display_name(identity->platform);
  GtkWidget *platform_lbl = gtk_label_new(platform_name);
  gtk_widget_add_css_class(platform_lbl, "dim-label");
  gtk_box_append(GTK_BOX(row), platform_lbl);

  /* Identity value */
  GtkWidget *identity_lbl = gtk_label_new(identity->identity);
  gtk_label_set_ellipsize(GTK_LABEL(identity_lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(identity_lbl, TRUE);
  gtk_label_set_xalign(GTK_LABEL(identity_lbl), 0.0);
  gtk_box_append(GTK_BOX(row), identity_lbl);

  /* Proof indicator */
  if (identity->proof_url && *identity->proof_url) {
    GtkWidget *proof_icon = gtk_image_new_from_icon_name("emblem-documents-symbolic");
    gtk_widget_set_tooltip_text(proof_icon, identity->proof_url);
    gtk_box_append(GTK_BOX(row), proof_icon);
  }

  /* Delete button */
  GtkWidget *delete_btn = gtk_button_new_from_icon_name("edit-delete-symbolic");
  gtk_widget_add_css_class(delete_btn, "flat");
  gtk_widget_add_css_class(delete_btn, "destructive-action");
  gtk_widget_set_tooltip_text(delete_btn, "Remove this identity");
  g_object_set_data(G_OBJECT(delete_btn), "profile-edit", self);
  g_object_set_data(G_OBJECT(delete_btn), "identity-index", GUINT_TO_POINTER(index));
  g_signal_connect(delete_btn, "clicked", G_CALLBACK(on_identity_delete_clicked), self);
  gtk_box_append(GTK_BOX(row), delete_btn);

  return row;
}

/* Rebuild the identities list UI */
static void rebuild_identities_list(GnostrProfileEdit *self) {
  if (!self->identities_list) return;

  /* Clear existing children */
  GtkWidget *child = gtk_widget_get_first_child(self->identities_list);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(self->identities_list), child);
    child = next;
  }

  /* Add rows for each identity */
  if (self->external_identities) {
    for (guint i = 0; i < self->external_identities->len; i++) {
      GnostrExternalIdentity *identity = g_ptr_array_index(self->external_identities, i);
      GtkWidget *row = create_identity_edit_row(self, identity, i);
      gtk_box_append(GTK_BOX(self->identities_list), row);
    }
  }

  /* Show section if we have identities */
  if (self->identities_section) {
    gboolean has_identities = self->external_identities && self->external_identities->len > 0;
    gtk_widget_set_visible(self->identities_section, has_identities || TRUE);  /* Always show for adding */
  }
}

/* Callback when delete button is clicked */
static void on_identity_delete_clicked(GtkButton *btn, gpointer user_data) {
  GnostrProfileEdit *self = GNOSTR_PROFILE_EDIT(user_data);
  guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "identity-index"));

  if (!self->external_identities || index >= self->external_identities->len) {
    return;
  }

  g_ptr_array_remove_index(self->external_identities, index);
  rebuild_identities_list(self);
}

/* Dialog for adding new external identity */
typedef struct {
  GnostrProfileEdit *self;
  GtkWidget *dialog;
  GtkWidget *platform_dropdown;
  GtkWidget *identity_entry;
  GtkWidget *proof_entry;
} AddIdentityDialog;

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void on_add_identity_response(GtkDialog *dialog, int response, gpointer user_data) {
  AddIdentityDialog *ctx = (AddIdentityDialog *)user_data;

  if (response == GTK_RESPONSE_OK) {
    /* Get selected platform */
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->platform_dropdown));
    const char *platforms[] = { "github", "twitter", "mastodon", "telegram", "keybase", "reddit", "website" };
    const char *platform_str = (selected < G_N_ELEMENTS(platforms)) ? platforms[selected] : "github";

    const char *identity = gtk_editable_get_text(GTK_EDITABLE(ctx->identity_entry));
    const char *proof = gtk_editable_get_text(GTK_EDITABLE(ctx->proof_entry));

    if (identity && *identity) {
      /* Build tag value */
      char *tag_value = g_strdup_printf("%s:%s", platform_str, identity);
      GnostrExternalIdentity *new_identity = gnostr_nip39_parse_identity(tag_value, proof && *proof ? proof : NULL);
      g_free(tag_value);

      if (new_identity) {
        if (!ctx->self->external_identities) {
          ctx->self->external_identities = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_external_identity_free);
        }
        g_ptr_array_add(ctx->self->external_identities, new_identity);
        rebuild_identities_list(ctx->self);
      }
    }
  }

  gtk_window_destroy(GTK_WINDOW(dialog));
  g_free(ctx);
}

static void on_add_identity_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrProfileEdit *self = GNOSTR_PROFILE_EDIT(user_data);

  /* Create dialog */
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
    "Add External Identity",
    GTK_WINDOW(self),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    "Cancel", GTK_RESPONSE_CANCEL,
    "Add", GTK_RESPONSE_OK,
    NULL);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_box_append(GTK_BOX(content), box);

  /* Platform dropdown */
  GtkWidget *platform_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *platform_label = gtk_label_new("Platform");
  gtk_label_set_xalign(GTK_LABEL(platform_label), 0.0);
  gtk_widget_add_css_class(platform_label, "dim-label");
  gtk_box_append(GTK_BOX(platform_box), platform_label);

  const char *platforms[] = { "GitHub", "Twitter/X", "Mastodon", "Telegram", "Keybase", "Reddit", "Website" };
  GtkStringList *platform_model = gtk_string_list_new(platforms);
  GtkWidget *platform_dropdown = gtk_drop_down_new(G_LIST_MODEL(platform_model), NULL);
  gtk_box_append(GTK_BOX(platform_box), platform_dropdown);
  gtk_box_append(GTK_BOX(box), platform_box);

  /* Identity entry */
  GtkWidget *identity_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *identity_label = gtk_label_new("Username/Handle");
  gtk_label_set_xalign(GTK_LABEL(identity_label), 0.0);
  gtk_widget_add_css_class(identity_label, "dim-label");
  gtk_box_append(GTK_BOX(identity_box), identity_label);

  GtkWidget *identity_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(identity_entry), "your_username");
  gtk_box_append(GTK_BOX(identity_box), identity_entry);
  gtk_box_append(GTK_BOX(box), identity_box);

  /* Proof URL entry */
  GtkWidget *proof_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *proof_label = gtk_label_new("Proof URL (optional)");
  gtk_label_set_xalign(GTK_LABEL(proof_label), 0.0);
  gtk_widget_add_css_class(proof_label, "dim-label");
  gtk_box_append(GTK_BOX(proof_box), proof_label);

  GtkWidget *proof_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(proof_entry), "https://gist.github.com/...");
  gtk_entry_set_input_purpose(GTK_ENTRY(proof_entry), GTK_INPUT_PURPOSE_URL);
  gtk_box_append(GTK_BOX(proof_box), proof_entry);

  GtkWidget *proof_hint = gtk_label_new("Link to a post containing your Nostr pubkey");
  gtk_label_set_xalign(GTK_LABEL(proof_hint), 0.0);
  gtk_widget_add_css_class(proof_hint, "caption");
  gtk_widget_add_css_class(proof_hint, "dim-label");
  gtk_box_append(GTK_BOX(proof_box), proof_hint);
  gtk_box_append(GTK_BOX(box), proof_box);

  /* Context for response handler */
  AddIdentityDialog *ctx = g_new0(AddIdentityDialog, 1);
  ctx->self = self;
  ctx->dialog = dialog;
  ctx->platform_dropdown = platform_dropdown;
  ctx->identity_entry = identity_entry;
  ctx->proof_entry = proof_entry;

  g_signal_connect(dialog, "response", G_CALLBACK(on_add_identity_response), ctx);

  gtk_window_present(GTK_WINDOW(dialog));
}
G_GNUC_END_IGNORE_DEPRECATIONS

/* Create the identities section UI (called from init) */
static void create_identities_section(GnostrProfileEdit *self, GtkWidget *parent_box) {
  /* Section container */
  self->identities_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(self->identities_section, 16);

  /* Section header */
  GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *header_label = gtk_label_new("External Identities (NIP-39)");
  gtk_label_set_xalign(GTK_LABEL(header_label), 0.0);
  gtk_widget_add_css_class(header_label, "heading");
  gtk_widget_set_hexpand(header_label, TRUE);
  gtk_box_append(GTK_BOX(header_box), header_label);

  /* Add button */
  self->btn_add_identity = gtk_button_new_from_icon_name("list-add-symbolic");
  gtk_widget_add_css_class(self->btn_add_identity, "flat");
  gtk_widget_set_tooltip_text(self->btn_add_identity, "Add external identity");
  g_signal_connect(self->btn_add_identity, "clicked", G_CALLBACK(on_add_identity_clicked), self);
  gtk_box_append(GTK_BOX(header_box), self->btn_add_identity);
  gtk_box_append(GTK_BOX(self->identities_section), header_box);

  /* Description */
  GtkWidget *desc = gtk_label_new("Link your accounts from other platforms to prove ownership.");
  gtk_label_set_xalign(GTK_LABEL(desc), 0.0);
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_widget_add_css_class(desc, "dim-label");
  gtk_widget_add_css_class(desc, "caption");
  gtk_box_append(GTK_BOX(self->identities_section), desc);

  /* Identities list */
  self->identities_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(self->identities_list, "card");
  gtk_box_append(GTK_BOX(self->identities_section), self->identities_list);

  gtk_box_append(GTK_BOX(parent_box), self->identities_section);
}

void gnostr_profile_edit_set_event_json(GnostrProfileEdit *self, const char *event_json) {
  g_return_if_fail(GNOSTR_IS_PROFILE_EDIT(self));

  /* Clear existing identities */
  g_clear_pointer(&self->external_identities, g_ptr_array_unref);

  if (!event_json || !*event_json) {
    rebuild_identities_list(self);
    return;
  }

  /* Parse identities from event tags */
  self->external_identities = gnostr_nip39_parse_identities_from_event(event_json);
  rebuild_identities_list(self);
}

char *gnostr_profile_edit_get_identity_tags_json(GnostrProfileEdit *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE_EDIT(self), NULL);
  return gnostr_nip39_build_tags_json(self->external_identities);
}
