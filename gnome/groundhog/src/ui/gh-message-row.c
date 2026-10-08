#include "gh-message-row.h"
#include "gh-conversation-open-probe-private.h"
#include "gh-display-name.h"
#include "gh-attachment-card.h"
#include "gh-reaction-bar.h"
#include "gh-reaction-picker.h"
#include "gh-conversation-row.h"
#include "gh-conversation-view.h"
#include "gh-delivery-indicator.h"
#include "gh-link-policy.h"

#ifdef GROUNDHOG_HAVE_VOICE
#include "gh-voice-bubble.h"
#include "gh-voice-meta.h"
#endif

#include <adwaita.h>
#include <glib/gi18n.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

/* Bubble text width caps (charter §7.6): 60 characters, 32 when compact. */
#define BODY_CHARS 60
#define BODY_CHARS_COMPACT 32

struct _GhMessageRow {
  GtkWidget parent_instance;
  GtkLabel *sender_label;
  GtkButton *reply_button;
  GtkLabel *reply_label;
  GtkBox *bubble;
  GtkLabel *body_label;
  GtkBox *voice_slot;      /* W27 voice: shown instead of attachment_slot */
  GtkWidget *voice_bubble;  /* GhVoiceBubble, created on demand */
  GtkBox *attachment_slot;
  GhAttachmentCard *attachment_card;
  GtkLabel *attachment_note;
  GPtrArray *extra_cards;   /* an encrypted group message's files 1.. (W25) */
  GtkBox *poll_slot;
  GtkWidget *poll_card;     /* set externally for kind-1068 poll messages */
  GtkBox *reaction_controls;
  GhReactionBar *reaction_bar;
  GhReactionSummary *reaction_summary; /* owned even while the bar is absent */
  GtkButton *react_button;
  GhReactionPicker *picker;    /* W26 slice B: quick-reaction popover */
  GtkBox *preview_box;
  GtkButton *preview_button;
  GtkButton *image_button;
  GtkButton *picture_button;
  GtkPicture *remote_image;
  GtkWidget *bubble_line;
  GtkWidget *legacy_box;
  AdwAvatar *avatar;
  GtkBox *web_box;
  GtkLabel *web_error;
  GtkLabel *preview_title;
  GtkLabel *preview_text;
  GtkBox *meta_box;
  GtkImage *timer_icon;
  GtkLabel *time_label;
  GhDeliveryIndicator *delivery;
  GtkButton *retry_button;

  GhMessage *message;
  GhConversationView *view; /* the enclosing view while rooted; not a reference */
  GBinding *compact_binding;
  gchar *preview_uri;       /* the first previewable link, or NULL */
  gchar *summary;
  gboolean run_start;
  gboolean run_end;
  gboolean show_sender;
  gboolean compact;
  gboolean undecryptable;
  gboolean has_reply;   /* nostrc-zjkv: the message has a reply_to_id */
};

enum {
  PROP_0,
  PROP_MESSAGE,
  PROP_RUN_START,
  PROP_RUN_END,
  PROP_SHOW_SENDER,
  PROP_COMPACT,
  PROP_UNDECRYPTABLE,
  PROP_SUMMARY,
  PROP_REACTION_SUMMARY,
  N_PROPS
};
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhMessageRow, gh_message_row, GTK_TYPE_WIDGET)

static void update_avatar(GhMessageRow *self);
static void update_web_images(GhMessageRow *self);

/* ---- text -------------------------------------------------------------------- */

gchar *
gh_message_row_display_name(const gchar *pubkey_hex)
{
  g_return_val_if_fail(pubkey_hex != NULL, NULL);
  return gh_display_name_for(pubkey_hex); /* W33: the kind-0 name when cached */
}

gchar *
gh_message_row_sender_name(GhMessage *message)
{
  g_return_val_if_fail(GH_IS_MESSAGE(message), NULL);
  return gh_message_is_self(message) ? g_strdup(_("You"))
                                     : gh_message_row_display_name(gh_message_get_sender(message));
}

static gboolean
ends_sentence(const gchar *text)
{
  gsize length = strlen(text);
  if (length == 0)
    return FALSE;
  gunichar last = g_utf8_get_char(g_utf8_prev_char(text + length));
  return last == '.' || last == '!' || last == '?' || last == 0x2026 /* … */;
}

/* Appends one sentence to a label, separated as prose. */
static void
append_sentence(GString *out, const gchar *sentence)
{
  if (!sentence || !*sentence)
    return;
  if (!ends_sentence(out->str))
    g_string_append_c(out, '.');
  g_string_append_c(out, ' ');
  g_string_append(out, sentence);
  if (!ends_sentence(sentence))
    g_string_append_c(out, '.');
}

/* How many files a message's bubble shows as cards (W25). */
static guint
card_count(GhMessage *message)
{
  if (gh_message_get_kind(message) == GH_NIP17_FILE_KIND)
    return 1;
  return gh_message_is_mls(message) ? gh_message_get_n_attachments(message) : 0;
}

#ifdef GROUNDHOG_HAVE_VOICE
/* W27 voice (nostrc-h4mk): TRUE when the message carries a single audio
 * file — a NIP-17 kind 15 whose declared MIME type starts with "audio/",
 * or an MLS message with exactly one attachment whose media_type does.
 * Multi-file messages are never voice (the first could be audio, but the
 * UX would be unclear). */
static gboolean
is_voice_message(GhMessage *message)
{
  if (gh_message_get_kind(message) == GH_NIP17_FILE_KIND) {
    g_autoptr(GhNip17File) file = gh_message_dup_file(message);
    return file && gh_voice_meta_is_audio(file->file_type);
  }
  if (gh_message_is_mls(message) && gh_message_get_n_attachments(message) == 1) {
    const GhMessageAttachment *att = gh_message_get_attachment(message, 0);
    return att && gh_voice_meta_is_audio(att->media_type);
  }
  return FALSE;
}
#endif

/* A kind-15 file message is "Photo" or "File" in text
 * (gh_message_dup_display_text()), never its URL, and its bubble holds the
 * attachment card (G22). The URL names encrypted bytes on a Blossom server:
 * as a link it could open in a browser outside the network mode (Tor) and
 * would be a preview candidate. NULL for any other message. */
static gchar *
file_text(GhMessage *message)
{
#ifdef GROUNDHOG_HAVE_VOICE
  if (is_voice_message(message))
    return g_strdup(_("Voice message"));
#endif
  if (gh_message_get_kind(message) == GH_NIP17_FILE_KIND)
    return gh_message_dup_display_text(message);
  /* An encrypted group's files without a caption: "Photo", "3 files". */
  const gchar *content = gh_message_get_content(message);
  if (gh_message_get_n_attachments(message) > 0 && (!content || !*content))
    return gh_message_dup_display_text(message);
  return NULL;
}

static gchar *
compose_summary(GhMessage *message, GDateTime *now, gboolean undecryptable)
{
  g_autofree gchar *sender = gh_message_row_sender_name(message);
  g_autofree gchar *time =
    gh_conversation_row_format_message_time(gh_message_get_created_at(message), now);
  gboolean withdrawn = !undecryptable && gh_message_get_withdrawn(message);
  g_autofree gchar *file = undecryptable || withdrawn ? NULL : file_text(message);
  const gchar *body = undecryptable ? _("Unable to decrypt yet")
                      : withdrawn   ? gh_message_withdrawn_text()
                      : file        ? file
                                    : gh_message_get_content(message);
  /* TRANSLATORS: a message's accessible label: sender, time, text. */
  GString *out = g_string_new(NULL);
  g_string_printf(out, _("%s, %s: %s"), sender, time, body);
  guint files = undecryptable || withdrawn || file ? 0 : gh_message_get_n_attachments(message);
  if (files > 0) {
    /* TRANSLATORS: a captioned message's attached files, in its accessible label. */
    g_autofree gchar *count = g_strdup_printf(g_dngettext(NULL, "%u file attached",
                                                          "%u files attached", files), files);
    append_sentence(out, count);
  }
  /* nostrc-zjkv: mention reply in accessible text. */
  if (!undecryptable && !withdrawn && gh_message_get_reply_to_id(message))
    append_sentence(out, _("In reply to another message"));
  if (gh_message_get_expires_at(message) > 0)
    append_sentence(out, _("Disappearing message"));
  if (gh_message_is_self(message))
    append_sentence(out, gh_message_status_get_label(gh_message_get_status(message)));
  return g_string_free(out, FALSE);
}

