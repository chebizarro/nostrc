#include "gh-message-row.h"
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
  GhReactionBar *reaction_bar;
  GhReactionPicker *picker;    /* W26 slice B: quick-reaction popover */
  GtkBox *preview_box;
  GtkButton *preview_button;
  GtkButton *image_button;
  GtkButton *picture_button;
  GtkPicture *remote_image;
  GtkPicture *profile_picture;
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

/* ---- text -------------------------------------------------------------------- */

gchar *
gh_message_row_display_name(const gchar *pubkey_hex)
{
  g_return_val_if_fail(pubkey_hex != NULL, NULL);
  guint8 bytes[32];
  char *npub = NULL;
  if (strlen(pubkey_hex) != 64 || !nostr_hex2bin(bytes, pubkey_hex, sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return g_strdup(pubkey_hex);
  gsize length = strlen(npub);
  gchar *out = length > 16 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4)
                           : g_strdup(npub);
  free(npub);
  return out;
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
  if (!undecryptable && gh_message_get_reply_to_id(message))
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
  gh_attachment_card_set_compact(self->attachment_card, self->compact);
  for (guint i = 0; self->extra_cards && i < self->extra_cards->len; i++)
    gh_attachment_card_set_compact(g_ptr_array_index(self->extra_cards, i), self->compact);
  gtk_label_set_max_width_chars(self->preview_title, chars);
  gtk_label_set_max_width_chars(self->preview_text, chars);
}

static void
update_runs(GhMessageRow *self)
{
  set_class(GTK_WIDGET(self), "run-start", self->run_start);
  set_class(GTK_WIDGET(self), "run-end", self->run_end);
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
  gtk_widget_set_visible(GTK_WIDGET(self->meta_box), self->run_end || expiring || noteworthy);
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
update_web_images(GhMessageRow *self)
{
  gboolean available = self->message && self->view &&
    gh_conversation_view_has_web_content(self->view) &&
    !gh_message_get_withdrawn(self->message);
  g_autofree gchar *picture = available
    ? gh_conversation_view_dup_picture_uri(self->view, self->message) : NULL;
  gtk_widget_set_visible(GTK_WIDGET(self->image_button), available && self->preview_uri);
  gtk_widget_set_visible(GTK_WIDGET(self->picture_button), picture != NULL);
  gboolean failed = FALSE;
  for (guint i = GH_WEB_IMAGE; i <= GH_WEB_PICTURE; i++) {
    GtkPicture *image = i == GH_WEB_IMAGE ? self->remote_image : self->profile_picture;
    GtkButton *button = i == GH_WEB_IMAGE ? self->image_button : self->picture_button;
    GhLinkPreviewState state = GH_LINK_PREVIEW_NONE;
    GdkTexture *texture = available
      ? gh_conversation_view_get_web_texture(self->view, self->message, i, &state) : NULL;
    gtk_picture_set_paintable(image, texture ? GDK_PAINTABLE(texture) : NULL);
    gtk_widget_set_visible(GTK_WIDGET(image), texture != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(button), state == GH_LINK_PREVIEW_NONE || state == GH_LINK_PREVIEW_FAILED);
    failed |= state == GH_LINK_PREVIEW_FAILED;
    if (self->message) {
      g_autofree gchar *id = g_strconcat(i == GH_WEB_IMAGE ? "image:" : "picture:",
                                        gh_message_get_rumor_id(self->message), NULL);
      gtk_actionable_set_action_target(GTK_ACTIONABLE(button), "s", id);
    }
  }
  gtk_widget_set_visible(GTK_WIDGET(self->web_error), failed);
}

static void
update_preview(GhMessageRow *self)
{
  update_web_images(self);
  /* Only with a fetcher (W13b review, non-blocking #1). */
  gboolean offered = self->message && self->preview_uri && self->view &&
                     gh_conversation_view_get_previews_available(self->view);
  gtk_widget_set_visible(GTK_WIDGET(self->preview_box), offered);
  if (!offered)
    return;
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

/* The first card is the template's; an encrypted group message's further
 * files get cards made here (at most GH_STORE_MAX_MLS_MEDIA, W25), and
 * imeta tags that weren't valid are said, not shown. */
static void
update_cards(GhMessageRow *self, GhMessage *message, guint cards)
{
  gh_attachment_card_set_index(self->attachment_card, 0);
  gh_attachment_card_set_message(self->attachment_card, cards ? message : NULL);
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
  guint rejected = message ? gh_message_get_rejected_attachments(message) : 0;
  if (rejected > 0 && !self->undecryptable) {
    g_autofree gchar *note =
      g_strdup_printf(g_dngettext(NULL, "%u attached file can't be read: it isn't a valid "
                                        "encrypted file reference.",
                                  "%u attached files can't be read: they aren't valid "
                                  "encrypted file references.", rejected), rejected);
    gtk_label_set_text(self->attachment_note, note);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->attachment_note), rejected > 0 &&
                         !self->undecryptable);
  gtk_widget_set_visible(GTK_WIDGET(self->attachment_card), cards > 0);
}

static void
update_all(GhMessageRow *self)
{
  GhMessage *message = self->message;
  gboolean outgoing = message && gh_message_is_self(message);
  set_class(GTK_WIDGET(self->bubble), "outgoing", message && outgoing);
  set_class(GTK_WIDGET(self->bubble), "incoming", message && !outgoing);
  GtkAlign align = outgoing ? GTK_ALIGN_END : GTK_ALIGN_START;
  gtk_widget_set_halign(GTK_WIDGET(self->bubble), align);
  gtk_widget_set_halign(GTK_WIDGET(self->preview_box), align);
  gtk_widget_set_halign(GTK_WIDGET(self->meta_box), align);

  g_clear_pointer(&self->preview_uri, g_free);

  /* nostrc-zjkv: reply header — show "Replying to npub1…" when the message
   * has a reply-target event id. The button's action target is the reply-to
   * id so conversation.scroll-to-reply can find and scroll to it. */
  const gchar *reply_to = message ? gh_message_get_reply_to_id(message) : NULL;
  self->has_reply = reply_to != NULL;
  gtk_widget_set_visible(GTK_WIDGET(self->reply_button), self->has_reply);
  if (self->has_reply) {
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
  } else {
    gtk_actionable_set_action_name(GTK_ACTIONABLE(self->reply_button), NULL);
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
      g_autofree gchar *markup = gh_link_policy_to_markup(gh_message_get_content(message));
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
    gtk_actionable_set_action_target(GTK_ACTIONABLE(self->preview_button), "s", rumor);
    if (self->preview_uri) {
      g_autofree gchar *host = gh_link_policy_dup_host(self->preview_uri);
      g_autofree gchar *tooltip = g_strdup_printf(_("Load a preview from %s"), host);
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->preview_button), tooltip);
    }
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
show_picker(GhMessageRow *self, gdouble x, gdouble y)
{
  if (!self->picker) {
    self->picker = GH_REACTION_PICKER(gh_reaction_picker_new());
    gtk_widget_set_parent(GTK_WIDGET(self->picker), GTK_WIDGET(self->bubble));
    g_signal_connect(self->picker, "emoji-picked", G_CALLBACK(on_emoji_picked), self);
  }
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

static void
gh_message_row_root(GtkWidget *widget)
{
  GhMessageRow *self = GH_MESSAGE_ROW(widget);
  GTK_WIDGET_CLASS(gh_message_row_parent_class)->root(widget);
  GtkWidget *view = gtk_widget_get_ancestor(widget, GH_TYPE_CONVERSATION_VIEW);
  if (!view)
    return;
  self->view = GH_CONVERSATION_VIEW(view);
  self->compact_binding = g_object_bind_property(view, "compact", self, "compact",
                                                 G_BINDING_SYNC_CREATE);
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
  if (self->message)
    g_signal_handlers_disconnect_by_data(self->message, self);
  g_set_object(&self->message, message);
  if (message) {
    g_signal_connect_object(message, "notify::status", G_CALLBACK(update_status), self,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(message, "notify::expires-at", G_CALLBACK(update_all), self,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(message, "notify::withdrawn", G_CALLBACK(update_all), self,
                            G_CONNECT_SWAPPED);
  }
  update_all(self);
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
  gh_reaction_bar_set_summary(self->reaction_bar, summary);
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
  if (self->message)
    g_signal_handlers_disconnect_by_data(self->message, self);
  g_clear_object(&self->message);
#ifdef GROUNDHOG_HAVE_VOICE
  /* The voice bubble is the voice_slot's child: it goes with the template. */
  self->voice_bubble = NULL;
#endif
  if (self->picker) {
    gtk_widget_unparent(GTK_WIDGET(self->picker));
    self->picker = NULL;
  }
  /* The extra cards are the slot's children: they go with the template. */
  g_clear_pointer(&self->extra_cards, g_ptr_array_unref);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_MESSAGE_ROW);
  G_OBJECT_CLASS(gh_message_row_parent_class)->dispose(object);
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
  show_picker(GH_MESSAGE_ROW(widget), -1, -1);
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
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, reply_button);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, reply_label);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, bubble);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, body_label);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, reaction_bar);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, preview_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, preview_button);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, image_button);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, picture_button);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, remote_image);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, profile_picture);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, web_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, web_error);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, preview_title);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, preview_text);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, meta_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, timer_icon);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, time_label);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, delivery);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, retry_button);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, voice_slot);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, attachment_slot);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, attachment_card);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, attachment_note);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, poll_slot);
  gtk_widget_class_set_css_name(widget_class, "groundhog-message");
}

static void
gh_message_row_init(GhMessageRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->extra_cards = g_ptr_array_new();
  /* The row's actions take the message's rumor id: a row set up (or rooted
   * again, e.g. when the window collapses) before a message is bound names
   * none, so GTK never meets a target-less "s" action. */
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->retry_button), "s", "");
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->preview_button), "s", "");
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->image_button), "s", "");
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->picture_button), "s", "");
  self->run_start = TRUE;
  g_signal_connect(self->reaction_bar, "reaction-toggled",
                   G_CALLBACK(on_reaction_toggled), self);
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
