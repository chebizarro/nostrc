#include "gh-message-row.h"
#include "gh-unparent.h"
#include "gh-conversation-open-probe-private.h"
#include "gh-display-name.h"
#include "gh-attachment-card.h"
#include "gh-reaction-bar.h"
#include "gh-reaction-picker.h"
#include "gh-conversation-row.h"
#include "gh-conversation-view.h"
#include "gh-delivery-indicator.h"
#include "gh-link-policy.h"
#include <nostr-gtk-1.0/gn-og-preview-card.h>
#include <nostr-gtk-1.0/gn-media-viewer.h>
#include <nostr-gtk-1.0/gn-animated-image.h>

#ifdef GROUNDHOG_HAVE_VOICE
#include "gh-voice-bubble.h"
#include "gh-voice-meta.h"
#endif

#include <adwaita.h>
#include <glib/gi18n.h>
#include "gh-bidi.h"
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

/* Bubble text width caps (charter §7.6): 60 characters, 32 when compact. */
#define BODY_CHARS 60
#define BODY_CHARS_COMPACT 32
/* A link preview card's width (nostrc-8xfib.3): gnostr's media card, as
 * wide as a full bubble, whatever little the page names. */
#define OG_CARD_WIDTH 380
#define OG_CARD_WIDTH_COMPACT 260
/* A Markdown table's width in characters (nostrc-p15n5.1): its columns
 * share it. Narrower than the body: a column cannot shrink below its width. */
#define TABLE_CHARS 56
#define TABLE_CHARS_COMPACT 27
#define TABLE_CELL_MIN_CHARS 4

struct _GhMessageRow {
  GtkWidget parent_instance;
  GtkLabel *sender_label;
  GtkButton *reply_button;
  GtkLabel *reply_label;
  GtkBox *bubble;
  GtkLabel *body_label;
  /* After body_label, built on first use: a body with a Markdown table is
   * laid out as blocks; body_label shows the text before the first table,
   * this box the tables (grids of cells) and the text after them. */
  GtkBox *body_blocks;
  GtkBox *voice_slot;      /* W27 voice: shown instead of attachment_slot */
  GtkWidget *voice_bubble;  /* GhVoiceBubble, created on demand */
  GtkBox *attachment_slot;
  GhAttachmentCard *attachment_card;
  GtkLabel *attachment_note;
  GPtrArray *extra_cards;   /* an encrypted group message's files 1.. (W25) */
  GtkBox *poll_slot;
  GtkWidget *poll_card;     /* set externally for kind-1068 poll messages */
  GhReactionBar *reaction_bar; /* on the meta line, only while there are reactions */
  GhReactionSummary *reaction_summary; /* owned even while the bar is absent */
  GtkButton *react_button;
  GhReactionPicker *picker;
  GtkPopoverMenu *menu;        /* the bubble's menu, built anew for each popup */
  gchar *menu_selection;       /* the selected text when the menu opened, or NULL */    /* W26 slice B: quick-reaction popover */
  gchar *menu_link;            /* Copy Link's address in that menu, or NULL */
  GtkBox *preview_box;
  /* Built on first use (ensure_reference); NULL for most rows. */
  GtkBox *reference_box;
  GnNostrReferenceCard *reference_card;
  GtkButton *reference_copy_button;
  GtkButton *share_reference_button;
  GtkBox *public_actions;
  GtkButton *repost_reference_button;
  GtkButton *quote_reference_button;
  GtkButton *preview_button;
  GtkButton *image_button;
  GtkPicture *remote_image;
  GtkWidget *bubble_line;
  GtkWidget *legacy_box;
  AdwAvatar *avatar;
  GtkPopover *avatar_menu;
  GtkBox *web_box;
  GtkLabel *web_error;
  GtkLabel *preview_title;
  GtkLabel *preview_text;
  GnOgPreviewCard *og_card;
  GtkBox *meta_box;
  GtkBox *status_box;
  GtkImage *timer_icon;
  GtkLabel *time_label;
  GhDeliveryIndicator *delivery;
  GtkButton *retry_button;