gchar *
gh_message_row_compose_summary(GhMessage *message, GDateTime *now)
{
  g_return_val_if_fail(GH_IS_MESSAGE(message), NULL);
  g_return_val_if_fail(now != NULL, NULL);
  return compose_summary(message, now, FALSE);
}

/* ---- updates ------------------------------------------------------------------ */

static void
set_summary(GhMessageRow *self, gchar *summary)
{
  if (g_strcmp0(self->summary, summary) == 0) {
    g_free(summary);
    return;
  }
  g_free(self->summary);
  self->summary = summary;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SUMMARY]);
}

static void
update_summary(GhMessageRow *self)
{
  if (!self->message) {
    set_summary(self, g_strdup(""));
    return;
  }
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  set_summary(self, compose_summary(self->message, now, self->undecryptable));
}

static void
set_class(GtkWidget *widget, const gchar *name, gboolean on)
{
  if (on)
    gtk_widget_add_css_class(widget, name);
  else
    gtk_widget_remove_css_class(widget, name);
}

static void
update_width(GhMessageRow *self)
{
  gint chars = self->compact ? BODY_CHARS_COMPACT : BODY_CHARS;
  gtk_label_set_max_width_chars(self->body_label, chars);
#ifdef GROUNDHOG_HAVE_VOICE
  if (self->voice_bubble)
    gh_voice_bubble_set_compact(GH_VOICE_BUBBLE(self->voice_bubble), self->compact);
#endif
  if (self->attachment_card)
    gh_attachment_card_set_compact(self->attachment_card, self->compact);
  for (guint i = 0; self->extra_cards && i < self->extra_cards->len; i++)
    gh_attachment_card_set_compact(g_ptr_array_index(self->extra_cards, i), self->compact);
  if (self->preview_title)
    gtk_label_set_max_width_chars(self->preview_title, chars);
  if (self->preview_text)
    gtk_label_set_max_width_chars(self->preview_text, chars);
}

/* The avatar: the sender's name (initials) and loaded picture, at the end
 * of a run; transparent on the other rows so bubbles stay in line. */
static void
update_avatar(GhMessageRow *self)
{
  if (!self->message) {
    adw_avatar_set_text(self->avatar, NULL);
    adw_avatar_set_custom_image(self->avatar, NULL);
    return;
  }
  const gchar *sender = gh_message_get_sender(self->message);
  g_autofree gchar *name = sender ? gh_display_name_for(sender) : NULL;
  adw_avatar_set_text(self->avatar, name);
  GdkTexture *texture = NULL;
  if (self->view && sender) {
    GhLinkPreviewState state = GH_LINK_PREVIEW_NONE;
    texture = gh_conversation_view_get_web_texture(self->view, self->message, GH_WEB_PICTURE,
                                                   &state);
  }
  adw_avatar_set_custom_image(self->avatar, texture ? GDK_PAINTABLE(texture) : NULL);
  gtk_widget_set_opacity(GTK_WIDGET(self->avatar), self->run_end ? 1.0 : 0.0);
}

static void
update_runs(GhMessageRow *self)
{
  set_class(GTK_WIDGET(self), "run-start", self->run_start);
  set_class(GTK_WIDGET(self), "run-end", self->run_end);
  if (self->message)
    update_web_images(self); /* the picture button is once per run */
  else
    update_avatar(self);
  gboolean outgoing = self->message && gh_message_is_self(self->message);
  gtk_widget_set_visible(GTK_WIDGET(self->sender_label),
                         self->message && self->show_sender && !outgoing);
}

/* The meta line: always at the end of a run, and on any message whose
 * state needs saying (disappearing, or an own message not simply "Sent"). */
static void
update_meta(GhMessageRow *self)
{
  GhMessage *message = self->message;
  if (!message) {
    gtk_widget_set_visible(GTK_WIDGET(self->meta_box), FALSE);
    return;
  }
  GhMessageStatus status = gh_message_is_self(message) ? gh_message_get_status(message)
                                                       : GH_MESSAGE_STATUS_NONE;
  gboolean expiring = gh_message_get_expires_at(message) > 0;
  gboolean noteworthy = status != GH_MESSAGE_STATUS_NONE && status != GH_MESSAGE_STATUS_SENT;
  gboolean legacy = gh_message_get_legacy_nip04(message);
  gboolean reacted = self->reaction_summary &&
    gh_reaction_summary_get_total_count(self->reaction_summary) > 0;
  if (legacy && !self->legacy_box) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    self->legacy_box = box;
    gtk_widget_set_name(box, "legacy_box");
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(box, _("Sent with the older NIP-04 format: anyone can see who wrote to whom and when, and its encryption is weaker. Your replies are sent privately (NIP-17)."));
    gtk_widget_add_css_class(box, "warning");
    gtk_widget_add_css_class(box, "groundhog-legacy-nip04");
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 12);
    gtk_accessible_update_property(GTK_ACCESSIBLE(icon), GTK_ACCESSIBLE_PROPERTY_LABEL, "", -1);
    gtk_box_append(GTK_BOX(box), icon);
    GtkWidget *label = gtk_label_new(_("Less private (NIP-04)"));
    gtk_widget_add_css_class(label, "caption");
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_insert_child_after(self->meta_box, box, GTK_WIDGET(self->timer_icon));
  } else if (!legacy && self->legacy_box) {
    gtk_box_remove(self->meta_box, self->legacy_box);
    self->legacy_box = NULL;
  }
  gtk_widget_set_visible(GTK_WIDGET(self->meta_box),
                         self->run_end || expiring || noteworthy || legacy || reacted);
  /* W17: a room message that reached only some people can be tried again
   * for the others (the same stored wraps; those who have it are skipped). */
  gboolean partial = status == GH_MESSAGE_STATUS_PARTIALLY_SENT;
  gtk_widget_set_visible(GTK_WIDGET(self->retry_button),
                         status == GH_MESSAGE_STATUS_NOT_SENT || partial);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->retry_button),
                              partial ? _("Send this message again to the people who don't "
                                          "have it yet")
                                      : _("Send this message again"));
}

static void
on_reaction_toggled(GhReactionBar *bar, const gchar *emoji, gboolean add, GhMessageRow *self);

static void
sync_reaction_bar(GhMessageRow *self)
{
  gboolean needed = self->reaction_summary &&
    gh_reaction_summary_get_total_count(self->reaction_summary) > 0;
  if (needed && !self->reaction_bar) {
    self->reaction_bar = GH_REACTION_BAR(gh_reaction_bar_new());
    gtk_widget_set_name(GTK_WIDGET(self->reaction_bar), "reaction_bar");
    gtk_box_insert_child_after(self->reaction_controls, GTK_WIDGET(self->reaction_bar), NULL);
    g_signal_connect(self->reaction_bar, "reaction-toggled",
                     G_CALLBACK(on_reaction_toggled), self);
    gh_reaction_bar_set_summary(self->reaction_bar, self->reaction_summary);
    if (self->message && gh_message_is_self(self->message))
      gtk_box_reorder_child_after(self->reaction_controls, GTK_WIDGET(self->reaction_bar),
                                  GTK_WIDGET(self->react_button));
  } else if (!needed && self->reaction_bar) {
    gh_reaction_bar_set_summary(self->reaction_bar, NULL);
    gtk_box_remove(self->reaction_controls, GTK_WIDGET(self->reaction_bar));
    self->reaction_bar = NULL;
  }
}

