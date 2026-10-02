#ifndef GH_COMPOSER_H
#define GH_COMPOSER_H

#include <adwaita.h>

G_BEGIN_DECLS

/* The bound the composer applies until its owner sets a length function: the
 * charter's figure (§7.7), the NIP-44 plaintext limit minus envelope
 * overhead. The application sets the outbox's exact check instead
 * (gh_outbox_text_fits()). */
#define GH_COMPOSER_DEFAULT_MAX_BYTES 60000
/* A draft is reported this long after the last edit (charter §7.7). */
#define GH_COMPOSER_DRAFT_DELAY_MS 1000
/* Lines of text shown before the composer scrolls, unless "max-lines" says
 * otherwise (3 when the content is short, charter §7.12). */
#define GH_COMPOSER_DEFAULT_MAX_LINES 6

/*
 * GhComposer (data/ui/gh-composer.blp, charter §7.4, §7.7, §7.14, G13): the
 * message entry under a conversation. GTK-only; it knows no account, store or
 * outbox. Its owner (gh-send-ui.c in the application) listens to "send" and
 * "draft-changed" and says why sending is impossible, if it is.
 *
 * Keys (a capture-phase controller on the text view, named
 * "groundhog-composer-keys"). While an input method composes text (the text
 * view's "preedit-changed" reports a preedit), Enter goes to the input
 * method (gtk_text_view_im_context_filter_keypress()) to commit it and does
 * nothing else: it never sends a half-typed word. An Enter that would send
 * is offered to the input method first too; Shift+Enter and the like are
 * left to the text view, whose own controller filters them once. (GTK runs
 * a widget's most recently added controller first, so none the composer
 * adds could run after the text view's input-method filtering, as charter
 * §7.7 first asked; it consults the input method itself instead.)
 *  - Enter sends while "enter-sends" is set (the enter-sends setting),
 *    otherwise it inserts a newline;
 *  - Shift+Enter always inserts a newline, Ctrl+Enter always sends;
 *  - Ctrl+. and Ctrl+; open GTK's emoji chooser (the text view's own
 *    bindings); the emoji button does the same with the mouse and is hidden
 *    while "compact".
 * Send ("composer.send", the Send button, Enter) is possible only while the
 * edit page shows and the text is neither blank (whitespace only) nor too
 * long for the length function (default: GH_COMPOSER_DEFAULT_MAX_BYTES bytes);
 * a text that is too long shows an inline error and is never truncated.
 * Sending emits "send" (text) -> gboolean: TRUE means the owner queued it,
 * and the composer clears itself without reporting a draft (the queueing
 * cleared the stored draft); FALSE keeps the text, and the owner says why
 * with gh_composer_set_error(). Keyboard focus stays in the text view.
 *
 * Drafts: "draft-changed" (text; "" when emptied) is emitted
 * GH_COMPOSER_DRAFT_DELAY_MS after the last edit, or at once by
 * gh_composer_flush_draft(); gh_composer_set_text() (e.g. a restored draft)
 * reports nothing and cancels a pending report.
 *
 * Disabled: a non-empty "disabled-reason" replaces the entry with that
 * sentence (the text is kept for when sending is possible again), with an
 * optional button (gh_composer_set_disabled_action()).
 *
 * Disappearing timer (charter §3.7, G07; W14 review B1): "disappearing-timer"
 * is the shown conversation's timer in seconds, 0 while it is off. While it
 * is on, "timer_slot" before the entry shows a compact indicator (an alarm
 * icon and the duration, e.g. "1 day") whose tooltip and accessible label
 * say "Messages you send disappear after 1 day", so nothing disappears
 * without a word before sending. It activates win.conversation-info
 * (gh_conversation_info_attach()), where the timer is changed; the owner
 * keeps the property in step with the conversation (gh-send-ui.c).
 *
 * Attachments (charter §6, G22): while "can-attach" is set (the owner can
 * send a file in the shown conversation) the attach button is shown; it
 * ("composer.attach") emits "attach-requested", for the owner's file
 * chooser. A file dropped on the composer (the first, when several) emits
 * "attach-file" (GFile), a dropped image "attach-texture" (GdkTexture); a
 * paste of an image, or of copied files without text, emits the same
 * instead of pasting text. Nothing is offered while the composer is
 * disabled. The composer only hands the file over; the owner shows what
 * will be sent before anything leaves.
 *
 * Properties: "compact" (hides the emoji button, tighter padding), "max-lines"
 * (lines shown before scrolling), "enter-sends", "disabled-reason" (nullable),
 * "disappearing-timer", "can-attach", and the read-only "can-send" and
 * "too-long".
 */