  GhMessage *message;
  GhConversationView *view; /* the enclosing view while rooted; not a reference */
  GBinding *compact_binding;
  gchar *preview_uri;       /* the first page link (a preview card), or NULL */
  gchar *image_uri;         /* the first image link (nostrc-p15n5.4), or NULL */
  gchar *summary;
  gboolean run_start;
  gboolean run_end;
  gboolean show_sender;
  gboolean compact;
  gboolean undecryptable;
  GnNostrReferenceCardRole reference_role;
  gboolean body_hidden_for_repost; /* the card shows the reposted note */
  gboolean has_reply;   /* nostrc-zjkv: the message has a reply_to_id */
  gint meta_side;       /* the side the meta line is arranged for: 0 incoming, 1 own */
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
  /* A repost or quote reads as what it shares, never its JSON (nostrc-tv98u). */
  g_autofree gchar *quote_text = NULL;
  gboolean repost = !undecryptable && !withdrawn && !file &&
    gh_conversation_view_parse_repost_body(gh_message_get_content(message), &quote_text, NULL);
  const gchar *body = undecryptable ? _("Unable to decrypt yet")
                      : withdrawn   ? gh_message_withdrawn_text()
                      : file        ? file
                      : quote_text  ? quote_text
                      : repost      ? _("Reposted Nostr note")
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

static void refresh_body_markup(GhMessageRow *self);
static void add_menu_gestures(GhMessageRow *self, GtkWidget *area);

static void
update_width(GhMessageRow *self)
{
  gint chars = self->compact ? BODY_CHARS_COMPACT : BODY_CHARS;
  gtk_label_set_max_width_chars(self->body_label, chars);
  if (self->body_blocks && gtk_widget_get_first_child(GTK_WIDGET(self->body_blocks)))
    refresh_body_markup(self);
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
  if (self->og_card)
    gtk_widget_set_size_request(GTK_WIDGET(self->og_card),
                                self->compact ? OG_CARD_WIDTH_COMPACT : OG_CARD_WIDTH, -1);
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
  g_autofree gchar *accessible = g_strdup_printf(_("Profile picture of %s"),
                                                  name ? name : _("sender"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->avatar),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, accessible,
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                 _("Open the picture menu with right-click, long press, or Menu key"),
                                 -1);
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
    gtk_box_insert_child_after(self->status_box, box, GTK_WIDGET(self->timer_icon));
  } else if (!legacy && self->legacy_box) {
    gtk_box_remove(self->status_box, self->legacy_box);
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

/* The meta line (nostrc-l1kn6.5): one order, mirrored by side, so that the
 * status is at the bubble's edge, the chips next to it and React outermost:
 *   incoming  [time][Try Again][chips…][React]
 *   own       [React][chips…][Try Again][time][receipt]
 * The status reads the same on both sides (gh-message-row.blp). Rearranged
 * only when the side or the presence of chips changes. */
static void
arrange_meta(GhMessageRow *self, gboolean outgoing, gboolean force)
{
  if (!force && self->meta_side == (outgoing ? 1 : 0))
    return;
  self->meta_side = outgoing ? 1 : 0;
  GtkWidget *order[4];
  guint n = 0;
  order[n++] = GTK_WIDGET(self->status_box);
  order[n++] = GTK_WIDGET(self->retry_button);
  if (self->reaction_bar)
    order[n++] = GTK_WIDGET(self->reaction_bar);
  order[n++] = GTK_WIDGET(self->react_button);
  GtkWidget *previous = NULL;
  for (guint i = 0; i < n; i++) {
    GtkWidget *widget = order[outgoing ? n - 1 - i : i];
    if (gtk_widget_get_prev_sibling(widget) != previous)
      gtk_box_reorder_child_after(self->meta_box, widget, previous);
    previous = widget;
  }
}

static void
sync_reaction_bar(GhMessageRow *self)
{
  gboolean needed = self->reaction_summary &&
    gh_reaction_summary_get_total_count(self->reaction_summary) > 0;
  if (needed && !self->reaction_bar) {
    self->reaction_bar = GH_REACTION_BAR(gh_reaction_bar_new());
    gtk_widget_set_name(GTK_WIDGET(self->reaction_bar), "reaction_bar");
    gtk_widget_set_valign(GTK_WIDGET(self->reaction_bar), GTK_ALIGN_CENTER);
    gtk_box_append(self->meta_box, GTK_WIDGET(self->reaction_bar));
    g_signal_connect(self->reaction_bar, "reaction-toggled",
                     G_CALLBACK(on_reaction_toggled), self);
    gh_reaction_bar_set_summary(self->reaction_bar, self->reaction_summary);
    arrange_meta(self, self->message && gh_message_is_self(self->message), TRUE);
  } else if (!needed && self->reaction_bar) {
    gh_reaction_bar_set_summary(self->reaction_bar, NULL);
    gtk_box_remove(self->meta_box, GTK_WIDGET(self->reaction_bar));
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
  self->og_card = NULL;
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
on_remote_image_pressed(GtkGestureClick *gesture, gint n_press, gdouble x,
                        gdouble y, GhMessageRow *self)
{
  (void)gesture; (void)n_press; (void)x; (void)y;
  if (!self->view || !self->message || !self->image_uri) return;
  GhLinkPreviewState state = GH_LINK_PREVIEW_NONE;
  GdkTexture *texture = gh_conversation_view_get_web_texture(self->view, self->message,
                                                               GH_WEB_IMAGE, &state);
  if (state != GH_LINK_PREVIEW_LOADED || !texture) return;
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GnMediaViewer *viewer = gn_media_viewer_new(GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL);
  const gchar *urls[] = { self->image_uri, NULL };
  gn_media_viewer_set_gallery(viewer, urls, 0);
  gn_media_viewer_set_texture(viewer, 0, texture);
  gtk_window_present(GTK_WINDOW(viewer));
}

static gboolean
on_remote_image_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                    GdkModifierType state, GhMessageRow *self)
{
  (void)controller; (void)keycode; (void)state;
  if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter && keyval != GDK_KEY_space)
    return FALSE;
  on_remote_image_pressed(NULL, 1, 0, 0, self);
  return TRUE;
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
    if (self->image_uri)
      gh_conversation_view_auto_load(self->view, self->message, GH_WEB_IMAGE);
    if (picture) gh_conversation_view_auto_load(self->view, self->message, GH_WEB_PICTURE);
  }
  GhLinkPreviewState image_state = GH_LINK_PREVIEW_NONE;
  GdkTexture *texture = available
    ? gh_conversation_view_get_web_texture(self->view, self->message, GH_WEB_IMAGE,
                                           &image_state) : NULL;
  /* Only an image link is a linked image; a page link has a preview card. */
  gboolean has_image = available && self->image_uri;
  gboolean ask_image = has_image &&
    image_state != GH_LINK_PREVIEW_LOADED && image_state != GH_LINK_PREVIEW_LOADING;
  gboolean failed = image_state == GH_LINK_PREVIEW_FAILED;
  if (!has_image && !texture && !failed) {
    clear_web_controls(self);
    update_avatar(self);
    return;
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
                                     _("Open image viewer for linked image"), -1);
      gtk_widget_set_focusable(GTK_WIDGET(self->remote_image), TRUE);
      GtkGesture *open = gtk_gesture_click_new();
      gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(open), GDK_BUTTON_PRIMARY);
      g_signal_connect(open, "pressed", G_CALLBACK(on_remote_image_pressed), self);
      gtk_widget_add_controller(GTK_WIDGET(self->remote_image), GTK_EVENT_CONTROLLER(open));
      GtkEventController *keys = gtk_event_controller_key_new();
      g_signal_connect(keys, "key-pressed", G_CALLBACK(on_remote_image_key), self);
      gtk_widget_add_controller(GTK_WIDGET(self->remote_image), keys);
      gtk_box_append(self->web_box, GTK_WIDGET(self->remote_image));
    }
    /* nostrc-p15n5.8: an animated GIF plays while the row is on screen. */
    GdkPaintable *shown = gn_animated_image_paintable_for_texture(texture);
    gtk_picture_set_paintable(self->remote_image, shown);
    gn_animated_image_attach(GTK_WIDGET(self->remote_image), shown);
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
on_og_image_requested(GnOgPreviewCard *card, const gchar *url, GhMessageRow *self)
{
  (void)card;
  if (!self->view || !self->message ||
      g_strcmp0(url, gh_conversation_view_get_og_image_uri(self->view, self->message)) != 0)
    return;
  g_autofree gchar *target = g_strconcat("og-image:",
                                         gh_message_get_rumor_id(self->message), NULL);
  gtk_widget_activate_action(GTK_WIDGET(self), "conversation.show-preview", "s", target);
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
    /* Not "Show Preview" again (nostrc-p15n5.4): the user already asked. */
    button = _("Try Again");
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
  if (state == GH_LINK_PREVIEW_LOADED) {
    if (!self->og_card) {
      self->og_card = gn_og_preview_card_new();
      gtk_widget_set_name(GTK_WIDGET(self->og_card), "og_card");
      /* gnostr's card: artwork at the start, then title, description and
       * site (owner report: the stacked card was a tiny box). */
      gn_og_preview_card_set_layout(self->og_card, GN_OG_PREVIEW_LAYOUT_COMPACT);
      gtk_widget_set_size_request(GTK_WIDGET(self->og_card),
                                  self->compact ? OG_CARD_WIDTH_COMPACT : OG_CARD_WIDTH, -1);
      gtk_box_append(self->preview_box, GTK_WIDGET(self->og_card));
      g_signal_connect(self->og_card, "image-load-requested",
                       G_CALLBACK(on_og_image_requested), self);
    }
    if (g_strcmp0(gn_og_preview_card_get_url(self->og_card), self->preview_uri) != 0)
      gn_og_preview_card_set_url(self->og_card, self->preview_uri);
    const gchar *site_name = gh_conversation_view_get_link_preview_site(self->view, self->message);
    g_autofree gchar *host = gh_link_policy_dup_host(self->preview_uri);
    g_autofree gchar *site = site_name && host && g_ascii_strcasecmp(site_name, host) != 0
      ? g_strdup_printf("%s · %s", site_name, host) : g_strdup(site_name ? site_name : host);
    gn_og_preview_card_set_result(self->og_card, title, description, site,
      gh_conversation_view_get_og_image_uri(self->view, self->message));
    GhLinkPreviewState image_state = GH_LINK_PREVIEW_NONE;
    GdkTexture *artwork = gh_conversation_view_get_web_texture(self->view, self->message,
                                                                GH_WEB_OG_IMAGE, &image_state);
    gn_og_preview_card_set_image_texture(self->og_card,
      image_state == GH_LINK_PREVIEW_LOADED ? artwork : NULL);
    gtk_widget_set_visible(GTK_WIDGET(self->preview_title), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->preview_text), FALSE);
  } else if (self->og_card) {
    gtk_box_remove(self->preview_box, GTK_WIDGET(self->og_card));
    self->og_card = NULL;
  }
  /* The card draws its own frame; the box frames only the other states'
   * text. */
  set_class(GTK_WIDGET(self->preview_box), "card", FALSE);
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