static void
on_reaction_total_changed(GObject *summary, GParamSpec *pspec, GhMessageRow *self)
{
  (void)summary;
  (void)pspec;
  sync_reaction_bar(self);
  update_meta(self);
}

static void
update_expiry(GhMessageRow *self)
{
  gint64 expires = self->message ? gh_message_get_expires_at(self->message) : 0;
  gtk_widget_set_visible(GTK_WIDGET(self->timer_icon), expires > 0);
  if (expires > 0) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *when = gh_conversation_row_format_message_time(expires, now);
    g_autofree gchar *tooltip = g_strdup_printf(_("Disappears %s"), when);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->timer_icon), tooltip);
  }
}

static void
clear_web_controls(GhMessageRow *self)
{
  while (gtk_widget_get_first_child(GTK_WIDGET(self->web_box)))
    gtk_box_remove(self->web_box, gtk_widget_get_first_child(GTK_WIDGET(self->web_box)));
  self->picture_button = NULL;
  self->image_button = NULL;
  self->remote_image = NULL;
  self->web_error = NULL;
}

static GtkButton *
new_web_button(GhMessageRow *self, const char *name, const char *label)
{
  GtkWidget *button = gtk_button_new_with_label(label);
  gtk_widget_set_name(button, name);
  gtk_widget_set_halign(button, GTK_ALIGN_START);
  gtk_widget_add_css_class(button, "flat");
  gtk_actionable_set_action_target(GTK_ACTIONABLE(button), "s", "");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "conversation.show-preview");
  gtk_box_append(self->web_box, button);
  return GTK_BUTTON(button);
}

static void
clear_preview_controls(GhMessageRow *self)
{
  while (gtk_widget_get_first_child(GTK_WIDGET(self->preview_box)))
    gtk_box_remove(self->preview_box, gtk_widget_get_first_child(GTK_WIDGET(self->preview_box)));
  self->preview_button = NULL;
  self->preview_title = NULL;
  self->preview_text = NULL;
  gtk_widget_set_visible(GTK_WIDGET(self->preview_box), FALSE);
}

static void
ensure_preview_controls(GhMessageRow *self)
{
  if (self->preview_button)
    return;
  self->preview_button = GTK_BUTTON(gtk_button_new_with_label(_("Show Preview")));
  gtk_widget_set_name(GTK_WIDGET(self->preview_button), "preview_button");
  gtk_widget_set_halign(GTK_WIDGET(self->preview_button), GTK_ALIGN_START);
  gtk_widget_add_css_class(GTK_WIDGET(self->preview_button), "flat");
  gtk_widget_add_css_class(GTK_WIDGET(self->preview_button), "groundhog-preview-button");
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->preview_button), "s", "");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(self->preview_button), "conversation.show-preview");
  gtk_box_append(self->preview_box, GTK_WIDGET(self->preview_button));
  self->preview_title = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_set_name(GTK_WIDGET(self->preview_title), "preview_title");
  gtk_label_set_xalign(self->preview_title, 0);
  gtk_label_set_wrap(self->preview_title, TRUE);
  gtk_label_set_wrap_mode(self->preview_title, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(self->preview_title, self->compact ? BODY_CHARS_COMPACT : BODY_CHARS);
  gtk_label_set_selectable(self->preview_title, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->preview_title), "heading");
  gtk_box_append(self->preview_box, GTK_WIDGET(self->preview_title));
  self->preview_text = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_set_name(GTK_WIDGET(self->preview_text), "preview_text");
  gtk_label_set_xalign(self->preview_text, 0);
  gtk_label_set_wrap(self->preview_text, TRUE);
  gtk_label_set_wrap_mode(self->preview_text, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(self->preview_text, self->compact ? BODY_CHARS_COMPACT : BODY_CHARS);
  gtk_label_set_selectable(self->preview_text, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->preview_text), "dim-label");
  gtk_box_append(self->preview_box, GTK_WIDGET(self->preview_text));
}

static void
update_web_images(GhMessageRow *self)
{
  gboolean available = self->message && self->view &&
    gh_conversation_view_has_web_content(self->view) &&
    !gh_message_get_withdrawn(self->message);
  g_autofree gchar *picture = available
    ? gh_conversation_view_dup_picture_uri(self->view, self->message) : NULL;
  /* The preference (or an earlier allow for this sender) loads without a
   * click; the buttons below stay for what is not loaded. */
  if (available) {
    if (self->preview_uri) gh_conversation_view_auto_load(self->view, self->message, GH_WEB_IMAGE);
    if (picture) gh_conversation_view_auto_load(self->view, self->message, GH_WEB_PICTURE);
  }
  GhLinkPreviewState image_state = GH_LINK_PREVIEW_NONE;
  GhLinkPreviewState picture_state = GH_LINK_PREVIEW_NONE;
  GdkTexture *texture = available
    ? gh_conversation_view_get_web_texture(self->view, self->message, GH_WEB_IMAGE,
                                           &image_state) : NULL;
  if (available)
    gh_conversation_view_get_web_texture(self->view, self->message, GH_WEB_PICTURE,
                                         &picture_state);
  gboolean has_image = available && self->preview_uri;
  gboolean has_picture = picture && self->run_start &&
    !gh_message_is_self(self->message);
  gboolean ask_image = has_image &&
    image_state != GH_LINK_PREVIEW_LOADED && image_state != GH_LINK_PREVIEW_LOADING;
  gboolean ask_picture = has_picture &&
    picture_state != GH_LINK_PREVIEW_LOADED && picture_state != GH_LINK_PREVIEW_LOADING;
  gboolean failed = image_state == GH_LINK_PREVIEW_FAILED;
  if (!has_image && !has_picture && !texture && !failed) {
    clear_web_controls(self);
    update_avatar(self);
    return;
  }
  if (ask_picture && !self->picture_button)
    self->picture_button = new_web_button(self, "picture_button", _("Load Profile Picture"));
  if (has_picture && self->picture_button) {
    g_autofree gchar *id = g_strconcat("picture:", gh_message_get_rumor_id(self->message), NULL);
    gtk_actionable_set_action_target(GTK_ACTIONABLE(self->picture_button), "s", id);
    gtk_widget_set_visible(GTK_WIDGET(self->picture_button), ask_picture);
    gtk_widget_set_sensitive(GTK_WIDGET(self->picture_button),
                             picture_state == GH_LINK_PREVIEW_NONE ||
                             picture_state == GH_LINK_PREVIEW_FAILED);
  } else if (!has_picture && self->picture_button) {
    gtk_box_remove(self->web_box, GTK_WIDGET(self->picture_button));
    self->picture_button = NULL;
  }
  if (ask_image && !self->image_button)
    self->image_button = new_web_button(self, "image_button", _("Load Linked Image"));
  if (has_image && self->image_button) {
    g_autofree gchar *id = g_strconcat("image:", gh_message_get_rumor_id(self->message), NULL);
    gtk_actionable_set_action_target(GTK_ACTIONABLE(self->image_button), "s", id);
    gtk_widget_set_visible(GTK_WIDGET(self->image_button), ask_image);
    gtk_widget_set_sensitive(GTK_WIDGET(self->image_button),
                             image_state == GH_LINK_PREVIEW_NONE ||
                             image_state == GH_LINK_PREVIEW_FAILED);
  } else if (!has_image && self->image_button) {
    gtk_box_remove(self->web_box, GTK_WIDGET(self->image_button));
    self->image_button = NULL;
  }
  if (texture) {
    if (!self->remote_image) {
      self->remote_image = GTK_PICTURE(gtk_picture_new());
      gtk_widget_set_name(GTK_WIDGET(self->remote_image), "remote_image");
      gtk_picture_set_can_shrink(self->remote_image, TRUE);
      gtk_picture_set_content_fit(self->remote_image, GTK_CONTENT_FIT_CONTAIN);
      gtk_widget_set_size_request(GTK_WIDGET(self->remote_image), -1, 180);
      gtk_accessible_update_property(GTK_ACCESSIBLE(self->remote_image),
                                     GTK_ACCESSIBLE_PROPERTY_LABEL,
                                     _("Image linked in this message"), -1);
      gtk_box_append(self->web_box, GTK_WIDGET(self->remote_image));
    }
    gtk_picture_set_paintable(self->remote_image, GDK_PAINTABLE(texture));
  } else if (self->remote_image) {
    gtk_box_remove(self->web_box, GTK_WIDGET(self->remote_image));
    self->remote_image = NULL;
  }
  if (failed) {
    if (!self->web_error) {
      self->web_error = GTK_LABEL(gtk_label_new(_("The image couldn't be loaded.")));
      gtk_widget_set_name(GTK_WIDGET(self->web_error), "web_error");
      gtk_label_set_wrap(self->web_error, TRUE);
      gtk_label_set_xalign(self->web_error, 0);
      gtk_widget_add_css_class(GTK_WIDGET(self->web_error), "error");
      gtk_widget_add_css_class(GTK_WIDGET(self->web_error), "caption");
      gtk_box_append(self->web_box, GTK_WIDGET(self->web_error));
    }
  } else if (self->web_error) {
    gtk_box_remove(self->web_box, GTK_WIDGET(self->web_error));
    self->web_error = NULL;
  }
  update_avatar(self);
}

