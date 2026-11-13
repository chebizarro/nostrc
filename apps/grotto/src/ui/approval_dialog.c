#include "../i18n.h"
#include "approval_dialog.h"
#include "nostr-kinds.h"
#include <string.h>
#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <json.h>
#include <nostr/nip19/nip19.h>

struct _GnostrApprovalDialog {
  AdwDialog parent_instance;
  AdwAvatar *app_avatar;
  GtkLabel *header_title, *label_verified, *label_details, *label_timestamp, *content_preview;
  AdwActionRow *row_action, *row_identity, *row_reason, *row_relay, *row_impact, *row_expiry;
  GtkLabel *scope_summary;
  AdwActionRow *saved_status, *stepper;
  AdwExpanderRow *details_expander, *batch_expander;
  AdwPreferencesGroup *group_saved, *group_flow;
  GtkBox *batch_box;
  AdwComboRow *permission_choice;
  GtkMenuButton *more_menu;
  GtkSpinner *loading;
  GSimpleActionGroup *actions;
  GSimpleAction *block_operation, *block_app;
  GVariant *info;
  gchar *app_name, *action_text, *reusable_text, *relay, *saved_expiry, *flow_scope;
  const gchar *choices[6];
  guint n_choices;
  GtkCheckButton *chk_flow_encrypt, *chk_flow_seals;
  GtkButton *btn_approve_selected;
  GtkButton *btn_deny, *btn_approve, *btn_not_now, *btn_previous, *btn_next;
  GnostrApprovalCallback callback;
  gpointer user_data;
  gchar *identity, *client_pubkey, *profile_details;
  gboolean finished, confirming, ready, initial_focus;
  guint position, count;
};
G_DEFINE_FINAL_TYPE(GnostrApprovalDialog, gnostr_approval_dialog, ADW_TYPE_DIALOG)
enum { DEFERRED, STEP, CHANGE_TRUST, APPROVE_SELECTED, APPROVE_FLOW, DECISION_MODE, N_SIGNALS };
static guint signals[N_SIGNALS];

static const char *field(GVariant *info, const char *key) {
  const char *s = "";
  if (info)
    g_variant_lookup(info, key, "&s", &s);
  return s;
}
static const struct { guint32 kind; const char *name; } kind_catalog[] = {
#include "kind-catalog.inc"
};
const char *gnostr_approval_kind_name(guint32 kind) {
  switch (kind) {
  case NOSTR_KIND_PROFILE_METADATA:
    return _("Profile metadata");
  case NOSTR_KIND_TEXT_NOTE:
    return _("Text note");
  case NOSTR_KIND_FOLLOW_LIST:
    return _("Contact list");
  case NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE:
    return _("Encrypted message");
  case NOSTR_KIND_DELETION:
    return _("Delete events");
  case NOSTR_KIND_REPOST:
  case NOSTR_KIND_GENERIC_REPOST:
    return _("Repost");
  case NOSTR_KIND_REACTION:
    return _("Reaction");
  case NOSTR_KIND_ARTICLE:
    return _("Article");
  case NOSTR_KIND_COMMENT:
    return _("Comment");
  case NOSTR_KIND_SEAL:
  case NOSTR_KIND_GIFT_WRAP:
    return _("Encrypted message");
  case NOSTR_KIND_DIRECT_MESSAGE:
  case NOSTR_KIND_FILE_MESSAGE:
    return _("Private message");
  case NOSTR_KIND_RELAY_LIST_METADATA:
    return _("Relay list");
  case NOSTR_KIND_CLIENT_AUTHENTICATION:
    return _("Relay authentication");
  case NOSTR_KIND_ZAP_REQUEST:
    return _("Zap request");
  case NOSTR_KIND_ZAP:
    return _("Zap receipt");
  case NOSTR_KIND_NOSTR_CONNECT:
    return _("Remote signer message");
  default: {
    /* Registry names (generated from nostr-kinds.h) before "Custom event". */
    for (guint i = 0; i < G_N_ELEMENTS(kind_catalog); i++)
      if (kind_catalog[i].kind == kind) return _(kind_catalog[i].name);
    return _("Custom event");
  }
  }
}
/* ---- declared permission sets (G18) ------------------------------------ */
/* Capabilities group an app's declared tokens into one human sentence; the
 * exact kinds stay available under Details. Order is display order. */
typedef enum { CAP_GROUPS, CAP_MESSAGING, CAP_AUTH, CAP_LISTS, CAP_DELETION, CAP_PUBLISHING,
               CAP_ZAPS, CAP_UPLOADS, CAP_READ, CAP_CUSTOM, CAP_COUNT } Capability;
static Capability capability_of(const gchar *type, guint32 kind) {
  if (strcmp(type, "sign_event")) {
    if (!strcmp(type, "get_public_key") || !strcmp(type, "get_relays")) return CAP_READ;
    if (!strcmp(type, "decrypt_zap_event")) return CAP_ZAPS;
    return CAP_MESSAGING;
  }
  if ((kind >= NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE && kind <= NOSTR_KIND_SIMPLE_GROUP_REPLY) ||
      (kind >= NOSTR_KIND_SIMPLE_GROUP_ADD_USER && kind <= 9030) || kind == NOSTR_KIND_SIMPLE_GROUP_LIST ||
      (kind >= NOSTR_KIND_SIMPLE_GROUP_METADATA && kind <= 39009)) return CAP_GROUPS;
  if (kind == NOSTR_KIND_PROFILE_METADATA || kind == NOSTR_KIND_FOLLOW_LIST ||
      (kind >= 10000 && kind <= 10099) || (kind >= 30000 && kind <= 30030)) return CAP_LISTS;
  switch (kind) {
  case NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE: case NOSTR_KIND_SEAL: case NOSTR_KIND_DIRECT_MESSAGE:
  case NOSTR_KIND_FILE_MESSAGE: case NOSTR_KIND_GIFT_WRAP: return CAP_MESSAGING;
  case NOSTR_KIND_CLIENT_AUTHENTICATION: return CAP_AUTH;
  case NOSTR_KIND_DELETION: return CAP_DELETION;
  case NOSTR_KIND_TEXT_NOTE: case NOSTR_KIND_REPOST: case NOSTR_KIND_GENERIC_REPOST: case NOSTR_KIND_REACTION:
  case NOSTR_KIND_COMMENT: case NOSTR_KIND_ARTICLE: case 1621: return CAP_PUBLISHING;
  case NOSTR_KIND_ZAP_REQUEST: return CAP_ZAPS;
  case 24242: return CAP_UPLOADS;
  default: return CAP_CUSTOM;
  }
}
static const gchar *capability_label(Capability c) {
  switch (c) {
  case CAP_GROUPS: return _("NIP-29 groups");
  case CAP_MESSAGING: return _("encrypted messages");
  case CAP_AUTH: return _("sign-in to any relay it chooses");
  case CAP_LISTS: return _("profile and relay lists");
  case CAP_DELETION: return _("deletions");
  case CAP_PUBLISHING: return _("public posts and reactions");
  case CAP_ZAPS: return _("zaps");
  case CAP_UPLOADS: return _("file uploads");
  case CAP_READ: return _("reading your public key and relays");
  case CAP_CUSTOM: case CAP_COUNT: break;
  }
  return _("custom kinds");
}
static const gchar *token_label(const gchar *type) {
  if (!strcmp(type, "nip44_encrypt")) return _("Encrypt messages (NIP-44)");
  if (!strcmp(type, "nip44_decrypt")) return _("Decrypt messages (NIP-44)");
  if (!strcmp(type, "nip04_encrypt")) return _("Encrypt messages (legacy NIP-04)");
  if (!strcmp(type, "nip04_decrypt")) return _("Decrypt messages (legacy NIP-04)");
  if (!strcmp(type, "get_public_key")) return _("Read your public key");
  if (!strcmp(type, "get_relays")) return _("Read your relay list");
  if (!strcmp(type, "decrypt_zap_event")) return _("Decrypt private zaps");
  return type;
}
/* One sentence fragment per capability, custom kinds listed by number. */
static gchar *declared_action_text(GVariant *declared) {
  gboolean present[CAP_COUNT] = {0};
  g_autoptr(GString) custom = g_string_new(NULL);
  for (gsize i = 0; declared && i < g_variant_n_children(declared); i++) {
    g_autoptr(GVariant) entry = g_variant_get_child_value(declared, i);
    const gchar *type = field(entry, "type"); guint32 kind = 0;
    g_variant_lookup(entry, "kind", "u", &kind);
    Capability c = capability_of(type, kind);
    present[c] = TRUE;
    if (c == CAP_CUSTOM) g_string_append_printf(custom, "%s%u", custom->len ? ", " : "", kind);
  }
  GString *text = g_string_new(NULL);
  for (Capability c = 0; c < CAP_COUNT; c++) {
    if (!present[c]) continue;
    if (text->len) g_string_append(text, ", ");
    if (c == CAP_CUSTOM) g_string_append_printf(text, _("custom kinds (%s)"), custom->str);
    else g_string_append(text, capability_label(c));
  }
  return g_string_free(text, FALSE);
}
/* Every declared token with its registry name and current standing. */
static gchar *declared_details(GVariant *declared, guint *requested, guint *granted, guint *denied) {
  GString *text = g_string_new(NULL);
  *requested = *granted = *denied = 0;
  for (gsize i = 0; declared && i < g_variant_n_children(declared); i++) {
    g_autoptr(GVariant) entry = g_variant_get_child_value(declared, i);
    const gchar *type = field(entry, "type"), *state = field(entry, "state"); guint32 kind = 0;
    gboolean has_kind = g_variant_lookup(entry, "kind", "u", &kind);
    if (!strcmp(state, "granted")) ++*granted; else if (!strcmp(state, "denied")) ++*denied; else ++*requested;
    if (text->len) g_string_append_c(text, '\n');
    if (has_kind) g_string_append_printf(text, "• %s (kind %u)", gnostr_approval_kind_name(kind), kind);
    else g_string_append_printf(text, "• %s", token_label(type));
    /* Any-relay AUTH is a broader delegation than one relay sign-in: name it. */
    if (has_kind && kind == NOSTR_KIND_CLIENT_AUTHENTICATION) g_string_append(text, _(" — any relay this app chooses"));
    if (!strcmp(state, "granted")) g_string_append(text, _(" — already allowed"));
    else if (!strcmp(state, "denied")) g_string_append(text, _(" — blocked; stays blocked"));
  }
  return g_string_free(text, FALSE);
}