/* Repost and Quote… need the note in the verified local cache (the
 * card may also have resolved from a repost's embedded original). */
/* A resolved repost card replaces the body line (nostrc-boq9.5), as
 * gnostr's kind-6 rows show the original instead of the repost's JSON.
 * Only a line this hid is shown again; update_all() decides the rest. */
static void
update_repost_body(GhMessageRow *self)
{
  gboolean hide = self->reference_card && self->message &&
    gtk_widget_get_visible(GTK_WIDGET(self->reference_box)) &&
    self->reference_role == GN_NOSTR_REFERENCE_CARD_ROLE_REPOST &&
    gn_nostr_reference_card_get_state(self->reference_card) == GN_NOSTR_REFERENCE_CARD_RESOLVED;
  if (hide && !self->body_hidden_for_repost &&
      gtk_widget_get_visible(GTK_WIDGET(self->body_label))) {
    gtk_widget_set_visible(GTK_WIDGET(self->body_label), FALSE);
    self->body_hidden_for_repost = TRUE;
  } else if (!hide && self->body_hidden_for_repost) {
    gtk_widget_set_visible(GTK_WIDGET(self->body_label), TRUE);
    self->body_hidden_for_repost = FALSE;
  }
}

static void
update_public_actions(GhMessageRow *self)
{
  update_repost_body(self);
  if (!self->reference_card)
    return;
  const GnNostrReference *reference = gn_nostr_reference_card_get_reference(self->reference_card);
  GnNostrReferenceResolver *resolver = self->view
    ? gh_conversation_view_get_reference_resolver(self->view) : NULL;
  g_autofree gchar *local = reference && resolver &&
    reference->type != GN_NOSTR_REFERENCE_PERSON &&
    gn_nostr_reference_card_get_state(self->reference_card) == GN_NOSTR_REFERENCE_CARD_RESOLVED
      ? gn_nostr_reference_resolver_lookup_local(resolver, reference) : NULL;
  gtk_widget_set_visible(GTK_WIDGET(self->public_actions), local != NULL);
}

/* The reference box (gh-message-reference.blp) is made the first time the
 * row shows a message with a reference, then kept while the row is
 * recycled: few messages carry one (nostrc-boq9.5). Its widgets are named
 * after their ids, as template children would be found. */
static void
ensure_reference(GhMessageRow *self)
{
  if (self->reference_box)
    return;
  g_autoptr(GtkBuilder) builder =
    gtk_builder_new_from_resource("/org/nostr/Groundhog/ui/gh-message-reference.ui");
  static const gchar *const names[] = {
    "reference_box", "reference_card", "reference_copy_button", "share_reference_button",
    "public_actions", "repost_reference_button", "quote_reference_button",
  };
  for (guint i = 0; i < G_N_ELEMENTS(names); i++)
    gtk_widget_set_name(GTK_WIDGET(gtk_builder_get_object(builder, names[i])), names[i]);
  self->reference_box = GTK_BOX(gtk_builder_get_object(builder, "reference_box"));
  self->reference_card = GN_NOSTR_REFERENCE_CARD(gtk_builder_get_object(builder, "reference_card"));
  self->reference_copy_button = GTK_BUTTON(gtk_builder_get_object(builder, "reference_copy_button"));
  self->share_reference_button = GTK_BUTTON(gtk_builder_get_object(builder, "share_reference_button"));
  self->public_actions = GTK_BOX(gtk_builder_get_object(builder, "public_actions"));
  self->repost_reference_button = GTK_BUTTON(gtk_builder_get_object(builder, "repost_reference_button"));
  self->quote_reference_button = GTK_BUTTON(gtk_builder_get_object(builder, "quote_reference_button"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->reference_copy_button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Copy Nostr address"), -1);
  g_signal_connect_swapped(self->reference_card, "notify::state",
                           G_CALLBACK(update_public_actions), self);
  gtk_widget_insert_before(GTK_WIDGET(self->reference_box), GTK_WIDGET(self),
                           GTK_WIDGET(self->web_box));
  add_menu_gestures(self, GTK_WIDGET(self->reference_box));
}

static void
update_reference(GhMessageRow *self)
{
  const gchar *uri = NULL;
  const gchar *original = NULL;
  GnNostrReferenceCardRole role = GN_NOSTR_REFERENCE_CARD_ROLE_MENTION;
  gboolean visible = self->view && self->message && !self->undecryptable &&
                     !gh_message_get_withdrawn(self->message) &&
                     (gh_message_get_expires_at(self->message) == 0 ||
                      gh_message_get_expires_at(self->message) >
                        g_get_real_time() / G_USEC_PER_SEC);
  const GnNostrReference *reference = visible
    ? gh_conversation_view_get_reference_target(self->view, self->message, &uri, &role, &original)
    : NULL;
  if (!reference && !self->reference_box)
    return;
  ensure_reference(self);
  self->reference_role = reference ? role : GN_NOSTR_REFERENCE_CARD_ROLE_MENTION;
  gtk_widget_set_visible(GTK_WIDGET(self->reference_box), reference != NULL);
  /* The card only consults the resolver's verified local cache here. */
  gn_nostr_reference_card_set_resolver(self->reference_card, reference
    ? gh_conversation_view_get_reference_resolver(self->view) : NULL);
  gn_nostr_reference_card_set_reference(self->reference_card, reference, role, original);
  update_public_actions(self);
  if (!reference) return;
  const gchar *id = gh_message_get_rumor_id(self->message);
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->reference_copy_button), "s", uri);
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->share_reference_button), "s", id);
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->repost_reference_button), "s", id);
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->quote_reference_button), "s", id);
}