static void
update_preview(GhMessageRow *self)
{
  update_web_images(self);
  /* Only with a fetcher (W13b review, non-blocking #1). */
  gboolean offered = self->message && self->preview_uri && self->view &&
                     gh_conversation_view_get_previews_available(self->view);
  if (!offered) {
    clear_preview_controls(self);
    return;
  }
  ensure_preview_controls(self);
  gtk_widget_set_visible(GTK_WIDGET(self->preview_box), TRUE);
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->preview_button), "s",
                                   gh_message_get_rumor_id(self->message));
  g_autofree gchar *host = gh_link_policy_dup_host(self->preview_uri);
  g_autofree gchar *tooltip = g_strdup_printf(_("Load a preview from %s"), host);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->preview_button), tooltip);
  const gchar *title = NULL;
  const gchar *description = NULL;
  GhLinkPreviewState state =
    self->view ? gh_conversation_view_get_link_preview(self->view, self->message, &title,
                                                       &description)
               : GH_LINK_PREVIEW_NONE;
  const gchar *button = NULL;
  const gchar *text = NULL;
  switch (state) {
  case GH_LINK_PREVIEW_NONE:
  case GH_LINK_PREVIEW_ASKING:
    button = _("Show Preview");
    break;
  case GH_LINK_PREVIEW_LOADING:
    button = _("Loading Preview…");
    break;
  case GH_LINK_PREVIEW_FAILED:
    button = _("Show Preview");
    text = _("The preview couldn't be loaded.");
    break;
  case GH_LINK_PREVIEW_UNAVAILABLE:
    text = _("Link previews aren't available in this version of Groundhog. Nothing was loaded.");
    break;
  case GH_LINK_PREVIEW_LOADED:
    text = description;
    break;
  }
  gtk_widget_set_visible(GTK_WIDGET(self->preview_button), button != NULL);
  if (button)
    gtk_button_set_label(self->preview_button, button);
  gtk_widget_set_sensitive(GTK_WIDGET(self->preview_button),
                           state == GH_LINK_PREVIEW_NONE || state == GH_LINK_PREVIEW_FAILED);
  gboolean has_title = state == GH_LINK_PREVIEW_LOADED && title && *title;
  gtk_label_set_text(self->preview_title, has_title ? title : "");
  gtk_widget_set_visible(GTK_WIDGET(self->preview_title), has_title);
  gtk_label_set_text(self->preview_text, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(self->preview_text), text && *text);
  set_class(GTK_WIDGET(self->preview_box), "card", state == GH_LINK_PREVIEW_LOADED);
}

static void
update_status(GhMessageRow *self)
{
  update_meta(self);
  update_summary(self);
}

/* File cards and rejection notes exist only while needed. */
static void
update_cards(GhMessageRow *self, GhMessage *message, guint cards)
{
  if (cards && !self->attachment_card) {
    self->attachment_card = GH_ATTACHMENT_CARD(gh_attachment_card_new());
    gtk_widget_set_name(GTK_WIDGET(self->attachment_card), "attachment_card");
    gh_attachment_card_set_compact(self->attachment_card, self->compact);
    gtk_box_append(self->attachment_slot, GTK_WIDGET(self->attachment_card));
  }
  if (self->attachment_card) {
    gh_attachment_card_set_index(self->attachment_card, 0);
    gh_attachment_card_set_message(self->attachment_card, cards ? message : NULL);
  }
  guint extra = cards > 1 ? cards - 1 : 0;
  if (!self->extra_cards)
    return;   /* disposed */
  while (self->extra_cards->len > extra) {
    GtkWidget *card = g_ptr_array_steal_index(self->extra_cards, self->extra_cards->len - 1);
    gtk_box_remove(self->attachment_slot, card);
  }
  for (guint i = 0; i < extra; i++) {
    GhAttachmentCard *card;
    if (i < self->extra_cards->len) {
      card = g_ptr_array_index(self->extra_cards, i);
    } else {
      card = GH_ATTACHMENT_CARD(gh_attachment_card_new());
      gh_attachment_card_set_compact(card, self->compact);
      GtkWidget *before = i == 0 ? GTK_WIDGET(self->attachment_card)
                                 : g_ptr_array_index(self->extra_cards, i - 1);
      gtk_box_insert_child_after(self->attachment_slot, GTK_WIDGET(card), before);
      g_ptr_array_add(self->extra_cards, card);
    }
    gh_attachment_card_set_message(card, NULL);
    gh_attachment_card_set_index(card, i + 1);
    gh_attachment_card_set_message(card, message);
  }
  if (!cards && self->attachment_card) {
    gtk_box_remove(self->attachment_slot, GTK_WIDGET(self->attachment_card));
    self->attachment_card = NULL;
  }
  guint rejected = message ? gh_message_get_rejected_attachments(message) : 0;
  if (rejected > 0 && !self->undecryptable) {
    if (!self->attachment_note) {
      self->attachment_note = GTK_LABEL(gtk_label_new(NULL));
      gtk_widget_set_name(GTK_WIDGET(self->attachment_note), "attachment_note");
      gtk_label_set_xalign(self->attachment_note, 0);
      gtk_label_set_wrap(self->attachment_note, TRUE);
      gtk_label_set_wrap_mode(self->attachment_note, PANGO_WRAP_WORD_CHAR);
      gtk_widget_add_css_class(GTK_WIDGET(self->attachment_note), "dim-label");
      gtk_widget_add_css_class(GTK_WIDGET(self->attachment_note), "caption");
      gtk_box_append(self->attachment_slot, GTK_WIDGET(self->attachment_note));
    }
    g_autofree gchar *note =
      g_strdup_printf(g_dngettext(NULL, "%u attached file can't be read: it isn't a valid "
                                        "encrypted file reference.",
                                  "%u attached files can't be read: they aren't valid "
                                  "encrypted file references.", rejected), rejected);
    gtk_label_set_text(self->attachment_note, note);
  } else if (self->attachment_note) {
    gtk_box_remove(self->attachment_slot, GTK_WIDGET(self->attachment_note));
    self->attachment_note = NULL;
  }
}