static gchar *bounded(const char *text, glong max) {
  g_autofree gchar *valid = g_utf8_make_valid(text ? text : "", -1);
  glong n = g_utf8_strlen(valid, -1);
  if (n <= max)
    return g_steal_pointer(&valid);
  g_autofree gchar *head = g_utf8_substring(valid, 0, max);
  return g_strconcat(head, "…", NULL);
}
static void finish_now(GnostrApprovalDialog *self, gboolean allow, gboolean remember, guint64 ttl) {
  if (self->finished || (allow && !self->ready) ||
      !gtk_widget_get_sensitive(GTK_WIDGET(allow ? self->btn_approve : self->btn_deny)))
    return;
  self->finished = TRUE;
  GnostrApprovalCallback cb = self->callback;
  self->callback = NULL;
  if (cb)
    cb(allow, remember, self->identity, ttl, self->user_data);
  adw_dialog_close(ADW_DIALOG(self));
}
static void mode_finish(GnostrApprovalDialog *self, const gchar *mode);
static const gchar *selected_mode(GnostrApprovalDialog *self) {
  guint selected = adw_combo_row_get_selected(self->permission_choice);
  return selected < self->n_choices ? self->choices[selected] : "allow-once";
}
static void do_finish(GnostrApprovalDialog *self, gboolean allow) {
  if (self->finished || self->confirming || (allow && !self->ready) ||
      !gtk_widget_get_sensitive(GTK_WIDGET(allow ? self->btn_approve : self->btn_deny)))
    return;
  const gchar *mode = selected_mode(self);
  if (allow && !strcmp(mode, "flow")) {
    gboolean encrypt = gtk_check_button_get_active(self->chk_flow_encrypt);
    gboolean seals = gtk_check_button_get_active(self->chk_flow_seals);
    if (encrypt || seals) {
      self->finished = TRUE; self->callback = NULL;
      g_signal_emit(self, signals[APPROVE_FLOW], 0, encrypt, seals);
      adw_dialog_close(ADW_DIALOG(self));
      return;
    }
  }
  if (allow && (!strcmp(mode, "trust-routine") || !strcmp(mode, "activate-saved") ||
                !strcmp(mode, "discard-saved"))) {
    mode_finish(self, mode);
    return;
  }
  gboolean remember = allow && (!strcmp(mode, "allow") || !strcmp(mode, "allow-24h"));
  finish_now(self, allow, remember, remember && !strcmp(mode, "allow-24h") ? 86400 : 0);
}
static void approve_clicked(GtkButton *button, gpointer data) {
  (void)button;
  do_finish(data, TRUE);
}
static void deny_clicked(GtkButton *button, gpointer data) {
  (void)button;
  do_finish(data, FALSE);
}
static void not_now_clicked(GtkButton *button, gpointer data) {
  (void)button;
  GnostrApprovalDialog *self = data;
  if (self->finished || self->confirming)
    return;
  self->finished = TRUE;
  self->callback = NULL;
  g_signal_emit(self, signals[DEFERRED], 0);
  adw_dialog_close(ADW_DIALOG(self));
}
static void selection_toggled(GtkCheckButton *button, gpointer data) {
  (void)button;
  GnostrApprovalDialog *self = data;
  gboolean selected = FALSE;
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->batch_box)); child;
       child = gtk_widget_get_next_sibling(child))
    selected |= gtk_check_button_get_active(GTK_CHECK_BUTTON(child));
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve_selected),
                           selected && self->ready && !self->finished);
}
static void selected_clicked(GtkButton *button, gpointer data) {
  GnostrApprovalDialog *self = data;
  if (!self->ready || self->finished || self->confirming ||
      !gtk_widget_get_sensitive(GTK_WIDGET(button)))
    return;
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->batch_box)); child;
       child = gtk_widget_get_next_sibling(child))
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(child)))
      g_ptr_array_add(ids, g_strdup(g_object_get_data(G_OBJECT(child), "request-id")));
  if (!ids->len)
    return;
  g_ptr_array_add(ids, NULL);
  self->finished = TRUE;
  self->callback = NULL;
  g_signal_emit(self, signals[APPROVE_SELECTED], 0, ids->pdata);
  adw_dialog_close(ADW_DIALOG(self));
}
static void previous_clicked(GtkButton *button, gpointer data) {
  (void)button;
  g_signal_emit(data, signals[STEP], 0, -1);
}
static void next_clicked(GtkButton *button, gpointer data) {
  (void)button;
  g_signal_emit(data, signals[STEP], 0, 1);
}
static void permission_changed(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)object; (void)pspec;
  GnostrApprovalDialog *self = data;
  const gchar *mode = selected_mode(self);
  gboolean flow = !strcmp(mode, "flow");
  gtk_widget_set_visible(GTK_WIDGET(self->group_flow), flow);
  if (!flow) {
    gtk_check_button_set_active(self->chk_flow_encrypt, FALSE);
    gtk_check_button_set_active(self->chk_flow_seals, FALSE);
  }
  const gchar *commit = _("_Allow Once");
  g_autofree gchar *scope = NULL;
  g_autofree gchar *at_relay = self->relay && *self->relay
      ? g_strdup_printf(_(" at %s"), self->relay) : g_strdup("");
  const gchar *identity = *field(self->info, "identity_label")
      ? field(self->info, "identity_label") : _("Identity");
  if (!strcmp(mode, "allow-24h")) {
    commit = _("_Allow for 24 Hours");
    scope = g_strdup_printf(_("%s may %s as %s%s for 24 hours"),
        self->app_name, self->reusable_text, identity, at_relay);
  } else if (!strcmp(mode, "allow")) {
    commit = _("_Always Allow");
    scope = g_strdup_printf(_("%s may %s as %s%s until revoked"),
        self->app_name, self->reusable_text, identity, at_relay);
  } else if (!strcmp(mode, "trust-routine")) {
    commit = _("Trust _App");
    scope = g_strdup(_("Trust this app for routine operations. Changes this app's access preset, not only this request or identity. Includes publishing, messaging, relay sign-in, profile/contact changes, deletion and zap requests. Custom kinds and other ask-by-default kinds still need their own grant. Blocks remain in effect"));
  } else if (!strcmp(mode, "activate-saved")) {
    commit = _("_Activate Permission");
    scope = g_strdup_printf(_("Activate the saved permission for %s%s. Original expiry: %s"),
        identity, at_relay, self->saved_expiry ? self->saved_expiry : _("Loading…"));
  } else if (!strcmp(mode, "discard-saved")) {
    commit = _("Discard _Permission");
    scope = g_strdup(_("Delete the inactive saved permission. This request stays waiting; no operation is approved"));
  } else if (flow) {
    commit = _("_Allow for This Message");
    scope = g_strdup(self->flow_scope);
  } else {
    scope = g_strdup_printf(_("Only this request from %s, to %s as %s%s. No permission is saved"),
        self->app_name ? self->app_name : _("this app"),
        self->action_text ? self->action_text : _("perform this operation"), identity, at_relay);
  }
  if (!strcmp(field(self->info, "operation"), "connect")) {
    commit = _("Pair _Application");
    g_free(scope);
    gboolean grants = FALSE;
    g_autoptr(GVariant) summary = g_variant_lookup_value(self->info, "event_summary", G_VARIANT_TYPE_VARDICT);
    if (summary) g_variant_lookup(summary, "granted_on_pairing", "b", &grants);
    scope = g_strdup(grants
        ? _("Pair this application with the identity and relays shown above. Pairing is saved and grants the permissions it requested (see Details) until revoked")
        : _("Pair this application with the identity and relays shown above. Pairing is saved; no signing permission is granted"));
  }
  if (!strcmp(field(self->info, "operation"), "permissions")) {
    /* A declared set is one decision, always until revoked; no once/24 h. */
    g_free(scope);
    if (!strcmp(mode, "trust-routine")) {
      commit = _("Trust _App");
      scope = g_strdup_printf(_("Trust %s for routine operations and allow everything listed above as %s, including custom kinds, until revoked in its Permissions page"), self->app_name, identity);
    } else {
      commit = _("_Allow");
      /* The identity is the Signing As row above; this line states duration. */
      scope = g_strdup_printf(_("Until you revoke it in %s's Permissions page"), self->app_name);
    }
  }
  if (!strcmp(field(self->info, "operation"), "self_test")) {
    g_free(scope);
    scope = g_strdup(_("This identity test only. Nothing is published and no app permission is stored"));
  }
  if (!strcmp(field(self->info, "transport"), "MANAGEMENT") && !strcmp(field(self->info, "purpose"), "profile-management")) {
    guint32 kind = 0; g_variant_lookup(self->info, "event_kind", "u", &kind);
    commit = kind == NOSTR_KIND_PROFILE_METADATA ? _("_Publish Profile") : kind == NOSTR_KIND_BLOBS ? _("_Authorize Upload") : _("_Sign In");
    g_free(scope); scope = g_strdup_printf(_("This exact request only, as %s. No permission is saved. Destinations:\n%s"), identity, field(self->info, "reason"));
  }
  gtk_button_set_label(self->btn_approve, commit);
  if (!strcmp(mode, "discard-saved"))
    gtk_widget_remove_css_class(GTK_WIDGET(self->btn_approve), "suggested-action");
  else gtk_widget_add_css_class(GTK_WIDGET(self->btn_approve), "suggested-action");
  gtk_label_set_text(self->scope_summary, scope);
}
/* Popup items wrap so every choice is readable; the closed selection stays
 * one line (the footer caption spells the selected scope out in full). */