static gboolean on_activate_link(GhMessageRow *self, const gchar *uri);

/* The blocks follow the body: hidden with it (a repost card, a poll). */
static void
sync_body_blocks_visible(GhMessageRow *self)
{
  if (self->body_blocks)
    gtk_widget_set_visible(GTK_WIDGET(self->body_blocks),
                           gtk_widget_get_visible(GTK_WIDGET(self->body_label)) &&
                           gtk_widget_get_first_child(GTK_WIDGET(self->body_blocks)) != NULL);
}

static void
clear_body_blocks(GhMessageRow *self)
{
  gtk_widget_set_child_visible(GTK_WIDGET(self->body_label), TRUE);
  if (!self->body_blocks)
    return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->body_blocks))))
    gtk_box_remove(self->body_blocks, child);
  sync_body_blocks_visible(self);
}

static GtkLabel *
body_block_label(GhMessageRow *self, const gchar *markup, gint chars)
{
  GtkLabel *label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_markup(label, markup);
  gtk_label_set_selectable(label, TRUE);
  gtk_label_set_wrap(label, TRUE);
  gtk_label_set_wrap_mode(label, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(label, 0);
  gtk_label_set_max_width_chars(label, chars);
  g_signal_connect_object(label, "activate-link", G_CALLBACK(on_activate_link), self,
                          G_CONNECT_SWAPPED);
  gh_bidi_label_follow_content(label);
  return label;
}

/* A GFM table as a real table: a grid of wrapping, selectable cells with a
 * header row. Each column gets a fixed width in characters (its widest
 * cell, or a fair share of the bubble's width when the table is wider than
 * that; water-filling), so its cells wrap there and the grid's height for
 * a width is consistent. No scroller: a wrapping label in one is measured
 * at its narrowest and the bubble grows very tall. */
static GtkWidget *
body_table(GhMessageRow *self, const GnMarkdownPangoBlock *block)
{
  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_widget_add_css_class(GTK_WIDGET(grid), "groundhog-message-table");
  gtk_widget_set_halign(GTK_WIDGET(grid), GTK_ALIGN_START);
  guint columns = block->columns;
  g_autofree gint *widths = g_new0(gint, columns);
  g_autofree GtkLabel **labels = g_new0(GtkLabel *, (gsize)block->rows * columns);
  for (guint r = 0; r < block->rows; r++) {
    for (guint c = 0; c < columns; c++) {
      const gchar *cell = block->cells[r * columns + c];
      gboolean header = block->header && r == 0;
      g_autofree gchar *markup = header ? g_strdup_printf("<b>%s</b>", cell) : NULL;
      GtkLabel *label = body_block_label(self, header ? markup : cell, 1);
      labels[r * columns + c] = label;
      widths[c] = MAX(widths[c], (gint)g_utf8_strlen(gtk_label_get_text(label), -1));
      gtk_widget_add_css_class(GTK_WIDGET(label), header ? "header" : "cell");
      if (r + 1 == block->rows)
        gtk_widget_add_css_class(GTK_WIDGET(label), "last-row");
      gtk_label_set_yalign(label, 0);
      guint align = block->aligns[c];
      gtk_label_set_xalign(label, align == GN_MARKDOWN_ALIGN_RIGHT ? 1.0f
                                : align == GN_MARKDOWN_ALIGN_CENTER ? 0.5f : 0.0f);
      gtk_grid_attach(grid, GTK_WIDGET(label), (int)c, (int)r, 1, 1);
    }
  }
  /* A cell's padding takes about two characters of the budget. */
  gint budget = (self->compact ? TABLE_CHARS_COMPACT : TABLE_CHARS) - 2 * (gint)(columns - 1);
  budget = MAX(budget, (gint)columns * TABLE_CELL_MIN_CHARS);
  g_autofree gboolean *fixed = g_new0(gboolean, columns);
  for (guint settled = 0; settled < columns;) {
    gint share = budget / (gint)(columns - settled);
    guint before = settled;
    for (guint c = 0; c < columns; c++) {
      if (!fixed[c] && widths[c] <= share) {
        fixed[c] = TRUE;
        budget -= MAX(widths[c], 1);
        settled++;
      }
    }
    if (settled == before) {
      for (guint c = 0; c < columns; c++)
        if (!fixed[c])
          widths[c] = MAX(share, TABLE_CELL_MIN_CHARS);
      break;
    }
  }
  for (guint k = 0; k < block->rows * columns; k++) {
    gint chars = MAX(widths[k % columns], 1);
    gtk_label_set_width_chars(labels[k], chars);
    gtk_label_set_max_width_chars(labels[k], chars);
  }
  return GTK_WIDGET(grid);
}

/* The width of @chars characters of the body text, as a label measures
 * its max-width-chars. */
static gint
body_chars_width(GhMessageRow *self, gint chars)
{
  PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(self->body_label));
  PangoFontMetrics *metrics = pango_context_get_metrics(context, NULL, NULL);
  gint width = MAX(pango_font_metrics_get_approximate_char_width(metrics),
                   pango_font_metrics_get_approximate_digit_width(metrics));
  pango_font_metrics_unref(metrics);
  return PANGO_PIXELS_CEIL(width * chars);
}