static void
update_all(GhMessageRow *self)
{
  GhMessage *message = self->message;
  gboolean outgoing = message && gh_message_is_self(message);
  set_class(GTK_WIDGET(self->bubble), "outgoing", message && outgoing);
  set_class(GTK_WIDGET(self->bubble), "incoming", message && !outgoing);
  GtkAlign align = outgoing ? GTK_ALIGN_END : GTK_ALIGN_START;
  gtk_widget_set_halign(GTK_WIDGET(self->bubble_line), align);
  gtk_widget_set_halign(GTK_WIDGET(self->bubble), align);
  /* Everything under the bubble starts where the bubble does, past the
   * avatar (32px and 6px of spacing). */
  const gint inset = 38;
  GtkWidget *under[] = { GTK_WIDGET(self->meta_box), GTK_WIDGET(self->preview_box),
                         GTK_WIDGET(self->web_box), GTK_WIDGET(self->sender_label),
                         GTK_WIDGET(self->reply_button) };
  for (guint u = 0; u < G_N_ELEMENTS(under); u++) {
    if (under[u]) {
      gtk_widget_set_margin_start(under[u], outgoing ? 0 : inset);
      gtk_widget_set_margin_end(under[u], outgoing ? inset : 0);
    }
  }
  if (outgoing)
    gtk_box_reorder_child_after(GTK_BOX(self->bubble_line), GTK_WIDGET(self->avatar),
                                GTK_WIDGET(self->bubble));
  else
    gtk_box_reorder_child_after(GTK_BOX(self->bubble_line), GTK_WIDGET(self->avatar), NULL);
  update_avatar(self);
  gtk_widget_set_halign(GTK_WIDGET(self->preview_box), align);
  gtk_widget_set_halign(GTK_WIDGET(self->meta_box), align);
  /* The controls stay together on the meta line. Incoming: chips, React,
   * then status. Outgoing: status, React, chips; chips end at the bubble. */
  if (outgoing) {
    gtk_box_reorder_child_after(self->reaction_controls, GTK_WIDGET(self->react_button), NULL);
    GtkWidget *last = gtk_widget_get_last_child(GTK_WIDGET(self->meta_box));
    if (last != GTK_WIDGET(self->reaction_controls))
      gtk_box_reorder_child_after(self->meta_box, GTK_WIDGET(self->reaction_controls), last);
  } else {
    gtk_box_reorder_child_after(self->reaction_controls, GTK_WIDGET(self->react_button),
                                self->reaction_bar ? GTK_WIDGET(self->reaction_bar) : NULL);
    gtk_box_reorder_child_after(self->meta_box, GTK_WIDGET(self->reaction_controls), NULL);
  }

  g_clear_pointer(&self->preview_uri, g_free);

  /* nostrc-zjkv: reply header — show "Replying to npub1…" when the message
   * has a reply-target event id. The button's action target is the reply-to
   * id so conversation.scroll-to-reply can find and scroll to it. */
  const gchar *reply_to = message && !self->undecryptable &&
    !gh_message_get_withdrawn(message) ? gh_message_get_reply_to_id(message) : NULL;
  self->has_reply = reply_to != NULL;
  if (self->has_reply) {
    if (!self->reply_button) {
      self->reply_button = GTK_BUTTON(gtk_button_new());
      gtk_widget_set_name(GTK_WIDGET(self->reply_button), "reply_button");
      gtk_widget_set_halign(GTK_WIDGET(self->reply_button), GTK_ALIGN_START);
      gtk_widget_add_css_class(GTK_WIDGET(self->reply_button), "flat");
      gtk_widget_add_css_class(GTK_WIDGET(self->reply_button), "caption");
      gtk_widget_add_css_class(GTK_WIDGET(self->reply_button), "dim-label");
      gtk_widget_add_css_class(GTK_WIDGET(self->reply_button), "groundhog-reply-header");
      GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      GtkWidget *icon = gtk_image_new_from_icon_name("go-up-symbolic");
      gtk_image_set_pixel_size(GTK_IMAGE(icon), 12);
      gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(content), icon);
      self->reply_label = GTK_LABEL(gtk_label_new(NULL));
      gtk_widget_set_name(GTK_WIDGET(self->reply_label), "reply_label");
      gtk_label_set_ellipsize(self->reply_label, PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars(self->reply_label, 40);
      gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->reply_label));
      gtk_button_set_child(self->reply_button, content);
      gtk_widget_insert_after(GTK_WIDGET(self->reply_button), GTK_WIDGET(self),
                              GTK_WIDGET(self->sender_label));
    }
    gtk_widget_set_margin_start(GTK_WIDGET(self->reply_button), outgoing ? 0 : inset);
    gtk_widget_set_margin_end(GTK_WIDGET(self->reply_button), outgoing ? inset : 0);
    /* TRANSLATORS: "Replying to" header above a message bubble. The
     * replacement is an abbreviated event id since we don't fetch the
     * reply target's author (charter PT-8). */
    gsize len = strlen(reply_to);
    g_autofree gchar *short_id = len > 16 ? g_strdup_printf("%.8s…%.4s", reply_to, reply_to + len - 4)
                                          : g_strdup(reply_to);
    g_autofree gchar *label = g_strdup_printf(_("Replying to %s"), short_id);
    gtk_label_set_text(self->reply_label, label);
    /* Set the action target before the action name so GTK doesn't warn
     * about a NULL target when the action expects "s". */
    gtk_actionable_set_action_target(GTK_ACTIONABLE(self->reply_button), "s", reply_to);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(self->reply_button),
                                   "conversation.scroll-to-reply");
    gtk_widget_set_halign(GTK_WIDGET(self->reply_button), align);
    /* TRANSLATORS: accessible description for the reply header. */
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->reply_button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  } else if (self->reply_button) {
    gtk_widget_unparent(GTK_WIDGET(self->reply_button));
    self->reply_button = NULL;
    self->reply_label = NULL;
  }

  set_class(GTK_WIDGET(self->body_label), "groundhog-undecryptable", self->undecryptable);
  /* nostrc-xrza: withdrawn when the group resolved a conflict -- marked,
   * never shown as the text (or the files) other members did not see. */
  gboolean withdrawn = message && !self->undecryptable && gh_message_get_withdrawn(message);
  set_class(GTK_WIDGET(self->body_label), "groundhog-withdrawn", withdrawn);
  guint cards = message && !self->undecryptable && !withdrawn ? card_count(message) : 0;
  gboolean file_message = cards > 0;