static void choice_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  (void)factory;
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  if (data) {
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 36);
  } else {
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 20);
  }
  gtk_list_item_set_child(item, label);
}
static void choice_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  (void)factory; (void)data;
  gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
      gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item))));
}
static void mode_finish(GnostrApprovalDialog *self, const gchar *mode) {
  if (self->finished || self->confirming || !self->ready) return;
  if (!strcmp(mode, "discard-saved")) {
    /* The daemon deletes authority, not the pending request. Keep this view
     * read-only until the controller refetches its authoritative snapshot. */
    self->ready = FALSE;
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->permission_choice), FALSE);
    g_signal_emit(self, signals[DECISION_MODE], 0, mode);
    return;
  }
  self->finished = TRUE; self->callback = NULL;
  g_signal_emit(self, signals[DECISION_MODE], 0, mode);
  adw_dialog_close(ADW_DIALOG(self));
}
static void block_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
  GnostrApprovalDialog *self = data;
  const gchar *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  self->confirming = FALSE;
  if (!strcmp(response, "block")) mode_finish(self, "block-app");
  g_object_unref(self);
}
static void block_activated(GSimpleAction *action, GVariant *parameter, gpointer data) {
  (void)parameter;
  GnostrApprovalDialog *self = data;
  if (!g_action_get_enabled(G_ACTION(action))) return;
  const gchar *mode = g_action_get_name(G_ACTION(action));
  if (!strcmp(mode, "block-app")) {
    if (self->finished || self->confirming || !self->ready) return;
    self->confirming = TRUE;
    AdwDialog *alert = adw_alert_dialog_new(_("Block App?"), _("Deny all operations from this app until you remove the block in its Permissions page."));
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", _("Cancel"), "block", _("Block App"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "block", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(alert), GTK_WIDGET(self), NULL, block_confirmed, g_object_ref(self));
  } else mode_finish(self, mode);
}
static void closed(AdwDialog *dialog) {
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(dialog);
  GnostrApprovalCallback cb = self->callback;
  self->callback = NULL;
  self->finished = TRUE;
  if (cb)
    cb(FALSE, FALSE, self->identity, 0, self->user_data);
  if (ADW_DIALOG_CLASS(gnostr_approval_dialog_parent_class)->closed)
    ADW_DIALOG_CLASS(gnostr_approval_dialog_parent_class)->closed(dialog);
}
static void finalize(GObject *object) {
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(object);
  g_free(self->identity);
  g_free(self->client_pubkey);
  g_free(self->profile_details);
  g_clear_pointer(&self->info, g_variant_unref);
  g_free(self->app_name); g_free(self->action_text); g_free(self->reusable_text); g_free(self->relay);
  g_free(self->saved_expiry); g_free(self->flow_scope);
  g_clear_object(&self->block_operation); g_clear_object(&self->block_app);
  g_clear_object(&self->actions);
  G_OBJECT_CLASS(gnostr_approval_dialog_parent_class)->finalize(object);
}
static gboolean grab_focus(GtkWidget *widget) {
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(widget);
  /* Presentation can clear the focus saved before rooting. Apply the safe
   * initial target when AdwDialog actually focuses its mapped contents. */
  if (self->initial_focus && gtk_widget_get_mapped(widget)) {
    self->initial_focus = FALSE;
    if (gtk_widget_grab_focus(GTK_WIDGET(self->btn_deny))) return TRUE;
  }
  return GTK_WIDGET_CLASS(gnostr_approval_dialog_parent_class)->grab_focus(widget);
}
static void gnostr_approval_dialog_class_init(GnostrApprovalDialogClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = finalize;
  ADW_DIALOG_CLASS(klass)->closed = closed;
  GtkWidgetClass *w = GTK_WIDGET_CLASS(klass);
  w->grab_focus = grab_focus;
  gtk_widget_class_set_template_from_resource(w, "/org/gnostr/signer/ui/approval-dialog.ui");
#define BIND(name) gtk_widget_class_bind_template_child(w, GnostrApprovalDialog, name)
  BIND(app_avatar); BIND(header_title); BIND(label_verified);
  BIND(row_action); BIND(row_identity); BIND(row_reason); BIND(row_relay);
  BIND(row_impact); BIND(row_expiry); BIND(scope_summary); BIND(saved_status);
  BIND(label_details); BIND(label_timestamp);
  BIND(batch_expander); BIND(batch_box);
  BIND(group_saved); BIND(group_flow);
  BIND(chk_flow_encrypt); BIND(chk_flow_seals);
  BIND(btn_approve_selected); BIND(content_preview);
  BIND(details_expander); BIND(stepper);
  BIND(permission_choice); BIND(more_menu); BIND(loading);
  BIND(btn_deny);
  BIND(btn_approve);
  BIND(btn_not_now);
  BIND(btn_previous);
  BIND(btn_next);
#undef BIND
  signals[DECISION_MODE] = g_signal_new("decision-mode", G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[APPROVE_FLOW] = g_signal_new("approve-flow", G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
  signals[APPROVE_SELECTED] =
      g_signal_new("approve-selected", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                   NULL, G_TYPE_NONE, 1, G_TYPE_STRV);
  signals[DEFERRED] = g_signal_new("deferred", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                   NULL, NULL, G_TYPE_NONE, 0);
  signals[STEP] = g_signal_new("step", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                               NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[CHANGE_TRUST] =
      g_signal_new("change-trust-level", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                   NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}
static void gnostr_approval_dialog_init(GnostrApprovalDialog *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  /* Scope-specific commit labels must fit the 360 px dialog, not raise its
   * minimum width (notably “Allow for 24 Hours” and “Allow for This Message”). */
  GtkButton *commits[] = {self->btn_deny, self->btn_approve};
  for (guint i = 0; i < G_N_ELEMENTS(commits); i++) {
    GtkLabel *label = GTK_LABEL(gtk_button_get_child(commits[i]));
    gtk_label_set_wrap(label, TRUE);
    gtk_label_set_wrap_mode(label, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_justify(label, GTK_JUSTIFY_CENTER);
  }
  self->ready = TRUE;
  self->initial_focus = TRUE;
  self->actions = g_simple_action_group_new();
  self->block_operation = g_simple_action_new("block-operation", NULL);
  self->block_app = g_simple_action_new("block-app", NULL);
  g_signal_connect(self->block_operation, "activate", G_CALLBACK(block_activated), self);
  g_signal_connect(self->block_app, "activate", G_CALLBACK(block_activated), self);
  g_action_map_add_action(G_ACTION_MAP(self->actions), G_ACTION(self->block_operation));
  g_action_map_add_action(G_ACTION_MAP(self->actions), G_ACTION(self->block_app));
  gtk_widget_insert_action_group(GTK_WIDGET(self), "review", G_ACTION_GROUP(self->actions));
  g_autoptr(GtkListItemFactory) factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(choice_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(choice_bind), NULL);
  g_autoptr(GtkListItemFactory) list_factory = gtk_signal_list_item_factory_new();
  g_signal_connect(list_factory, "setup", G_CALLBACK(choice_setup), GINT_TO_POINTER(1));
  g_signal_connect(list_factory, "bind", G_CALLBACK(choice_bind), NULL);
  adw_combo_row_set_factory(self->permission_choice, factory);
  adw_combo_row_set_list_factory(self->permission_choice, list_factory);
  g_signal_connect(self->permission_choice, "notify::selected", G_CALLBACK(permission_changed), self);
  self->choices[0] = "allow-once"; self->n_choices = 1;
  const gchar *initial[] = {_("Allow once"), NULL};
  g_autoptr(GtkStringList) model = gtk_string_list_new(initial);
  adw_combo_row_set_model(self->permission_choice, G_LIST_MODEL(model));
#define CONNECT(widget, fn) g_signal_connect(self->widget, "clicked", G_CALLBACK(fn), self)
  CONNECT(btn_approve_selected, selected_clicked);
  CONNECT(btn_approve, approve_clicked);
  CONNECT(btn_deny, deny_clicked);
  CONNECT(btn_not_now, not_now_clicked);
  CONNECT(btn_previous, previous_clicked);
  CONNECT(btn_next, next_clicked);

#undef CONNECT
  adw_dialog_set_focus(ADW_DIALOG(self), GTK_WIDGET(self->btn_deny));
  /* No default approve widget: Enter on a dropdown or Details is not consent. */
}
GnostrApprovalDialog *gnostr_approval_dialog_new(void) {
  return g_object_new(GNOSTR_TYPE_APPROVAL_DIALOG, NULL);
}
void gnostr_approval_dialog_set_callback(GnostrApprovalDialog *self,
                                         GnostrApprovalCallback callback, gpointer data) {
  self->callback = callback;
  self->user_data = data;
}
void gnostr_approval_dialog_set_identity(GnostrApprovalDialog *self, const char *identity) {
  g_free(self->identity);
  self->identity = g_strdup(identity);
  adw_action_row_set_subtitle(self->row_identity,
      identity && *identity ? identity : _("No identity (identity-independent operation)"));
}
void gnostr_approval_dialog_set_app_name(GnostrApprovalDialog *self, const char *name) {
  g_free(self->app_name);
  self->app_name = g_strdup(name ? name : _("Unverified app"));
  adw_avatar_set_text(self->app_avatar, self->app_name);
}
void gnostr_approval_dialog_set_event_type(GnostrApprovalDialog *self, int kind) {
  g_autofree gchar *label =
      g_strdup_printf("%s (kind %d)", gnostr_approval_kind_name((guint32)kind), kind);
  adw_action_row_set_subtitle(self->row_action, label);
}
void gnostr_approval_dialog_set_timestamp(GnostrApprovalDialog *self, guint64 timestamp) {
  g_autofree gchar *text = g_strdup_printf(_("Request time: %" G_GUINT64_FORMAT), timestamp);
  gtk_label_set_text(self->label_timestamp, text);
}
void gnostr_approval_dialog_set_content(GnostrApprovalDialog *self, const char *content) {
  g_autofree gchar *preview = bounded(content, 280);
  gtk_label_set_text(self->content_preview, *preview ? preview : _("No content"));
}
/* Compatibility callers cannot change the identity pinned by the daemon.
 * Account metadata is never an authorization source or a first-key fallback. */
void gnostr_approval_dialog_set_accounts(GnostrApprovalDialog *self, AccountsStore *store,
                                         const char *selected) {
  (void)store;
  gnostr_approval_dialog_set_identity(self, selected);
}
void gnostr_approval_dialog_set_client_pubkey(GnostrApprovalDialog *self, const char *key) {
  g_free(self->client_pubkey);
  self->client_pubkey = g_strdup(key);
}
void gnostr_approval_dialog_set_request(GnostrApprovalDialog *self, const char *kind,
                                        const char *app, const char *claimed, gboolean verified,
                                        const char *preview) {
  gnostr_approval_dialog_set_app_name(self, app);
  gtk_label_set_text(self->header_title, self->app_name);
  /* This compatibility entrypoint has no transport evidence. */
  gtk_label_set_text(self->label_verified, verified ? _("Verified caller") : _("Unverified app"));
  adw_action_row_set_subtitle(self->row_action, kind && !strcmp(kind, "self_test") ? _("Identity self-test") : kind ? kind : _("Unknown request"));
  g_autofree gchar *details =
      g_strdup_printf(_("%s%s%s"), verified ? _("Verified caller") : _("Unverified app"),
                      claimed && *claimed ? _("\nClaimed name (unverified): ") : "",
                      claimed && *claimed ? claimed : "");
  gtk_label_set_text(self->label_details, details);
  gnostr_approval_dialog_set_content(self, preview);
  gtk_button_set_label(self->btn_approve, _("_Allow Once"));
}
void gnostr_approval_dialog_set_batch(GnostrApprovalDialog *self, GVariant *rows) {
  while (gtk_widget_get_first_child(GTK_WIDGET(self->batch_box)))
    gtk_box_remove(self->batch_box, gtk_widget_get_first_child(GTK_WIDGET(self->batch_box)));
  gtk_widget_set_visible(GTK_WIDGET(self->batch_expander), rows && g_variant_n_children(rows) > 1);
  if (!rows)
    return;
  for (gsize i = 0; i < g_variant_n_children(rows); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(rows, i);
    g_autoptr(GVariant) summary =
        g_variant_lookup_value(row, "event_summary", G_VARIANT_TYPE_VARDICT);
    guint32 kind;
    g_autofree gchar *operation =
        g_variant_lookup(row, "event_kind", "u", &kind)
            ? g_strdup_printf(_("%s · %s (kind %u)"), field(row, "operation"),
                              gnostr_approval_kind_name(kind), kind)
            : g_strdup(field(row, "operation"));
    g_autofree gchar *label =
        g_strdup_printf(_("%s\nIdentity: %s\nPeer: %s"), operation, field(row, "identity"),
                        *field(summary, "peer")             ? field(summary, "peer")
                        : *field(summary, "recipient_peer") ? field(summary, "recipient_peer")
                                                            : _("None"));
    GtkWidget *check = gtk_check_button_new();
    GtkWidget *text = gtk_label_new(label);
    gtk_check_button_set_child(GTK_CHECK_BUTTON(check), text);
    gtk_accessible_update_property(GTK_ACCESSIBLE(check), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(text), PANGO_WRAP_WORD_CHAR);
    g_object_set_data_full(G_OBJECT(check), "request-id", g_strdup(field(row, "request_id")),
                           g_free);
    g_signal_connect(check, "toggled", G_CALLBACK(selection_toggled), self);
    gtk_box_append(self->batch_box, check);
  }
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve_selected), FALSE);
}
gboolean gnostr_approval_dialog_has_batch_request(GnostrApprovalDialog *self, const gchar *id) {
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->batch_box)); child;
       child = gtk_widget_get_next_sibling(child))
    if (!g_strcmp0(id, g_object_get_data(G_OBJECT(child), "request-id"))) return TRUE;
  return FALSE;
}
void gnostr_approval_dialog_set_trust_level(GnostrApprovalDialog *self, const char *tier) {
  (void)self; (void)tier; /* Eligibility is supplied by the daemon, not the preset label. */
}
void gnostr_approval_dialog_set_position(GnostrApprovalDialog *self, guint position, guint count) {
  self->position = position;
  self->count = count;
  g_autofree gchar *label = g_strdup_printf("%u of %u", position, count);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->stepper), label);
  gtk_widget_set_visible(GTK_WIDGET(self->stepper), count > 1);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_previous), position > 1);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next), position < count);
}
static gboolean profile_management(GVariant *info) {
  return !strcmp(field(info, "operation"), "sign_event") && !strcmp(field(info, "transport"), "MANAGEMENT") && !strcmp(field(info, "purpose"), "profile-management");
}
static JsonNode *management_event(GVariant *info) {
  g_autoptr(GVariant) summary = g_variant_lookup_value(info, "event_summary", G_VARIANT_TYPE_VARDICT);
  const char *raw = field(summary, "management_event_json"); guint32 kind = G_MAXUINT;
  g_variant_lookup(info, "event_kind", "u", &kind);
  if (!*raw || strlen(raw) > 32768 || !*field(info, "reason")) return NULL;
  NostrEvent *event = nostr_event_new(); guint8 pk[32] = {0}; char hex[65];
  gboolean valid = !nostr_nip19_decode_npub(field(info, "identity"), pk) && !nostr_event_deserialize(event, raw);
  for (guint i = 0; i < 32; i++) g_snprintf(hex + 2*i, 3, "%02x", pk[i]);
  valid = valid && (guint)nostr_event_get_kind(event) == kind && !g_strcmp0(hex, nostr_event_get_pubkey(event));
  if (valid && kind == NOSTR_KIND_PROFILE_METADATA) {
    g_autoptr(JsonNode) content = json_from_string(nostr_event_get_content(event), NULL);
    valid = content && JSON_NODE_HOLDS_OBJECT(content) && !nostr_tags_size(nostr_event_get_tags(event));
  }
  nostr_event_free(event);
  return valid ? json_from_string(raw, NULL) : NULL;
}
static char *management_preview(JsonNode *event, guint kind) {
  JsonObject *object = json_node_get_object(event);
  if (kind == NOSTR_KIND_PROFILE_METADATA) {
    g_autoptr(JsonNode) content = json_from_string(json_object_get_string_member(object, "content"), NULL);
    GString *text = g_string_new(_("Complete public profile fields:\n"));
    GList *members = json_object_get_members(json_node_get_object(content));
    for (GList *m = members; m; m = m->next) {
      g_autofree char *value = json_to_string(json_object_get_member(json_node_get_object(content), m->data), FALSE);
      g_string_append_printf(text, "%s: %s\n", (char *)m->data, value);
    }
    g_list_free(members); return g_string_free(text, FALSE);
  }
  GString *text = g_string_new(kind == NOSTR_KIND_BLOBS ? _("Public image upload authorization\n") : _("Relay sign-in authorization\n"));
  JsonArray *tags = json_object_get_array_member(object, "tags"); gboolean server_scoped = FALSE;
  for (guint i = 0; tags && i < json_array_get_length(tags); i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (!tag || json_array_get_length(tag) < 2) continue;
    const char *name = json_array_get_string_element(tag, 0), *value = json_array_get_string_element(tag, 1);
    if (!strcmp(name, "server")) server_scoped = TRUE;
    const char *label = !strcmp(name, "t") ? _("Action") : !strcmp(name, "x") ? _("Blob SHA-256") : !strcmp(name, "expiration") ? _("Expiry (Unix seconds)") : !strcmp(name, "relay") ? _("Relay") : !strcmp(name, "challenge") ? _("Challenge") : !strcmp(name, "server") ? _("Server restriction") : name;
    g_string_append_printf(text, "%s: %s\n", label, value);
    if (!strcmp(name, "expiration")) {
      g_autoptr(GDateTime) time = g_date_time_new_from_unix_utc(g_ascii_strtoll(value, NULL, 10));
      g_autofree char *expiry = time ? g_date_time_format_iso8601(time) : NULL;
      if (expiry) g_string_append_printf(text, _("Expires (UTC): %s\n"), expiry);
    }
  }
  if (kind == NOSTR_KIND_BLOBS && !server_scoped) g_string_append(text, _("No server restriction in this token: it authorizes this exact blob hash at any Blossom server until expiry. Grotto will try only the listed servers, in order."));
  return g_string_free(text, FALSE);
}
gboolean gnostr_approval_info_can_approve(GVariant *info) {
  const char *op = field(info, "operation");
  if (profile_management(info)) { g_autoptr(JsonNode) event = management_event(info); if (!event) return FALSE; }
  if (!strcmp(op, "permissions")) {
    g_autoptr(GVariant) summary = g_variant_lookup_value(info, "event_summary", G_VARIANT_TYPE_VARDICT);
    g_autoptr(GVariant) declared = summary ? g_variant_lookup_value(summary, "declared_permissions", G_VARIANT_TYPE("aa{sv}")) : NULL;
    return declared && g_variant_n_children(declared) > 0 && *field(info, "identity");
  }
  if (!strcmp(op, "connect")) {
    const gchar *principal = field(info, "principal");
    if (!*field(info, "identity") || !g_str_has_prefix(principal, "nip46:") || strlen(principal) != 70) return FALSE;
    for (guint i = 6; i < 70; i++) if (!g_ascii_isxdigit(principal[i])) return FALSE;
    g_autoptr(GVariant) summary = g_variant_lookup_value(info, "event_summary", G_VARIANT_TYPE_VARDICT);
    g_auto(GStrv) relays = NULL;
    if (!summary || !g_variant_lookup(summary, "relays", "^as", &relays) || !relays[0] || g_strv_length(relays) > 64) return FALSE;
    for (guint i = 0; relays[i]; i++) if (!*relays[i] || !g_utf8_validate(relays[i], -1, NULL)) return FALSE;
  }
  if (!strcmp(op, "sign_event")) {
    guint32 kind, count;
    if (!g_variant_lookup(info, "event_kind", "u", &kind) || kind > G_MAXUINT16 ||
        !*field(info, "identity"))
      return FALSE;
    g_autoptr(GVariant) summary =
        g_variant_lookup_value(info, "event_summary", G_VARIANT_TYPE_VARDICT);
    if (kind == NOSTR_KIND_DELETION)
      return summary && g_variant_lookup(summary, "delete_target_count", "u", &count);
    if (kind == NOSTR_KIND_CLIENT_AUTHENTICATION)
      return *field(summary, "auth_relay") != '\0';
    if (kind == NOSTR_KIND_REPOST || kind == NOSTR_KIND_GENERIC_REPOST ||
        kind == NOSTR_KIND_REACTION) {
      if (!*field(summary, "target_event_id"))
        return FALSE;
      if (kind == NOSTR_KIND_REACTION) {
        g_autoptr(GVariant) value =
            summary ? g_variant_lookup_value(summary, "reaction_value", G_VARIANT_TYPE_STRING)
                    : NULL;
        return value != NULL;
      }
      return TRUE;
    }
  }
  return TRUE;
}
static gchar *caller_description(GVariant *info, const gchar *name) {
  const gchar *transport = field(info, "transport");
  const gchar *kind = field(info, "principal_kind");
  gboolean verified = FALSE;
  g_variant_lookup(info, "verified", "b", &verified);
  /* Transport-key authentication is not local-executable or app-name proof.
   * The principal guard also handles contradictory/version-skewed metadata. */
  if (!strcmp(transport, "NIP46") || !strcmp(kind, "nip46") ||
      g_str_has_prefix(field(info, "principal"), "nip46:"))
    return g_strdup(verified
        ? _("Remote NIP-46 client · verified transport key; app name unverified")
        : _("Remote NIP-46 client · unverified"));
  if (!strcmp(transport, "MANAGEMENT") && !strcmp(field(info, "purpose"), "profile-management"))
    return g_strdup(_("Grotto profile management"));
  if (!strcmp(transport, "MANAGEMENT") && !strcmp(field(info, "operation"), "self_test"))
    return g_strdup(_("Grotto identity test"));
  if (!verified || !strcmp(kind, "claimed") ||
      g_str_has_prefix(field(info, "principal"), "claimed:"))
    return g_strdup_printf(_("Unverified: claims to be “%s”"),
        *field(info, "claimed_app_id") ? field(info, "claimed_app_id") : name);
  gboolean local = !strcmp(transport, "DBUS") || !strcmp(transport, "NIP5F");
  if (local && (!strcmp(kind, "website") || *field(info, "via")))
    return g_strdup_printf(_("Browser origin via bridge · %s"),
        *field(info, "app_id") ? field(info, "app_id") : _("Origin not supplied"));
  return g_strdup(local ? _("Verified local app") : _("Verified caller · transport not identified"));
}
void gnostr_approval_dialog_set_client_label(GnostrApprovalDialog *self, const char *label) {
  if (!self->info) return;
  g_autofree gchar *label_copy = g_strdup(label);
  label = label_copy;
  GVariantDict display; g_variant_dict_init(&display, self->info);
  g_variant_dict_insert(&display, "client_label", "s", label ? label : "");
  GVariant *updated = g_variant_ref_sink(g_variant_dict_end(&display));
  g_clear_pointer(&self->info, g_variant_unref); self->info = updated;
  g_autofree gchar *caller = caller_description(self->info, self->app_name);
  g_autofree gchar *secondary = label && *label ? g_strdup_printf(_("%s · called “%s”"), caller, label) : NULL;
  gtk_label_set_text(self->label_verified, secondary ? secondary : caller);
}
void gnostr_approval_dialog_set_info(GnostrApprovalDialog *self, GVariant *info, const char *name) {
  g_clear_pointer(&self->info, g_variant_unref);
  self->info = g_variant_ref(info);
  const char *op = field(info, "operation");
  gboolean self_test = !strcmp(op, "self_test") &&
      !strcmp(field(info, "transport"), "MANAGEMENT");
  gboolean declared = !strcmp(op, "permissions");
  if (self_test)
    name = "Grotto";
  gboolean verified = FALSE, rememberable = FALSE;
  g_variant_lookup(info, "verified", "b", &verified);
  g_variant_lookup(info, "rememberable", "b", &rememberable);
  gnostr_approval_dialog_set_request(self, op, name, field(info, "claimed_app_id"), verified,
                                     field(info, "preview"));
  gnostr_approval_dialog_set_identity(self, field(info, "identity"));
  const gchar *identity_label = *field(info, "identity_label") ? field(info, "identity_label") : _("Identity");
  g_autofree gchar *short_key = strlen(self->identity) > 24
      ? g_strdup_printf("%.12s…%s", self->identity, self->identity + strlen(self->identity) - 8)
      : g_strdup(self->identity);
  g_autofree gchar *identity_display = g_strdup_printf("%s · %s", identity_label, short_key);
  if (*self->identity) adw_action_row_set_subtitle(self->row_identity, identity_display);
  gnostr_approval_dialog_set_client_label(self, field(info, "client_label"));
  guint32 remaining = 0;
  g_variant_lookup(info, "remaining_seconds", "u", &remaining);
  g_autofree gchar *expiry = g_strdup_printf(_("Expires in at most %u seconds"), remaining);
  adw_action_row_set_subtitle(self->row_expiry, expiry);
  self->ready = remaining > 0 && !strcmp(field(info, "state"), "PENDING");
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve), self->ready);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_deny), self->ready);
  gboolean routine_trust = FALSE;
  g_variant_lookup(info, "routine_trust_available", "b", &routine_trust);
  /* Do not offer persistence to the empty UID-only claim, even on an old daemon. */
  rememberable = rememberable && strcmp(field(info, "principal"), "claimed:") &&
      strcmp(op, "connect") && !self_test;
  gtk_widget_set_visible(GTK_WIDGET(self->more_menu), rememberable);
  gboolean review = FALSE, drift = FALSE;
  g_variant_lookup(info, "policy_review", "b", &review);
  g_variant_lookup(info, "identity_drift", "b", &drift);
  gtk_widget_set_visible(GTK_WIDGET(self->group_saved), review || drift);
  guint size = 0;
  g_variant_lookup(info, "flow_size", "u", &size);
  guint flow_kind = 0;
  g_variant_lookup(info, "event_kind", "u", &flow_kind);
  gboolean flow = rememberable && self->ready && size > 0 && size <= 1024 &&
      *field(info, "flow_id") && !strcmp(field(info, "purpose"), "nip17-room-send") &&
      (!strcmp(op, "nip44_encrypt") || (!strcmp(op, "sign_event") && flow_kind == NOSTR_KIND_SEAL));
  guint flow_left = 0; g_variant_lookup(info, "flow_remaining_seconds", "u", &flow_left);
  flow = flow && flow_left > 0;
  flow = flow && !review;
  gtk_widget_set_visible(GTK_WIDGET(self->group_flow), FALSE);
  gtk_check_button_set_active(self->chk_flow_encrypt, FALSE);
  gtk_check_button_set_active(self->chk_flow_seals, FALSE);
  const char *reason = field(info, "reason");
  g_autofree gchar *reason_text = bounded(reason, 160);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->row_reason),
      !strcmp(field(info, "reason_source"), "caller") ? _("Reason Given by App") : _("Signer Explanation"));
  adw_action_row_set_subtitle(self->row_reason, reason_text);
  gtk_widget_set_visible(GTK_WIDGET(self->row_reason), *reason != '\0');
  guint32 kind = 0;
  gboolean has_kind = g_variant_lookup(info, "event_kind", "u", &kind);
  g_autoptr(GVariant) declared_summary = g_variant_lookup_value(info, "event_summary", G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariant) declared_set = declared && declared_summary
      ? g_variant_lookup_value(declared_summary, "declared_permissions", G_VARIANT_TYPE("aa{sv}")) : NULL;
  g_autofree gchar *declared_text = declared ? declared_action_text(declared_set) : NULL;
  gboolean extends = FALSE;
  g_variant_lookup(info, "declared_set", "b", &extends);
  if (has_kind && kind <= G_MAXUINT16)
    gnostr_approval_dialog_set_event_type(self, (int)kind);
  else if (!strcmp(op, "sign_event")) {
    self->ready = FALSE;
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve), FALSE);
    adw_action_row_set_subtitle(self->row_action, _("Invalid or missing event kind"));
  }

  g_autoptr(GVariant) summary =
      g_variant_lookup_value(info, "event_summary", G_VARIANT_TYPE_VARDICT);
  const char *peer = field(summary, "peer");
  g_free(self->relay);
  self->relay = g_strdup(kind == NOSTR_KIND_CLIENT_AUTHENTICATION ? field(summary, "auth_relay") : "");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->row_relay), _("Relay"));
  adw_action_row_set_subtitle(self->row_relay, self->relay);
  gtk_widget_set_visible(GTK_WIDGET(self->row_relay), *self->relay != '\0');
  g_free(self->action_text);
  self->action_text = declared ? g_strdup(declared_text)
      : has_kind && !strcmp(op, "sign_event")
      ? kind == NOSTR_KIND_TEXT_NOTE ? g_strdup(_("sign a note (kind 1)"))
      : kind == NOSTR_KIND_FOLLOW_LIST ? g_strdup(_("sign a contact list (kind 3)"))
      : kind == NOSTR_KIND_CLIENT_AUTHENTICATION ? g_strdup(_("sign in to a relay"))
      : g_strdup_printf(_("sign an event: %s (kind %u)"), gnostr_approval_kind_name(kind), kind)
      : g_str_has_suffix(op, "encrypt") ? g_strdup(_("encrypt a message"))
      : g_str_has_suffix(op, "decrypt") ? g_strdup(_("decrypt a message"))
      : g_strdup(!strcmp(op, "connect") ? _("connect") : self_test ? _("test this identity")
          : !strcmp(op, "get_public_key") ? _("read the public key")
          : !strcmp(op, "get_relays") ? _("read relay settings")
          : !strcmp(op, "ping") ? _("check the signer connection") : op);
  g_free(self->reusable_text);
  self->reusable_text = !strcmp(op, "sign_event") && kind == NOSTR_KIND_TEXT_NOTE
      ? g_strdup(_("sign notes (kind 1)"))
      : !strcmp(op, "sign_event") && kind == NOSTR_KIND_FOLLOW_LIST ? g_strdup(_("sign contact lists (kind 3)"))
      : g_str_has_suffix(op, "encrypt") ? g_strdup(_("encrypt messages (all peers)"))
      : g_str_has_suffix(op, "decrypt") ? g_strdup(_("decrypt messages (all peers)"))
      : g_strdup(self->action_text);
  g_autofree gchar *headline = NULL;
  if (self_test) headline = g_strdup_printf(_("Test %s's identity"), *field(info, "identity_label") ? field(info, "identity_label") : field(info, "identity"));
  else if (declared) headline = g_strdup_printf(_("%s wants to use your identity"), name);
  else if (extends && rememberable && !review && !flow && strcmp(op, "connect") && strcmp(field(info, "transport"), "MANAGEMENT"))
    headline = g_strdup_printf(_("%s wants an additional permission"), name);
  else if (!strcmp(field(info, "transport"), "MANAGEMENT") &&
      !strcmp(field(info, "purpose"), "profile-management") && has_kind && kind == NOSTR_KIND_PROFILE_METADATA)
    headline = g_strdup_printf(_("Publish profile for %s"), *field(info, "identity_label") ? field(info, "identity_label") : field(info, "identity"));
  else if (flow) headline = g_strdup_printf(_("%s wants to prepare encrypted messages"), name);
  else if (!strcmp(op, "connect")) headline = g_strdup_printf(_("%s wants to connect"), name);
  else if (g_str_has_suffix(op, "encrypt")) headline = g_strdup_printf(_("%s wants to encrypt a message"), name);
  else if (g_str_has_suffix(op, "decrypt")) headline = g_strdup_printf(_("%s wants to decrypt a message"), name);
  else if (kind == NOSTR_KIND_TEXT_NOTE) headline = g_strdup_printf(_("%s wants to sign a note"), name);
  else if (kind == NOSTR_KIND_FOLLOW_LIST) headline = g_strdup_printf(_("%s wants to update your contact list"), name);
  else if (kind == NOSTR_KIND_CLIENT_AUTHENTICATION) headline = g_strdup_printf(_("%s wants to sign in to a relay"), name);
  else if (!strcmp(gnostr_approval_kind_name(kind), _("Encrypted message")) ||
           !strcmp(gnostr_approval_kind_name(kind), _("Private message")))
    headline = g_strdup_printf(_("%s wants to sign an encrypted message"), name);
  else if (!strcmp(gnostr_approval_kind_name(kind), _("Custom event")))
    headline = g_strdup_printf(_("%s wants to sign a custom event (kind %u)"), name, kind);
  else headline = g_strdup_printf(_("%s wants to %s"), name, self->action_text);
  gtk_label_set_text(self->header_title, headline);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->row_action), declared ? _("Allow It To Use") : _("Action"));
  adw_action_row_set_subtitle(self->row_action, self->action_text);
  gtk_button_set_label(self->btn_deny, declared ? _("_Deny") : _("_Deny Once"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->btn_deny), declared ? _("Deny This Request (Alt+D, Escape)") : _("Deny Once (Alt+D, Escape)"));
  g_free(self->flow_scope);
  self->flow_scope = g_strdup_printf(_("%u encrypted copies, including your copy, as %s; at most %u seconds left from first arrival. Count is supplied by the app, not proof of recipients. No permission is saved to disk"), size, identity_label, flow_left);
  g_autoptr(GtkStringList) choices = gtk_string_list_new(NULL);
  self->n_choices = 0;