/* A fenced code block as written: one unwrapped, selectable monospace label
 * that scrolls sideways (scrollbar only when it is wider than the bubble's
 * text width) and is never taller than its lines. */
static GtkWidget *
body_code(GhMessageRow *self, const GnMarkdownPangoBlock *block, gint chars)
{
  GtkLabel *label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_markup(label, block->markup);
  gtk_label_set_selectable(label, TRUE);
  gtk_label_set_wrap(label, FALSE);
  gtk_label_set_xalign(label, 0);
  gtk_label_set_yalign(label, 0);
  gtk_widget_add_css_class(GTK_WIDGET(label), "monospace");
  GtkScrolledWindow *scroller = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
  gtk_widget_add_css_class(GTK_WIDGET(scroller), "groundhog-message-code");
  gtk_scrolled_window_set_policy(scroller, GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_width(scroller, TRUE);
  gtk_scrolled_window_set_propagate_natural_height(scroller, TRUE);
  gtk_scrolled_window_set_max_content_width(scroller, body_chars_width(self, chars));
  gtk_scrolled_window_set_child(scroller, GTK_WIDGET(label));
  gtk_widget_set_halign(GTK_WIDGET(scroller), GTK_ALIGN_FILL);
  gtk_widget_set_valign(GTK_WIDGET(scroller), GTK_ALIGN_START);
  gtk_widget_set_vexpand(GTK_WIDGET(scroller), FALSE);
  return GTK_WIDGET(scroller);
}

/* Shows the rendered blocks of a body with a table or fenced code (see
 * body_blocks). */
static void
show_body_blocks(GhMessageRow *self, GPtrArray *blocks)
{
  if (!self->body_blocks) {
    self->body_blocks = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_widget_set_name(GTK_WIDGET(self->body_blocks), "body_blocks");
    gtk_widget_add_css_class(GTK_WIDGET(self->body_blocks), "groundhog-message-body");
    gtk_box_insert_child_after(self->bubble, GTK_WIDGET(self->body_blocks),
                               GTK_WIDGET(self->body_label));
  }
  gint chars = self->compact ? BODY_CHARS_COMPACT : BODY_CHARS;
  guint i = 0;
  const GnMarkdownPangoBlock *first = g_ptr_array_index(blocks, 0);
  if (first->kind == GN_MARKDOWN_PANGO_BLOCK_TEXT) {
    gtk_label_set_markup(self->body_label, first->markup);
    i = 1;
  } else {
    /* The body starts with a table: the empty label takes no room. */
    gtk_label_set_text(self->body_label, "");
    gtk_widget_set_child_visible(GTK_WIDGET(self->body_label), FALSE);
  }
  for (; i < blocks->len; i++) {
    const GnMarkdownPangoBlock *block = g_ptr_array_index(blocks, i);
    GtkWidget *widget = block->kind == GN_MARKDOWN_PANGO_BLOCK_TABLE ? body_table(self, block)
      : block->kind == GN_MARKDOWN_PANGO_BLOCK_CODE ? body_code(self, block, chars)
      : GTK_WIDGET(body_block_label(self, block->markup, chars));
    gtk_box_append(self->body_blocks, widget);
  }
  sync_body_blocks_visible(self);
}

static void
refresh_body_markup(GhMessageRow *self)
{
  clear_body_blocks(self);
  if (!self->message || self->undecryptable || gh_message_get_withdrawn(self->message))
    return;
  const gchar *body = gh_message_get_content(self->message);
  const gchar *cached = self->view
    ? gh_conversation_view_get_render_markup(self->view, self->message) : NULL;
  GPtrArray *blocks = cached
    ? gh_conversation_view_get_render_blocks(self->view, self->message) : NULL;
  if (blocks && blocks->len) {
    show_body_blocks(self, blocks);
  } else if (cached) {
    gtk_label_set_markup(self->body_label, cached);
  } else {
    /* Even a body larger than the cache budget must remain visible; a
     * repost or quote as the card and its own text, never its JSON
     * (nostrc-tv98u). */
    g_autofree gchar *quote_text = NULL;
    gboolean repost = gh_conversation_view_parse_repost_body(body, &quote_text, NULL);
    const gchar *shown = !repost ? body
      : quote_text ? quote_text : _("The reposted note isn't available on this device.");
    g_autofree gchar *valid = g_utf8_make_valid(shown ? shown : "", -1);
    g_autofree gchar *plain = g_markup_escape_text(valid, -1);
    gtk_label_set_markup(self->body_label, plain);
  }
  gh_bidi_label_follow_content(self->body_label);
}

static void
refresh_preview_uri(GhMessageRow *self)
{
  g_clear_pointer(&self->preview_uri, g_free);
  g_clear_pointer(&self->image_uri, g_free);
  if (!self->view || !self->message || self->undecryptable ||
      gh_message_get_withdrawn(self->message) ||
      !gtk_widget_get_visible(GTK_WIDGET(self->body_label)))
    return;
  self->preview_uri = g_strdup(gh_conversation_view_get_preview_uri(self->view,
                                                                     self->message));
  self->image_uri = g_strdup(gh_conversation_view_get_image_uri(self->view, self->message));
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
  arrange_meta(self, outgoing, FALSE);

  g_clear_pointer(&self->preview_uri, g_free);
  g_clear_pointer(&self->image_uri, g_free);

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
  self->body_hidden_for_repost = FALSE;
  clear_body_blocks(self);
  if (!message) {
    gtk_label_set_text(self->body_label, "");
    gtk_label_set_text(self->sender_label, "");
    gtk_label_set_text(self->time_label, "");
    gtk_widget_set_direction(GTK_WIDGET(self->body_label), GTK_TEXT_DIR_NONE);
    gtk_widget_set_direction(GTK_WIDGET(self->sender_label), GTK_TEXT_DIR_NONE);
  } else {
    g_autofree gchar *file = self->undecryptable || withdrawn || show_body ? NULL : file_text(message);
    if (self->undecryptable) {
      gtk_label_set_text(self->body_label, _("Unable to decrypt yet"));
    } else if (withdrawn) {
      gtk_label_set_text(self->body_label, gh_message_withdrawn_text());
    } else if (file) {
      gtk_label_set_text(self->body_label, file);
    } else {
      refresh_body_markup(self);
      refresh_preview_uri(self);
    }
    g_autofree gchar *sender = gh_message_row_sender_name(message);
    gtk_label_set_text(self->sender_label, sender);
    gh_bidi_label_follow_content(self->sender_label);
    if (self->undecryptable || withdrawn || file)
      gtk_widget_set_direction(GTK_WIDGET(self->body_label), GTK_TEXT_DIR_NONE);
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
  update_reference(self);
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

/* ---- the bubble's menu ------------------------------------------------------------ */

/* The text selected in a selectable label under @root (the body, a table
 * cell, a code block), or NULL. GTK keeps one selection per label. */
static gchar *
selected_text(GtkWidget *root)
{
  int start = 0, end = 0;
  if (GTK_IS_LABEL(root) && gtk_label_get_selectable(GTK_LABEL(root)) &&
      gtk_label_get_selection_bounds(GTK_LABEL(root), &start, &end) && start != end) {
    const gchar *text = gtk_label_get_text(GTK_LABEL(root));
    glong length = g_utf8_strlen(text, -1);
    start = CLAMP(start, 0, length);
    end = CLAMP(end, start, length);
    const gchar *from = g_utf8_offset_to_pointer(text, start);
    const gchar *to = g_utf8_offset_to_pointer(text, end);
    return to > from ? g_strndup(from, to - from) : NULL;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_POPOVER(child) || !gtk_widget_get_visible(child))
      continue;
    gchar *text = selected_text(child);
    if (text)
      return text;
  }
  return NULL;
}

/* "Copy Message" (or "Copy Link" for a file) when the message's text can be
 * copied: the same messages, and the same text, as the view's
 * conversation.copy-message; NULL otherwise. */
static const gchar *
copy_message_label(GhMessageRow *self)
{
  GhMessage *message = self->message;
  if (!message || self->undecryptable || gh_message_get_withdrawn(message) ||
      gh_message_get_kind(message) == GH_MESSAGE_MLS_POLL_VOTE_KIND ||
      (gh_message_get_expires_at(message) > 0 &&
       gh_message_get_expires_at(message) <= g_get_real_time() / G_USEC_PER_SEC))
    return NULL;
  return gh_message_get_kind(message) == GH_NIP17_FILE_KIND ? _("Copy _Link")
                                                           : _("Copy _Message");
}

static void
append_targeted(GMenu *menu, const gchar *label, const gchar *action, GVariant *target)
{
  g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
  g_menu_item_set_action_and_target_value(item, action, target);
  g_menu_append_item(menu, item);
}

static void
append_section(GMenu *menu, GMenu *section)
{
  if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0)
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
}

/* The menu for the bound message, as it is now: Copy only with a selection;
 * nothing that edits text (the bubble is read-only); Delete only where the
 * view can do it (gh_conversation_view_can_delete_message()). */
static GMenuModel *
build_menu(GhMessageRow *self)
{
  GMenu *menu = g_menu_new();
  const gchar *rumor = gh_message_get_rumor_id(self->message);
  GMenu *copy = g_menu_new();
  if (self->menu_selection)
    g_menu_append(copy, _("_Copy"), "message.copy-selection");
  const gchar *copy_label = copy_message_label(self);
  if (copy_label && rumor)
    append_targeted(copy, copy_label, "conversation.copy-message", g_variant_new_string(rumor));
  append_section(menu, copy);
  GMenu *react = g_menu_new();
  if (rumor && gtk_widget_get_visible(GTK_WIDGET(self->react_button)) &&
      gtk_widget_is_sensitive(GTK_WIDGET(self->react_button)))
    g_menu_append(react, _("Add _Reaction…"), "message.react");
  append_section(menu, react);
  GMenu *remove = g_menu_new();
  if (rumor && self->view &&
      gh_conversation_view_can_delete_message(self->view, self->message, FALSE))
    append_targeted(remove, _("_Delete for Me…"), "conversation.delete-message",
                    g_variant_new("(sb)", rumor, FALSE));
  if (rumor && self->view &&
      gh_conversation_view_can_delete_message(self->view, self->message, TRUE))
    append_targeted(remove, _("Delete for _Everyone…"), "conversation.delete-message",
                    g_variant_new("(sb)", rumor, TRUE));
  append_section(menu, remove);
  return G_MENU_MODEL(menu);
}

/* nostrc-yloj8: the menu of a part of the row outside the bubble (the link
 * preview, the shared note's card): Copy with a selection there and Copy
 * Link for its address, never GTK's label menu. */
static GMenuModel *
build_area_menu(GhMessageRow *self, GtkWidget *area)
{
  GMenu *menu = g_menu_new();
  GMenu *copy = g_menu_new();
  if (self->menu_selection)
    g_menu_append(copy, _("_Copy"), "message.copy-selection");
  g_clear_pointer(&self->menu_link, g_free);
  const gchar *link = NULL;
  if (area == GTK_WIDGET(self->preview_box))
    link = self->preview_uri;
  else if (self->view && self->message)
    gh_conversation_view_get_reference(self->view, self->message, &link);
  if (link) {
    self->menu_link = g_strdup(link);
    g_menu_append(copy, _("Copy _Link"), "message.copy-link");
  }
  append_section(menu, copy);
  return G_MENU_MODEL(menu);
}

/* The part of the row (the bubble, the link preview or the reference card)
 * holding @widget, or NULL. */
static GtkWidget *
menu_area(GhMessageRow *self, GtkWidget *widget)
{
  GtkWidget *areas[] = { GTK_WIDGET(self->bubble), GTK_WIDGET(self->preview_box),
                         self->reference_box ? GTK_WIDGET(self->reference_box) : NULL };
  for (guint i = 0; i < G_N_ELEMENTS(areas); i++)
    if (areas[i] && widget && (widget == areas[i] || gtk_widget_is_ancestor(widget, areas[i])))
      return areas[i];
  return NULL;
}

/* Pops up @area's menu at (@x, @y) in @area (negative: the whole area). The
 * popover always hangs off the bubble, which the row never empties. */
static gboolean
popup_area_menu(GhMessageRow *self, GtkWidget *area, gdouble x, gdouble y)
{
  if (!self->message || !area || !gtk_widget_get_mapped(area))
    return FALSE;
  g_free(self->menu_selection);
  self->menu_selection = selected_text(area);
  g_autoptr(GMenuModel) model = area == GTK_WIDGET(self->bubble) ? build_menu(self)
                                                                 : build_area_menu(self, area);
  if (g_menu_model_get_n_items(model) == 0)
    return FALSE;
  /* A new popover each time: the macOS backend cannot present a popup
   * surface again while the last one is still going away. */
  if (self->menu)
    gh_widget_unparent_unfocused(GTK_WIDGET(self->menu));
  self->menu = GTK_POPOVER_MENU(gtk_popover_menu_new_from_model(model));
  gtk_widget_set_name(GTK_WIDGET(self->menu), "message_menu");
  gtk_widget_set_halign(GTK_WIDGET(self->menu), GTK_ALIGN_START);
  gtk_popover_set_has_arrow(GTK_POPOVER(self->menu), FALSE);
  gtk_widget_set_parent(GTK_WIDGET(self->menu), GTK_WIDGET(self->bubble));
  graphene_rect_t bounds;
  graphene_rect_init(&bounds, 0, 0, gtk_widget_get_width(area), gtk_widget_get_height(area));
  if (area != GTK_WIDGET(self->bubble)) {
    graphene_point_t origin;
    if (gtk_widget_compute_point(area, GTK_WIDGET(self->bubble), graphene_point_zero(), &origin))
      bounds.origin = origin;
  }
  GdkRectangle rect = { (int)bounds.origin.x, (int)bounds.origin.y,
                        MAX((int)bounds.size.width, 1), MAX((int)bounds.size.height, 1) };
  if (x >= 0 && y >= 0)
    rect = (GdkRectangle){ (int)(bounds.origin.x + x), (int)(bounds.origin.y + y), 1, 1 };
  gtk_popover_set_pointing_to(GTK_POPOVER(self->menu), &rect);
  gtk_popover_popup(GTK_POPOVER(self->menu));
  return TRUE;
}

gboolean
gh_message_row_popup_menu(GhMessageRow *self, gdouble x, gdouble y)
{
  g_return_val_if_fail(GH_IS_MESSAGE_ROW(self), FALSE);
  return popup_area_menu(self, GTK_WIDGET(self->bubble), x, y);
}

gboolean
gh_message_row_popup_menu_for_focus(GhMessageRow *self, GtkWidget *focus)
{
  g_return_val_if_fail(GH_IS_MESSAGE_ROW(self), FALSE);
  /* The avatar has its own menu (the sender's picture). */
  if (focus && (focus == GTK_WIDGET(self->avatar) ||
                gtk_widget_is_ancestor(focus, GTK_WIDGET(self->avatar))))
    return FALSE;
  GtkWidget *area = menu_area(self, focus);
  return popup_area_menu(self, area ? area : GTK_WIDGET(self->bubble), -1, -1);
}

GtkPopover *
gh_message_row_get_menu(GhMessageRow *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE_ROW(self), NULL);
  return self->menu ? GTK_POPOVER(self->menu) : NULL;
}