#ifdef GROUNDHOG_HAVE_VOICE
  /* W27 voice (nostrc-h4mk): a single audio attachment shows as a voice
   * bubble, not as an attachment card. */
  gboolean voice = file_message && is_voice_message(message);
  if (voice) {
    if (!self->voice_bubble) {
      self->voice_bubble = gh_voice_bubble_new();
      gtk_box_append(self->voice_slot, self->voice_bubble);
    }
    gh_voice_bubble_set_index(GH_VOICE_BUBBLE(self->voice_bubble), 0);
    gh_voice_bubble_set_message(GH_VOICE_BUBBLE(self->voice_bubble), message);
    gh_voice_bubble_set_compact(GH_VOICE_BUBBLE(self->voice_bubble), self->compact);
  } else if (self->voice_bubble) {
    gh_voice_bubble_set_message(GH_VOICE_BUBBLE(self->voice_bubble), NULL);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->voice_slot), voice);
  /* Voice messages show the bubble, not the attachment card. */
  if (voice) {
    update_cards(self, NULL, 0);
    file_message = FALSE;  /* no attachment_slot for voice */
  } else {
    update_cards(self, file_message ? message : NULL, cards);
  }
#else
  update_cards(self, file_message ? message : NULL, cards);
#endif

  const gchar *caption = message ? gh_message_get_content(message) : NULL;
#ifdef GROUNDHOG_HAVE_VOICE
  gboolean show_body = withdrawn || (!file_message && !voice) ||
                       (gh_message_get_kind(message) != GH_NIP17_FILE_KIND && !voice &&
                        caption && *caption);
#else
  gboolean show_body = withdrawn || !file_message ||
                       (gh_message_get_kind(message) != GH_NIP17_FILE_KIND && caption && *caption);
#endif
  gtk_widget_set_visible(GTK_WIDGET(self->attachment_slot), file_message ||
                         (message && !self->undecryptable && !withdrawn &&
                          gh_message_get_rejected_attachments(message) > 0));
  /* Row enricher (W26 polls): the conversation view's enricher can inject
   * or remove a poll card based on the message kind, without the message
   * row knowing about MLS poll types. */
  if (self->view)
    gh_conversation_view_enrich_row(self->view, self);
  /* Responses update their poll card; they are not standalone bubbles. */
  gtk_widget_set_visible(GTK_WIDGET(self),
                         !message || gh_message_get_kind(message) != GH_MESSAGE_MLS_POLL_VOTE_KIND);
  /* Poll card: visible only when a poll widget has been set on this row. */
  gboolean is_poll = self->poll_card != NULL;
  gtk_widget_set_visible(GTK_WIDGET(self->poll_slot), is_poll);
  if (is_poll) show_body = FALSE;
  gtk_widget_set_visible(GTK_WIDGET(self->body_label), show_body);
  if (!message) {
    gtk_label_set_text(self->body_label, "");
    gtk_label_set_text(self->sender_label, "");
    gtk_label_set_text(self->time_label, "");
  } else {
    g_autofree gchar *file = self->undecryptable || withdrawn || show_body ? NULL : file_text(message);
    if (self->undecryptable) {
      gtk_label_set_text(self->body_label, _("Unable to decrypt yet"));
    } else if (withdrawn) {
      gtk_label_set_text(self->body_label, gh_message_withdrawn_text());
    } else if (file) {
      gtk_label_set_text(self->body_label, file);
    } else {
      g_autofree gchar *markup = gh_link_policy_to_mention_markup(
        gh_message_get_content(message), gh_message_get_account(message));
      gtk_label_set_markup(self->body_label, markup);
      self->preview_uri = gh_link_policy_dup_preview_uri(gh_message_get_content(message));
    }
    g_autofree gchar *sender = gh_message_row_sender_name(message);
    gtk_label_set_text(self->sender_label, sender);
    gint64 created = gh_message_get_created_at(message);
    g_autofree gchar *time = gh_conversation_row_format_time_of_day(created);
    gtk_label_set_text(self->time_label, time);
    g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(created);
    g_autofree gchar *full = when ? g_date_time_format(when, "%c") : NULL;
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->time_label), full);
    const gchar *rumor = gh_message_get_rumor_id(message);
    gtk_actionable_set_action_target(GTK_ACTIONABLE(self->retry_button), "s", rumor);
  }
  gh_delivery_indicator_set_message(self->delivery, message);
  update_runs(self);
  update_expiry(self);
  update_meta(self);
  update_preview(self);
  update_summary(self);
}

/* ---- reactions ------------------------------------------------------------------ */

/* W26 slice B (nostrc-191r): relay the reaction bar's toggle to the
 * conversation view's conversation.react action. */
static void
on_reaction_toggled(GhReactionBar *bar, const gchar *emoji, gboolean add, GhMessageRow *self)
{
  (void)bar;
  if (!self->message)
    return;
  const gchar *rumor_id = gh_message_get_rumor_id(self->message);
  if (!rumor_id)
    return;
  gtk_widget_activate_action(GTK_WIDGET(self), "conversation.react", "(ssb)",
                             rumor_id, emoji, add);
}

/* W26 slice B (nostrc-191r): the reaction picker (right-click / long-press /
 * the "+" button on the reaction bar). The picker is a GtkPopover parented
 * to the bubble. Its emoji-picked signal fires conversation.react (add). */
static void
on_emoji_picked(GhReactionPicker *picker, const gchar *emoji, GhMessageRow *self)
{
  (void)picker;
  if (!self->message)
    return;
  const gchar *rumor_id = gh_message_get_rumor_id(self->message);
  if (!rumor_id)
    return;
  gtk_widget_activate_action(GTK_WIDGET(self), "conversation.react", "(ssb)",
                             rumor_id, emoji, TRUE);
}

static void
ensure_picker(GhMessageRow *self)
{
  if (!self->picker) {
    self->picker = GH_REACTION_PICKER(gh_reaction_picker_new());
    gtk_widget_set_parent(GTK_WIDGET(self->picker), GTK_WIDGET(self->bubble));
    g_signal_connect(self->picker, "emoji-picked", G_CALLBACK(on_emoji_picked), self);
  }
}

static void
show_picker(GhMessageRow *self, gdouble x, gdouble y)
{
  ensure_picker(self);
  if (x >= 0 && y >= 0) {
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(self->picker), &rect);
  }
  gtk_popover_popup(GTK_POPOVER(self->picker));
}