#define GH_TYPE_COMPOSER (gh_composer_get_type())
G_DECLARE_FINAL_TYPE(GhComposer, gh_composer, GH, COMPOSER, GtkWidget)

GtkWidget *gh_composer_new(void);

/* The whole text (transfer full). */
gchar *gh_composer_dup_text(GhComposer *self);
/* Replaces the text (NULL: empty) without reporting a draft, and cancels a
 * pending report and any error. */
void gh_composer_set_text(GhComposer *self, const gchar *text);
/* Emits a pending "draft-changed" now; nothing when there is none. */
void gh_composer_flush_draft(GhComposer *self);
/* Whether an edit has not been reported by "draft-changed" yet. */
gboolean gh_composer_get_draft_pending(GhComposer *self);

/* What Enter and the Send button do: emits "send" when sending is possible.
 * TRUE when the owner queued the text (it is cleared). */
gboolean gh_composer_send(GhComposer *self);
gboolean gh_composer_get_can_send(GhComposer *self);
gboolean gh_composer_get_too_long(GhComposer *self);

/* Whether @text is short enough to send (e.g. gh_outbox_text_fits()). NULL
 * restores the default byte bound. */
typedef gboolean (*GhComposerLengthFunc)(const gchar *text, gpointer user_data);
void gh_composer_set_length_func(GhComposer *self, GhComposerLengthFunc func,
                                 gpointer user_data, GDestroyNotify destroy);
/* Re-checks the text, e.g. after the length function's inputs changed. */
void gh_composer_revalidate(GhComposer *self);

/* An inline error under the entry, such as "Storage is full" after a failed
 * send; NULL clears it. The next edit clears it too. */
void gh_composer_set_error(GhComposer *self, const gchar *message);
const gchar *gh_composer_get_error(GhComposer *self);

void gh_composer_set_disabled_reason(GhComposer *self, const gchar *reason);
const gchar *gh_composer_get_disabled_reason(GhComposer *self);
/* A button beside the disabled reason, e.g. "Check Again" (NULL label
 * hides it); @detailed_action is a detailed action name. */
void gh_composer_set_disabled_action(GhComposer *self, const gchar *label,
                                     const gchar *detailed_action);

void gh_composer_set_compact(GhComposer *self, gboolean compact);
gboolean gh_composer_get_compact(GhComposer *self);
void gh_composer_set_max_lines(GhComposer *self, guint max_lines);
guint gh_composer_get_max_lines(GhComposer *self);
void gh_composer_set_enter_sends(GhComposer *self, gboolean enter_sends);
gboolean gh_composer_get_enter_sends(GhComposer *self);

/* The text view, which takes keyboard focus (also through
 * gtk_widget_grab_focus() on the composer while it is editable). */
GtkTextView *gh_composer_get_text_view(GhComposer *self);
/* The clipboard Paste looks at for an image or files; NULL (the default):
 * the text view's own, the display's. Anything else the composer lets
 * through to GtkTextView's own "paste-clipboard" handler, which pastes text
 * from the text view's clipboard. Tests hand in a private one (nostrc-rjz2)
 * and paste its text from a handler of their own that runs after the
 * composer's: the system clipboard is shared with every other process, and
 * on macOS an item this process put there re-enters its main loop whenever
 * another process reads it (GDK's pasteboard provider), which GLib then
 * reports as a failed poll(2). */
void gh_composer_set_clipboard(GhComposer *self, GdkClipboard *clipboard);
/* The disappearing timer shown before sending (see above); negative: off. */
void gh_composer_set_disappearing_timer(GhComposer *self, gint64 seconds);
gint64 gh_composer_get_disappearing_timer(GhComposer *self);
/* The box holding the timer indicator (visible while the timer is on). */
GtkBox *gh_composer_get_timer_slot(GhComposer *self);
/* Whether a file can be sent in the shown conversation (see above). */
void gh_composer_set_can_attach(GhComposer *self, gboolean can_attach);
gboolean gh_composer_get_can_attach(GhComposer *self);
GtkButton *gh_composer_get_attach_button(GhComposer *self);

/* NIP-88 poll creation (W26 slice C). */
void gh_composer_set_can_create_poll(GhComposer *self, gboolean can_create_poll);
gboolean gh_composer_get_can_create_poll(GhComposer *self);

G_END_DECLS
#endif