static void
action_copy_selection(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name; (void)parameter;
  GhMessageRow *self = GH_MESSAGE_ROW(widget);
  if (self->menu_selection)
    gtk_widget_activate_action(widget, "conversation.copy-text", "s", self->menu_selection);
}

static void
action_copy_link(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name; (void)parameter;
  GhMessageRow *self = GH_MESSAGE_ROW(widget);
  if (self->menu_link)
    gtk_widget_activate_action(widget, "conversation.copy-text", "s", self->menu_link);
}

static void
action_react(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name; (void)parameter;
  show_picker(GH_MESSAGE_ROW(widget), -1, -1);
}

static void
on_secondary_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                     GhMessageRow *self)
{
  (void)n_press;
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  popup_area_menu(self, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)), x, y);
}

static void
on_long_pressed(GtkGestureLongPress *gesture, gdouble x, gdouble y, GhMessageRow *self)
{
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  popup_area_menu(self, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)), x, y);
}

/* Right-click and long-press anywhere on @area, its text included, open
 * the row's own menu for it: caught before the labels' own gestures, so
 * GTK's Cut / Paste / Delete / Insert Emoji menu never shows. */
static void
add_menu_gestures(GhMessageRow *self, GtkWidget *area)
{
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect(click, "pressed", G_CALLBACK(on_secondary_pressed), self);
  gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
  GtkGesture *hold = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(hold), TRUE);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(hold), GTK_PHASE_CAPTURE);
  g_signal_connect(hold, "pressed", G_CALLBACK(on_long_pressed), self);
  gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(hold));
}