static void
on_secondary_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                     GhMessageRow *self)
{
  (void)n_press;
  show_picker(self, x, y);
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_long_pressed(GtkGestureLongPress *gesture, gdouble x, gdouble y, GhMessageRow *self)
{
  show_picker(self, x, y);
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* ---- links and the enclosing view ------------------------------------------------ */

/* Every click on a link goes through the view's policy; GTK's default
 * handler, which would open any URI, never runs. */
static gboolean
on_activate_link(GhMessageRow *self, const gchar *uri)
{
  gtk_widget_activate_action(GTK_WIDGET(self), "conversation.open-link", "s", uri);
  return TRUE;
}

/* rumor_id NULL: every row (the fetcher came or went). */
static void
on_preview_changed(GhMessageRow *self, const gchar *rumor_id)
{
  (void)rumor_id;
  if (self->message) update_preview(self);
}

/* A name arrived (W33): the sender line, avatar and summary follow. */
static void
on_display_name_changed(GhMessageRow *self, const gchar *pubkey)
{
  if (!self->message)
    return;
  if (g_strcmp0(gh_message_get_sender(self->message), pubkey) == 0) {
    update_all(self);
    return;
  }
  g_auto(GStrv) mentions = gh_message_extract_mentions(gh_message_get_content(self->message));
  if (mentions && g_strv_contains((const gchar *const *)mentions, pubkey)) {
    g_autofree gchar *markup = gh_link_policy_to_mention_markup(
      gh_message_get_content(self->message), gh_message_get_account(self->message));
    gtk_label_set_markup(self->body_label, markup);
  }
}

static void
gh_message_row_root(GtkWidget *widget)
{
  GhMessageRow *self = GH_MESSAGE_ROW(widget);
  GTK_WIDGET_CLASS(gh_message_row_parent_class)->root(widget);
  GtkWidget *view = gtk_widget_get_ancestor(widget, GH_TYPE_CONVERSATION_VIEW);
  if (!view)
    return;
  self->view = GH_CONVERSATION_VIEW(view);
  /* Rows are bound before GTK roots them. Install the poll card now that the
   * conversation view (and its enricher) is available. */
  gh_conversation_view_enrich_row(self->view, self);
  gtk_widget_set_visible(GTK_WIDGET(self->poll_slot), self->poll_card != NULL);
  if (self->poll_card)
    gtk_widget_set_visible(GTK_WIDGET(self->body_label), FALSE);
  self->compact_binding = g_object_bind_property(view, "compact", self, "compact",
                                                 G_BINDING_SYNC_CREATE);
  g_signal_connect_object(gh_display_name_get_notifier(), "changed",
                          G_CALLBACK(on_display_name_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(view, "preview-changed", G_CALLBACK(on_preview_changed), self,
                          G_CONNECT_SWAPPED);
  update_preview(self);
}

static void
gh_message_row_unroot(GtkWidget *widget)
{
  GhMessageRow *self = GH_MESSAGE_ROW(widget);
  if (self->view) {
    g_signal_handlers_disconnect_by_func(self->view, on_preview_changed, self);
    g_clear_pointer(&self->compact_binding, g_binding_unbind);
    self->view = NULL;
  }
  GTK_WIDGET_CLASS(gh_message_row_parent_class)->unroot(widget);
}

/* ---- public ------------------------------------------------------------------- */

GtkWidget *
gh_message_row_new(void)
{
  return g_object_new(GH_TYPE_MESSAGE_ROW, NULL);
}

void
gh_message_row_set_message(GhMessageRow *self, GhMessage *message)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  g_return_if_fail(!message || GH_IS_MESSAGE(message));
  if (self->message == message)
    return;
  if (self->message) {
    g_signal_handlers_disconnect_by_data(self->message, self);
    clear_web_controls(self);
    clear_preview_controls(self);
  }
  if (self->reaction_summary)
    gh_message_row_set_reaction_summary(self, NULL);
  if (self->poll_card)
    gh_message_row_set_poll_widget(self, NULL);
  g_set_object(&self->message, message);
  if (message) {
    g_signal_connect_object(message, "notify::status", G_CALLBACK(update_status), self,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(message, "notify::expires-at", G_CALLBACK(update_all), self,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(message, "notify::withdrawn", G_CALLBACK(update_all), self,
                            G_CONNECT_SWAPPED);
  }
#ifdef GROUNDHOG_CONVERSATION_OPEN_PROBE
  gint64 bind_start_us = message ? g_get_monotonic_time() : 0;
#endif
  update_all(self);
#ifdef GROUNDHOG_CONVERSATION_OPEN_PROBE
  if (message) {
    gint64 bind_elapsed_us = g_get_monotonic_time() - bind_start_us;
    guint widgets = 0;
    GQueue queue = G_QUEUE_INIT;
    g_queue_push_tail(&queue, self);
    while (!g_queue_is_empty(&queue)) {
      GtkWidget *widget = g_queue_pop_head(&queue);
      widgets++;
      for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
           child = gtk_widget_get_next_sibling(child))
        g_queue_push_tail(&queue, child);
    }
    guint optional = 0;
    if (self->has_reply) optional |= GH_OPEN_REPLY;
    if (self->attachment_slot && gtk_widget_get_visible(GTK_WIDGET(self->attachment_slot)))
      optional |= GH_OPEN_ATTACHMENT;
    if (gh_message_get_kind(message) == GH_MESSAGE_MLS_POLL_KIND)
      optional |= GH_OPEN_POLL;
    if (self->preview_uri) optional |= GH_OPEN_LINK;
#ifdef GROUNDHOG_HAVE_VOICE
    if (self->voice_bubble && gtk_widget_get_visible(GTK_WIDGET(self->voice_slot)))
      optional |= GH_OPEN_AUDIO;
#endif
    GhReactionSummary *reactions = self->reaction_summary;
    if (reactions && gh_reaction_summary_get_total_count(reactions))
      optional |= GH_OPEN_REACTION;
    gh_conversation_open_probe_bind(self, bind_elapsed_us, widgets, optional);
  }
#endif
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_MESSAGE]);
}

GhMessage *
gh_message_row_get_message(GhMessageRow *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE_ROW(self), NULL);
  return self->message;
}

static void
set_flag(GhMessageRow *self, gboolean *flag, gboolean value, guint prop)
{
  if (*flag == !!value)
    return;
  *flag = !!value;
  g_object_notify_by_pspec(G_OBJECT(self), props[prop]);
}

void
gh_message_row_set_run(GhMessageRow *self, gboolean run_start, gboolean run_end)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  set_flag(self, &self->run_start, run_start, PROP_RUN_START);
  set_flag(self, &self->run_end, run_end, PROP_RUN_END);
  update_runs(self);
  update_meta(self);
}

void
gh_message_row_set_show_sender(GhMessageRow *self, gboolean show_sender)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  set_flag(self, &self->show_sender, show_sender, PROP_SHOW_SENDER);
  update_runs(self);
}

void
gh_message_row_set_compact(GhMessageRow *self, gboolean compact)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  set_flag(self, &self->compact, compact, PROP_COMPACT);
  update_width(self);
}

void
gh_message_row_set_undecryptable(GhMessageRow *self, gboolean undecryptable)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  if (self->undecryptable == !!undecryptable)
    return;
  self->undecryptable = !!undecryptable;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_UNDECRYPTABLE]);
  update_all(self);
}

const gchar *
gh_message_row_get_summary(GhMessageRow *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE_ROW(self), NULL);
  return self->summary;
}

void
gh_message_row_set_reaction_summary(GhMessageRow *self, GhReactionSummary *summary)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  if (self->reaction_summary != summary) {
    if (self->reaction_summary)
      g_signal_handlers_disconnect_by_func(self->reaction_summary,
                                           G_CALLBACK(on_reaction_total_changed), self);
    g_set_object(&self->reaction_summary, summary);
    if (summary)
      g_signal_connect_object(summary, "notify::total-count",
                              G_CALLBACK(on_reaction_total_changed), self, 0);
    if (self->reaction_bar)
      gh_reaction_bar_set_summary(self->reaction_bar, summary);
  }
  sync_reaction_bar(self);
  update_meta(self);
}