#define CHOICE(mode, label) do { self->choices[self->n_choices++] = mode; gtk_string_list_append(choices, label); } while (0)
  if (declared) {
    /* One review, one lasting answer: never "once" or 24 h for a declared set. */
    if (rememberable) CHOICE("allow", _("Allow these permissions"));
    if (rememberable && routine_trust) CHOICE("trust-routine", _("Trust this app for routine operations"));
    if (!rememberable) self->ready = FALSE;
  } else CHOICE("allow-once", _("Allow once"));
  if (declared) {
  } else if (review) {
    if (rememberable) CHOICE("activate-saved", _("Activate saved permission"));
    CHOICE("discard-saved", _("Discard saved permission"));
  } else if (rememberable) {
    CHOICE("allow-24h", _("Allow for 24 hours"));
    g_autofree gchar *permission = g_strdup_printf(_("Always allow this app to %s as %s%s%s"),
        self->reusable_text, identity_label, *self->relay ? _(" at ") : "", self->relay);
    CHOICE("allow", permission);
    if (routine_trust) CHOICE("trust-routine", _("Trust this app for routine operations"));
    if (flow) CHOICE("flow", _("This message only…"));
  }
#undef CHOICE
  /* Reset on every authoritative request snapshot; never carry consent between
   * steps. An app that already holds a declared set is extending it: the
   * lasting choice is the default, allow-once stays available. */
  guint initial = 0;
  if (extends && !declared && !review)
    for (guint i = 0; i < self->n_choices; i++) if (!strcmp(self->choices[i], "allow")) initial = i;
  adw_combo_row_set_model(self->permission_choice, G_LIST_MODEL(choices));
  adw_combo_row_set_selected(self->permission_choice, initial);
  /* The reusable choice appears only where there is a choice to make. */
  gtk_widget_set_visible(GTK_WIDGET(self->permission_choice), self->n_choices > 1 &&
      strcmp(op, "connect") && !self_test && strcmp(field(info, "transport"), "MANAGEMENT"));
  gtk_widget_set_sensitive(GTK_WIDGET(self->permission_choice), self->ready);
  g_free(self->saved_expiry);
  self->saved_expiry = NULL;
  g_autoptr(GVariant) permissions = g_variant_lookup_value(info, "permission_rows", G_VARIANT_TYPE("aa{sv}"));
  g_autoptr(GString) other_identities = g_string_new(NULL);
  const gchar *saved_reason = _("Inactive under the previous access policy");
  for (gsize i = 0; permissions && i < g_variant_n_children(permissions); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(permissions, i);
    gint saved_kind = -1;
    g_variant_lookup(row, "event-kind", "i", &saved_kind);
    if (strcmp(field(row, "operation"), !strcmp(op, "sign_event") ? "event" : op) ||
        saved_kind != (has_kind ? (gint)kind : -1) ||
        (*field(row, "relay") && strcmp(field(row, "relay"), self->relay))) continue;
    if (strcmp(field(row, "identity"), field(info, "identity"))) {
      if (strcmp(field(row, "identity"), "*"))
        g_string_append(other_identities, _("Saved permission is for another identity. "));
    } else if (!strcmp(field(row, "decision"), "policy-review")) {
      guint64 expires = 0;
      if (g_variant_lookup(row, "expires-at", "t", &expires)) {
        g_clear_pointer(&self->saved_expiry, g_free);
        if (expires) {
          g_autoptr(GDateTime) time = g_date_time_new_from_unix_local((gint64)expires);
          self->saved_expiry = time ? g_date_time_format(time, "%c %Z") : g_strdup(_("Invalid expiry"));
        } else self->saved_expiry = g_strdup(_("Until revoked"));
      }
      if (*field(row, "reason")) saved_reason = field(row, "reason");
    }
  }
  /* Keep the row variant's strings alive through construction below. */
  if ((review && !self->saved_expiry) || (drift && !permissions)) self->ready = FALSE;
  if (!self->saved_expiry) self->saved_expiry = g_strdup(_("Loading…"));
  g_autofree gchar *saved = review
      ? g_strdup_printf(_("%s. Identity: %s%s%s. Original expiry: %s"),
          saved_reason, identity_label, *self->relay ? _(" at ") : "", self->relay, self->saved_expiry)
      : g_strdup_printf(_("%sPermissions do not follow a change of default identity"), other_identities->str);
  adw_action_row_set_subtitle(self->saved_status, saved);
  g_autofree gchar *preview = NULL, *declared_effect = NULL;
  const char *impact = _("Grotto returns the result to the app. It does not publish anything.");
  if (!strcmp(op, "sign_event")) {
    guint32 tags = 0, targets = 0;
    if (summary) {
      g_variant_lookup(summary, "tag_count", "u", &tags);
      g_variant_lookup(summary, "delete_target_count", "u", &targets);
    }
    switch (kind) {
    case NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE:
    case NOSTR_KIND_SEAL:
    case NOSTR_KIND_GIFT_WRAP:
    case NOSTR_KIND_DIRECT_MESSAGE:
    case NOSTR_KIND_FILE_MESSAGE:
    case NOSTR_KIND_NOSTR_CONNECT:
      preview = g_strdup_printf(_("Encrypted message — private content is not shown.\nRecipient: %s"),
                                *field(summary, "recipient_peer") ? field(summary, "recipient_peer")
                                                                  : _("Not visible in this event"));
      break;
    case NOSTR_KIND_DELETION:
      preview = g_strdup_printf(
          _("Delete %u target(s). Deletion is a permanent request; copies may remain elsewhere."),
          targets);
      break;
    case NOSTR_KIND_CLIENT_AUTHENTICATION:
      preview = g_strdup_printf(
          _("Relay claimed by app: %s\nAllows signing in to this relay. Grotto cannot verify the app's relay connection."),
          *field(summary, "auth_relay") ? field(summary, "auth_relay") : _("Not supplied by daemon"));
      break;
    case NOSTR_KIND_REPOST:
    case NOSTR_KIND_GENERIC_REPOST:
    case NOSTR_KIND_REACTION: {
      guint32 target_kind = 0;
      g_autofree gchar *type = NULL;
      if (summary && g_variant_lookup(summary, "target_kind", "u", &target_kind))
        type = g_strdup_printf(_("\nTarget type: %s (kind %u)"),
                               gnostr_approval_kind_name(target_kind), target_kind);
      const gchar *target = *field(summary, "target_event_id")
          ? field(summary, "target_event_id") : _("Not supplied by daemon");
      preview = kind == NOSTR_KIND_REACTION
          ? g_strdup_printf(_("Target: %s%s\nReaction: %s"), target,
              type ? type : "", field(summary, "reaction_value"))
          : g_strdup_printf(_("Target: %s%s"), target, type ? type : "");
      break;
    }
    case NOSTR_KIND_TEXT_NOTE:
    case NOSTR_KIND_ARTICLE:
    case NOSTR_KIND_COMMENT:
      preview = bounded(
          *field(summary, "content") ? field(summary, "content") : field(summary, "preview"), 280);
      break;
    default: {
      GString *text = g_string_new(NULL);
      g_string_append_printf(text, _("Kind %u · %u structured tags"), kind, tags);
      g_autoptr(GVariant) counts =
          summary ? g_variant_lookup_value(summary, "tag_counts", G_VARIANT_TYPE("a{su}")) : NULL;
      if (counts) {
        GVariantIter iter;
        const gchar *name;
        guint32 count;
        g_variant_iter_init(&iter, counts);
        while (g_variant_iter_next(&iter, "{&su}", &name, &count))
          g_string_append_printf(text, _("\n%s: %u"), name, count);
      }
      preview = g_string_free(text, FALSE);
      break;
    }
    }
  } else if (declared) {
    guint requested = 0, granted = 0, denied = 0;
    preview = declared_details(declared_set, &requested, &granted, &denied);
    g_autofree gchar *counts = granted || denied
        ? g_strdup_printf(_(" %u new, %u already allowed, %u blocked."), requested, granted, denied) : g_strdup("");
    declared_effect = g_strdup_printf(_("Grotto saves these permissions for %s until you revoke them in its Permissions page.%s Nothing is signed or published now; every later request listed here runs without asking."), name, counts);
    impact = declared_effect;
  } else if (g_str_has_suffix(op, "encrypt") || g_str_has_suffix(op, "decrypt")) {
    preview = g_strdup(_("Encrypted message. Private message bodies are not shown."));
  } else if (!strcmp(op, "self_test")) {
    preview = g_strdup(_("1. Sign and verify an unpublished test event.\n"
                         "2. Encrypt a random challenge to a temporary peer.\n"
                         "3. Decrypt the peer's reply and compare the challenge."));
    impact = _("One-time identity test. Nothing is published and no app permissions are saved.");
  } else if (!strcmp(op, "connect")) {
    g_auto(GStrv) urls = NULL, hints = NULL;
    if (summary) {
      g_variant_lookup(summary, "relays", "^as", &urls);
      g_variant_lookup(summary, "requested_permissions_hint", "^as", &hints);
    }
    g_autofree gchar *relays = urls ? g_strjoinv("\n", urls) : g_strdup("");
    g_autofree gchar *permissions = hints ? g_strjoinv(", ", hints) : g_strdup("");
    adw_action_row_set_subtitle(self->row_relay, relays);
    gtk_widget_set_visible(GTK_WIDGET(self->row_relay), *relays != '\0');
    gboolean grants = FALSE;
    if (summary) g_variant_lookup(summary, "granted_on_pairing", "b", &grants);
    /* With a pinned identity the daemon reports each perm's standing: an
     * explicit block is kept, never replaced by pairing; AUTH is any-relay. */
    g_autoptr(GVariant) standing = summary ? g_variant_lookup_value(summary, "declared_permissions", G_VARIANT_TYPE("aa{sv}")) : NULL;
    if (standing && g_variant_n_children(standing)) {
      guint requested = 0, already = 0, blocked = 0;
      g_autofree gchar *lines = declared_details(standing, &requested, &already, &blocked);
      g_free(permissions); permissions = g_strconcat("\n", lines, NULL);
    }
    preview = g_strdup_printf(grants
        ? _("App fingerprint: %s\nClaimed name (unverified): %s\nRequested permissions (granted on pairing, until revoked; blocked ones stay blocked): %s\nRelays: %s")
        : _("App fingerprint: %s\nClaimed name (unverified): "
            "%s\nRequested permissions (hints only): %s\nRelays: %s"),
                              field(info, "principal"), field(info, "claimed_app_id"),
                              permissions, relays);
    impact = grants ? _("Pairing admits this application and saves the permissions it requested (any relay for relay sign-in; blocked ones stay blocked), so those operations will not ask again.")
                    : _("Pairing admits this application. It grants no signing permissions.");
  }
  g_autoptr(JsonNode) management = profile_management(info) ? management_event(info) : NULL;
  if (management) {
    g_free(preview); preview = management_preview(management, kind);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->row_relay), kind == NOSTR_KIND_BLOBS ? _("Upload Servers") : _("Relays"));
    adw_action_row_set_subtitle(self->row_relay, _("destinations: see Details"));
    gtk_widget_set_visible(GTK_WIDGET(self->row_relay), TRUE);
    impact = kind == NOSTR_KIND_PROFILE_METADATA ? _("Approval signs this exact profile event and allows Grotto to publish it to the publication relays shown in this review. Public copies may remain even if you stop later.") :
      kind == NOSTR_KIND_BLOBS ? _("Approval signs the authorization shown here. Grotto then uploads the reviewed, cropped and re-encoded PNG image to the listed servers, without the original file's embedded metadata. It becomes public immediately, independently of profile Save. Stopping does not delete it or recall copies.") :
      _("Approval signs this relay challenge. Grotto sends AUTH to the relay shown above. This is not permission to publish a profile or save reusable access.");
  }
  if (preview)
    gtk_label_set_text(self->content_preview, preview);
  if (!gnostr_approval_info_can_approve(info)) {
    self->ready = FALSE;
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve), FALSE);
    impact = _("The daemon did not supply the required preview details. Update Grotto before "
               "approving; you can still deny this request.");
  }
  g_autofree gchar *effect = !strcmp(op, "sign_event") && kind == NOSTR_KIND_TEXT_NOTE
      ? g_strdup_printf(_("%s can publish this signed note. Grotto does not publish it"), name)
      : !strcmp(op, "sign_event") && kind == NOSTR_KIND_FOLLOW_LIST
      ? g_strdup_printf(_("%s can publish this signed contact list. Grotto does not publish it"), name)
      : NULL;
  adw_action_row_set_subtitle(self->row_impact, effect ? effect : impact);
  g_autofree gchar *details =
      g_strdup_printf(_("%s\nPrincipal: %s\nTransport: %s\nClaimed name (unverified): %s\nSigning identity: %s\nPeer fingerprint: %s"),
                      self_test ? _("Grotto identity self-test") : verified ? _("Verified caller") : _("Unverified app"), field(info, "principal"),
                      field(info, "transport"), field(info, "claimed_app_id"), self->identity, peer);
  if (management) {
    g_autofree char *raw = g_strdup(field(summary, "management_event_json"));
    g_autofree char *profile = NULL;
    if (kind == NOSTR_KIND_PROFILE_METADATA) {
      profile = g_strdup(json_object_get_string_member(json_node_get_object(management), "content"));
    }
    char *full = g_strdup_printf("%s\n\n%s%s\n\n%s\n%s", details, profile ? _("Exact profile JSON:\n") : "", profile ? profile : "", _("Exact unsigned event JSON:"), raw);
    g_free(details); details = full;
  }
  if (management) {
    char *full = g_strdup_printf("%s\n\n%s\n%s", details, _("Daemon destination details:"), field(info, "reason"));
    g_free(details); details = full;
  }
  g_free(self->profile_details); self->profile_details = management ? g_strdup(details) : NULL;
  gtk_label_set_text(self->label_details, details);
  gtk_widget_set_visible(GTK_WIDGET(self->loading), (review || drift) && !permissions);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve), self->ready);
  gtk_widget_set_sensitive(GTK_WIDGET(self->permission_choice), self->ready);
  g_simple_action_set_enabled(self->block_operation, rememberable && self->ready && !declared);
  g_simple_action_set_enabled(self->block_app, rememberable && self->ready);
  /* Default view stays compact: Details opens only on request. */
  adw_expander_row_set_expanded(self->details_expander, FALSE);
  permission_changed(NULL, NULL, self);
}
void gnostr_approval_dialog_set_profile_provenance(GnostrApprovalDialog *self,
    const char *request_id, const char *const *destinations, const char *const *bootstrap) {
  if (!self->info || !profile_management(self->info) || !self->profile_details ||
      !request_id || g_strcmp0(request_id, field(self->info, "request_id"))) return;
  g_autofree char *targets = destinations ? g_strjoinv("\n", (char **)destinations) : g_strdup("");
  g_autofree char *lookups = bootstrap ? g_strjoinv("\n", (char **)bootstrap) : g_strdup("");
  guint32 kind = 0; g_variant_lookup(self->info, "event_kind", "u", &kind);
  const char *title = kind == NOSTR_KIND_PROFILE_METADATA ? _("Publication relays:") :
    kind == NOSTR_KIND_BLOBS ? _("Image upload servers:") : _("Relay authentication destination:");
  g_autofree char *display = g_strdup_printf("%s\n%s\n\n%s\n%s", title, *targets ? targets : _("None recorded"),
    _("Host bootstrap discovery (lookup only, never a publish fallback or authentication grant):"), *lookups ? lookups : _("Not used"));
  adw_action_row_set_subtitle(self->row_relay, display);
  g_autofree char *details = g_strdup_printf("%s\n\n%s", self->profile_details, display);
  gtk_label_set_text(self->label_details, details);
}
GnostrApprovalDialog *gnostr_show_approval_request_dialog(GtkWidget *parent, const char *identity,
                                                          const char *kind, const char *name,
                                                          const char *claimed, gboolean verified,
                                                          const char *preview, AccountsStore *store,
                                                          GnostrApprovalCallback callback,
                                                          gpointer data) {
  (void)store;
  GnostrApprovalDialog *dialog = gnostr_approval_dialog_new();
  gnostr_approval_dialog_set_identity(dialog, identity);
  gnostr_approval_dialog_set_request(dialog, kind, name, claimed, verified, preview);
  gnostr_approval_dialog_set_callback(dialog, callback, data);
  adw_dialog_present(ADW_DIALOG(dialog), parent);
  return dialog;
}
void gnostr_show_approval_dialog_full(GtkWidget *parent, const char *identity, const char *name,
                                      const char *content, int kind, guint64 timestamp,
                                      AccountsStore *store, GnostrApprovalCallback callback,
                                      gpointer data) {
  GnostrApprovalDialog *dialog = gnostr_show_approval_request_dialog(
      parent, identity, "sign_event", name, NULL, FALSE, content, store, callback, data);
  gnostr_approval_dialog_set_event_type(dialog, kind);
  gnostr_approval_dialog_set_timestamp(dialog, timestamp);
}
void gnostr_show_approval_dialog(GtkWidget *parent, const char *identity, const char *name,
                                 const char *content, AccountsStore *store,
                                 GnostrApprovalCallback callback, gpointer data) {
  gnostr_show_approval_dialog_full(parent, identity, name, content, NOSTR_KIND_TEXT_NOTE, 0, store,
                                   callback, data);
}
gboolean gnostr_show_approval_dialog_with_session(GtkWidget *parent, const char *client,
                                                  const char *identity, const char *name,
                                                  const char *content, int kind, guint64 timestamp,
                                                  AccountsStore *store,
                                                  GnostrApprovalCallback callback, gpointer data) {
  (void)client;
  /* Legacy entry point retained, but GUI sessions never authorize signing. */
  gnostr_show_approval_dialog_full(parent, identity, name, content, kind, timestamp, store,
                                   callback, data);
  return TRUE;
}