static gboolean
avatar_picture_action_available(GhMessageRow *self)
{
  if (!self->message || !self->view || !self->run_end ||
      gh_message_is_self(self->message) || self->undecryptable ||
      gh_message_get_withdrawn(self->message))
    return FALSE;
  g_autofree gchar *uri = gh_conversation_view_dup_picture_uri(self->view, self->message);
  if (!uri) return FALSE;
  GhLinkPreviewState state = GH_LINK_PREVIEW_NONE;
  gh_conversation_view_get_web_texture(self->view, self->message, GH_WEB_PICTURE, &state);
  return state == GH_LINK_PREVIEW_NONE || state == GH_LINK_PREVIEW_FAILED;
}

static void
show_avatar_menu(GhMessageRow *self)
{
  if (!avatar_picture_action_available(self)) return;
  if (!self->avatar_menu) {
    self->avatar_menu = GTK_POPOVER(gtk_popover_new());
    gtk_widget_set_name(GTK_WIDGET(self->avatar_menu), "avatar_menu");
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->avatar_menu),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, _("Profile Picture"), -1);
    gtk_widget_set_parent(GTK_WIDGET(self->avatar_menu), GTK_WIDGET(self->avatar));
    GtkWidget *button = gtk_button_new_with_label(_("Load Profile Picture"));
    gtk_widget_set_name(button, "picture_menu_button");
    gtk_widget_add_css_class(button, "flat");
    g_signal_connect_swapped(button, "clicked", G_CALLBACK(gtk_popover_popdown),
                             self->avatar_menu);
    gtk_popover_set_child(self->avatar_menu, button);
  }
  GtkWidget *button = gtk_popover_get_child(self->avatar_menu);
  g_autofree gchar *target = g_strconcat("picture:",
                                         gh_message_get_rumor_id(self->message), NULL);
  gtk_actionable_set_action_target(GTK_ACTIONABLE(button), "s", target);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "conversation.show-preview");
  gtk_popover_popup(self->avatar_menu);
}