static void
gh_message_row_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhMessageRow *self = GH_MESSAGE_ROW(object);
  switch (id) {
  case PROP_MESSAGE:
    g_value_set_object(value, self->message);
    break;
  case PROP_RUN_START:
    g_value_set_boolean(value, self->run_start);
    break;
  case PROP_RUN_END:
    g_value_set_boolean(value, self->run_end);
    break;
  case PROP_SHOW_SENDER:
    g_value_set_boolean(value, self->show_sender);
    break;
  case PROP_COMPACT:
    g_value_set_boolean(value, self->compact);
    break;
  case PROP_UNDECRYPTABLE:
    g_value_set_boolean(value, self->undecryptable);
    break;
  case PROP_SUMMARY:
    g_value_set_string(value, self->summary);
    break;
  case PROP_REACTION_SUMMARY:
    g_value_set_object(value, NULL); /* write-only in practice */
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_message_row_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  GhMessageRow *self = GH_MESSAGE_ROW(object);
  switch (id) {
  case PROP_MESSAGE:
    gh_message_row_set_message(self, g_value_get_object(value));
    break;
  case PROP_RUN_START:
    gh_message_row_set_run(self, g_value_get_boolean(value), self->run_end);
    break;
  case PROP_RUN_END:
    gh_message_row_set_run(self, self->run_start, g_value_get_boolean(value));
    break;
  case PROP_SHOW_SENDER:
    gh_message_row_set_show_sender(self, g_value_get_boolean(value));
    break;
  case PROP_COMPACT:
    gh_message_row_set_compact(self, g_value_get_boolean(value));
    break;
  case PROP_UNDECRYPTABLE:
    gh_message_row_set_undecryptable(self, g_value_get_boolean(value));
    break;
  case PROP_REACTION_SUMMARY:
    gh_message_row_set_reaction_summary(self, g_value_get_object(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_message_row_dispose(GObject *object)
{
  GhMessageRow *self = GH_MESSAGE_ROW(object);
#ifdef GROUNDHOG_CONVERSATION_OPEN_PROBE
  gint64 dispose_start_us = g_get_monotonic_time();
#endif
  if (self->message)
    g_signal_handlers_disconnect_by_data(self->message, self);
  if (self->reaction_summary)
    g_signal_handlers_disconnect_by_func(self->reaction_summary,
                                         G_CALLBACK(on_reaction_total_changed), self);
  g_clear_object(&self->reaction_summary);
  g_clear_object(&self->message);
#ifdef GROUNDHOG_HAVE_VOICE
  /* The voice bubble is the voice_slot's child: it goes with the template. */
  self->voice_bubble = NULL;
#endif
  if (self->picker) {
    gtk_widget_unparent(GTK_WIDGET(self->picker));
    self->picker = NULL;
  }
  if (self->reply_button) {
    gtk_widget_unparent(GTK_WIDGET(self->reply_button));
    self->reply_button = NULL;
    self->reply_label = NULL;
  }
  /* The extra cards are the slot's children: they go with the template. */
  g_clear_pointer(&self->extra_cards, g_ptr_array_unref);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_MESSAGE_ROW);
  G_OBJECT_CLASS(gh_message_row_parent_class)->dispose(object);
#ifdef GROUNDHOG_CONVERSATION_OPEN_PROBE
  gh_conversation_open_probe_dispose(g_get_monotonic_time() - dispose_start_us);
#endif
}

static void
gh_message_row_finalize(GObject *object)
{
  GhMessageRow *self = GH_MESSAGE_ROW(object);
  g_free(self->preview_uri);
  g_free(self->summary);
  G_OBJECT_CLASS(gh_message_row_parent_class)->finalize(object);
}

static void
action_add_reaction(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhMessageRow *self = GH_MESSAGE_ROW(widget);
  /* From the react button: the picker points at it, not at the whole
   * message (W33, owner). */
  graphene_rect_t bounds;
  if (gtk_widget_get_mapped(GTK_WIDGET(self->react_button)) &&
      gtk_widget_compute_bounds(GTK_WIDGET(self->react_button), GTK_WIDGET(self->bubble), &bounds)) {
    ensure_picker(self);
    GdkRectangle rect = { (int)bounds.origin.x, (int)bounds.origin.y,
                          MAX(1, (int)bounds.size.width), MAX(1, (int)bounds.size.height) };
    gtk_popover_set_pointing_to(GTK_POPOVER(self->picker), &rect);
    gtk_popover_popup(GTK_POPOVER(self->picker));
    return;
  }
  show_picker(self, -1, -1);
}

static void
gh_message_row_class_init(GhMessageRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->get_property = gh_message_row_get_property;
  object_class->set_property = gh_message_row_set_property;
  object_class->dispose = gh_message_row_dispose;
  object_class->finalize = gh_message_row_finalize;
  widget_class->root = gh_message_row_root;
  widget_class->unroot = gh_message_row_unroot;

  const GParamFlags rw = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  props[PROP_MESSAGE] = g_param_spec_object("message", NULL, NULL, GH_TYPE_MESSAGE, rw);
  props[PROP_RUN_START] = g_param_spec_boolean("run-start", NULL, NULL, TRUE, rw);
  props[PROP_RUN_END] = g_param_spec_boolean("run-end", NULL, NULL, TRUE, rw);
  props[PROP_SHOW_SENDER] = g_param_spec_boolean("show-sender", NULL, NULL, FALSE, rw);
  props[PROP_COMPACT] = g_param_spec_boolean("compact", NULL, NULL, FALSE, rw);
  props[PROP_UNDECRYPTABLE] = g_param_spec_boolean("undecryptable", NULL, NULL, FALSE, rw);
  props[PROP_SUMMARY] = g_param_spec_string("summary", NULL, NULL, "",
    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_REACTION_SUMMARY] = g_param_spec_object("reaction-summary", NULL, NULL,
    GH_TYPE_REACTION_SUMMARY, rw);
  g_object_class_install_properties(object_class, N_PROPS, props);

  /* W26 slice B: the "+" button in the reaction bar activates this action;
   * the message row shows its picker. */
  gtk_widget_class_install_action(widget_class, "conversation.add-reaction", NULL,
                                  action_add_reaction);

  g_type_ensure(GH_TYPE_DELIVERY_INDICATOR);
  g_type_ensure(GH_TYPE_ATTACHMENT_CARD);
#ifdef GROUNDHOG_HAVE_VOICE
  g_type_ensure(GH_TYPE_VOICE_BUBBLE);
#endif

  g_type_ensure(GH_TYPE_REACTION_BAR);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-message-row.ui");
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, sender_label);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, bubble);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, body_label);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, reaction_controls);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, react_button);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, preview_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, bubble_line);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, avatar);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, web_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, meta_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, timer_icon);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, time_label);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, delivery);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, retry_button);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, voice_slot);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, attachment_slot);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, poll_slot);
  gtk_widget_class_set_css_name(widget_class, "groundhog-message");
}

static void
gh_message_row_init(GhMessageRow *self)
{
#ifdef GROUNDHOG_CONVERSATION_OPEN_PROBE
  gint64 construct_start_us = g_get_monotonic_time();
#endif
  gtk_widget_init_template(GTK_WIDGET(self));
  self->extra_cards = g_ptr_array_new();
  /* The row's actions take the message's rumor id: a row set up (or rooted
   * again, e.g. when the window collapses) before a message is bound names
   * none, so GTK never meets a target-less "s" action. */
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->retry_button), "s", "");
  self->run_start = TRUE;
  /* Right-click and long-press on the bubble show the reaction picker. */
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_secondary_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self->bubble), GTK_EVENT_CONTROLLER(click));
  GtkGesture *hold = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(hold), TRUE);
  g_signal_connect(hold, "pressed", G_CALLBACK(on_long_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self->bubble), GTK_EVENT_CONTROLLER(hold));
  self->run_end = TRUE;
  self->summary = g_strdup("");
  g_signal_connect_swapped(self->body_label, "activate-link", G_CALLBACK(on_activate_link),
                           self);
  update_width(self);
  update_all(self);
#ifdef GROUNDHOG_CONVERSATION_OPEN_PROBE
  gh_conversation_open_probe_construct(g_get_monotonic_time() - construct_start_us);
#endif
}

void
gh_message_row_set_poll_widget(GhMessageRow *self, GtkWidget *poll_card)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  if (self->poll_card == poll_card) return;
  if (self->poll_card) {
    gtk_box_remove(self->poll_slot, self->poll_card);
    self->poll_card = NULL;
  }
  if (poll_card) {
    self->poll_card = poll_card;
    gtk_box_append(self->poll_slot, poll_card);
    gtk_widget_set_visible(GTK_WIDGET(self->poll_slot), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->body_label), FALSE);
  } else {
    gtk_widget_set_visible(GTK_WIDGET(self->poll_slot), FALSE);
  }
}