static void
on_avatar_secondary_pressed(GtkGestureClick *gesture, gint n_press,
                            gdouble x, gdouble y, GhMessageRow *self)
{
  (void)n_press; (void)x; (void)y;
  show_avatar_menu(self);
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_avatar_long_pressed(GtkGestureLongPress *gesture, gdouble x, gdouble y,
                       GhMessageRow *self)
{
  (void)x; (void)y;
  show_avatar_menu(self);
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static gboolean
on_avatar_key_pressed(GtkEventControllerKey *controller, guint keyval,
                      guint keycode, GdkModifierType state, GhMessageRow *self)
{
  (void)controller; (void)keycode;
  if (keyval != GDK_KEY_Menu && !(keyval == GDK_KEY_F10 && (state & GDK_SHIFT_MASK)))
    return FALSE;
  show_avatar_menu(self);
  return TRUE;
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

/* A worker completed for this immutable message. Recycled rows reject IDs
 * from a previous binding, so a late result cannot paint another message. */
static void
on_render_changed(GhMessageRow *self, const gchar *id)
{
  if (self->message && (!id || g_strcmp0(gh_message_get_rumor_id(self->message), id) == 0)) {
    refresh_body_markup(self);
    refresh_preview_uri(self);
    update_preview(self);
    update_reference(self);
  }
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
    refresh_body_markup(self);
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
  g_signal_connect_object(view, "render-changed", G_CALLBACK(on_render_changed), self,
                          G_CONNECT_SWAPPED);
  refresh_body_markup(self);
  refresh_preview_uri(self);
  update_preview(self);
  update_reference(self);
}

static void
gh_message_row_unroot(GtkWidget *widget)
{
  GhMessageRow *self = GH_MESSAGE_ROW(widget);
  if (self->avatar_menu)
    gtk_popover_popdown(self->avatar_menu);
  if (self->menu)
    gtk_popover_popdown(GTK_POPOVER(self->menu));
  if (self->view) {
    g_signal_handlers_disconnect_by_func(self->view, on_preview_changed, self);
    g_signal_handlers_disconnect_by_func(self->view, on_render_changed, self);
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
  if (self->avatar_menu)
    gtk_popover_popdown(self->avatar_menu);
  if (self->menu)
    gtk_popover_popdown(GTK_POPOVER(self->menu));
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
    if (self->preview_uri || self->image_uri) optional |= GH_OPEN_LINK;
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
    gh_widget_unparent_unfocused(GTK_WIDGET(self->picker));
    self->picker = NULL;
  }
  if (self->avatar_menu) {
    gh_widget_unparent_unfocused(GTK_WIDGET(self->avatar_menu));
    self->avatar_menu = NULL;
  }
  if (self->menu) {
    gh_widget_unparent_unfocused(GTK_WIDGET(self->menu));
    self->menu = NULL;
  }
  g_clear_pointer(&self->menu_selection, g_free);
  g_clear_pointer(&self->menu_link, g_free);
  if (self->reply_button) {
    gtk_widget_unparent(GTK_WIDGET(self->reply_button));
    self->reply_button = NULL;
    self->reply_label = NULL;
  }
  if (self->reference_box) {
    gh_widget_unparent_unfocused(GTK_WIDGET(self->reference_box));
    self->reference_box = NULL;
    self->reference_card = NULL;
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
  g_free(self->image_uri);
  g_free(self->summary);
  G_OBJECT_CLASS(gh_message_row_parent_class)->finalize(object);
}

static void
action_avatar_menu(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name; (void)parameter;
  show_avatar_menu(GH_MESSAGE_ROW(widget));
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
  gtk_widget_class_install_action(widget_class, "message.avatar-menu", NULL,
                                  action_avatar_menu);

  g_type_ensure(GH_TYPE_DELIVERY_INDICATOR);
  g_type_ensure(GN_TYPE_NOSTR_REFERENCE_CARD);
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
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, react_button);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, preview_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, bubble_line);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, avatar);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, web_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, meta_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, status_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, timer_icon);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, time_label);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, delivery);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, retry_button);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, voice_slot);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, attachment_slot);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, poll_slot);
  gtk_widget_class_set_css_name(widget_class, "groundhog-message");
  gtk_widget_class_install_action(widget_class, "message.copy-selection", NULL,
                                  action_copy_selection);
  gtk_widget_class_install_action(widget_class, "message.copy-link", NULL, action_copy_link);
  gtk_widget_class_install_action(widget_class, "message.react", NULL, action_react);
}

static void
gh_message_row_init(GhMessageRow *self)
{
#ifdef GROUNDHOG_CONVERSATION_OPEN_PROBE
  gint64 construct_start_us = g_get_monotonic_time();
#endif
  gtk_widget_init_template(GTK_WIDGET(self));
  self->meta_side = 0; /* the template lists the meta line in the incoming order */
  self->extra_cards = g_ptr_array_new();
  /* The row's actions take the message's rumor id: a row set up (or rooted
   * again, e.g. when the window collapses) before a message is bound names
   * none, so GTK never meets a target-less "s" action. */
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->retry_button), "s", "");
  self->run_start = TRUE;
  /* The bubble and the link preview (yloj8) have the row's own menus. */
  add_menu_gestures(self, GTK_WIDGET(self->bubble));
  add_menu_gestures(self, GTK_WIDGET(self->preview_box));
  gtk_widget_set_focusable(GTK_WIDGET(self->avatar), TRUE);
  GtkGesture *avatar_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(avatar_click), GDK_BUTTON_SECONDARY);
  g_signal_connect(avatar_click, "pressed", G_CALLBACK(on_avatar_secondary_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self->avatar), GTK_EVENT_CONTROLLER(avatar_click));
  GtkGesture *avatar_hold = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(avatar_hold), TRUE);
  g_signal_connect(avatar_hold, "pressed", G_CALLBACK(on_avatar_long_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self->avatar), GTK_EVENT_CONTROLLER(avatar_hold));
  GtkEventController *avatar_keys = gtk_event_controller_key_new();
  g_signal_connect(avatar_keys, "key-pressed", G_CALLBACK(on_avatar_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self->avatar), avatar_keys);
  self->run_end = TRUE;
  self->summary = g_strdup("");
  g_signal_connect_swapped(self->body_label, "activate-link", G_CALLBACK(on_activate_link),
                           self);
  g_signal_connect_swapped(self->body_label, "notify::visible",
                           G_CALLBACK(sync_body_blocks_visible), self);
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
