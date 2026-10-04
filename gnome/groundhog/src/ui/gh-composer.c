#include "gh-composer.h"

#include <glib/gi18n.h>
#include <string.h>

#define ICON_RESOURCE_PATH "/org/nostr/Groundhog/icons"

struct _GhComposer {
  GtkWidget parent_instance;

  /* Template children. */
  GtkStack *composer_stack;
  GtkLabel *error_label;
  GtkButton *attach_button;
  GtkButton *voice_button;
  GtkButton *poll_button;
  GtkBox *timer_slot;
  GtkButton *timer_button;
  GtkLabel *timer_label;
  GtkScrolledWindow *scroller;
  GtkTextView *text_view;
  GtkLabel *placeholder;
  GtkMenuButton *emoji_button;
  GtkEmojiChooser *emoji_chooser;
  GtkButton *send_button;
  GtkLabel *disabled_reason;
  GtkButton *disabled_button;

  /* W27 recording page (nostrc-4h64). */
  GtkImage *recording_icon;
  GtkLabel *recording_label;
  GtkLabel *recording_time;
  GtkLevelBar *recording_level;
  GtkButton *cancel_recording_button;
  GtkButton *stop_recording_button;

  GtkTextBuffer *buffer; /* the text view's */
  gboolean compact;
  gboolean enter_sends;
  guint max_lines;
  gint64 timer;          /* the conversation's disappearing timer; 0: off */
  gchar *reason;         /* why sending is impossible; NULL: it is possible */
  gchar *error;          /* the owner's inline error (e.g. storage full) */
  gboolean blank;
  gboolean too_long;
  gboolean can_send;
  gboolean setting_text; /* a programmatic change: no draft report */
  gboolean preedit;      /* an input method is composing text (a preedit) */
  gboolean can_attach;   /* the owner can send a file here (G22) */
  gboolean can_create_poll; /* in an encrypted group (W26) */
  gboolean can_record_voice; /* voice recording available (W27) */
  gboolean draft_pending;
  guint draft_timer;
  GhComposerLengthFunc length_func;
  gpointer length_data;
  GDestroyNotify length_destroy;
  GdkClipboard *clipboard; /* what Paste reads; NULL: the text view's own */
};

enum {
  PROP_0,
  PROP_COMPACT,
  PROP_MAX_LINES,
  PROP_ENTER_SENDS,
  PROP_DISABLED_REASON,
  PROP_DISAPPEARING_TIMER,
  PROP_CAN_ATTACH,
  PROP_CAN_CREATE_POLL,
  PROP_CAN_RECORD_VOICE,
  PROP_CAN_SEND,
  PROP_TOO_LONG,
  N_PROPS
};
static GParamSpec *props[N_PROPS];

enum {
  SIGNAL_SEND,
  SIGNAL_DRAFT_CHANGED,
  SIGNAL_ATTACH_REQUESTED,
  SIGNAL_ATTACH_FILE,
  SIGNAL_ATTACH_TEXTURE,
  SIGNAL_POLL_REQUESTED,
  SIGNAL_RECORD_VOICE_REQUESTED,
  SIGNAL_STOP_RECORDING,
  SIGNAL_CANCEL_RECORDING,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhComposer, gh_composer, GTK_TYPE_WIDGET)

/* ---- state ------------------------------------------------------------------------- */

static gboolean
is_blank(const gchar *text)
{
  for (const gchar *p = text; *p; p = g_utf8_next_char(p))
    if (!g_unichar_isspace(g_utf8_get_char(p)))
      return FALSE;
  return TRUE;
}

static gboolean
fits(GhComposer *self, const gchar *text)
{
  if (self->length_func)
    return self->length_func(text, self->length_data);
  return strlen(text) <= GH_COMPOSER_DEFAULT_MAX_BYTES;
}

static gboolean
editable(GhComposer *self)
{
  return !self->reason;
}

/* Recomputes what the text allows: Send, the placeholder and the inline
 * error. */
static void
update_state(GhComposer *self)
{
  g_autofree gchar *text = gh_composer_dup_text(self);
  gboolean blank = is_blank(text);
  gboolean too_long = !fits(self, text);
  gboolean can_send = editable(self) && !blank && !too_long;

  gtk_widget_set_visible(GTK_WIDGET(self->placeholder), *text == '\0');
  const gchar *error = too_long
    ? _("This message is too long to send privately. Shorten it to send it.")
    : self->error;
  gtk_label_set_text(self->error_label, error ? error : "");
  gtk_widget_set_visible(GTK_WIDGET(self->error_label), error != NULL);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "composer.send", can_send);

  self->blank = blank;
  g_object_freeze_notify(G_OBJECT(self));
  if (self->too_long != too_long) {
    self->too_long = too_long;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TOO_LONG]);
  }
  if (self->can_send != can_send) {
    self->can_send = can_send;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_SEND]);
  }
  g_object_thaw_notify(G_OBJECT(self));
}

/* The entry grows up to max-lines lines of the current font, then scrolls. */
static void
update_height(GhComposer *self)
{
  PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(self->text_view));
  PangoFontMetrics *metrics = pango_context_get_metrics(context, NULL, NULL);
  gint line = PANGO_PIXELS(pango_font_metrics_get_height(metrics));
  if (line <= 0)
    line = PANGO_PIXELS(pango_font_metrics_get_ascent(metrics) +
                        pango_font_metrics_get_descent(metrics));
  pango_font_metrics_unref(metrics);
  gint margins = gtk_text_view_get_top_margin(self->text_view) +
                 gtk_text_view_get_bottom_margin(self->text_view);
  line = MAX(line, 1);
  /* At least one line, at most max-lines; order keeps min <= max. */
  gtk_scrolled_window_set_min_content_height(self->scroller, -1);
  gtk_scrolled_window_set_max_content_height(self->scroller,
                                             line * (gint)self->max_lines + margins);
  gtk_scrolled_window_set_min_content_height(self->scroller, line + margins);
}

static void
cancel_draft_timer(GhComposer *self)
{
  if (self->draft_timer) {
    g_source_remove(self->draft_timer);
    self->draft_timer = 0;
  }
}

static gboolean
draft_timeout(gpointer data)
{
  GhComposer *self = data;
  self->draft_timer = 0;
  gh_composer_flush_draft(self);
  return G_SOURCE_REMOVE;
}

static void
on_buffer_changed(GhComposer *self)
{
  if (self->setting_text)
    return;
  g_clear_pointer(&self->error, g_free);
  update_state(self);
  self->draft_pending = TRUE;
  cancel_draft_timer(self);
  self->draft_timer = g_timeout_add(GH_COMPOSER_DRAFT_DELAY_MS, draft_timeout, self);
}

/* Whether keyboard focus is in the composer (its entry or a button). */
static gboolean
focus_within(GhComposer *self)
{
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
  return focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(self));
}

static void
replace_text(GhComposer *self, const gchar *text)
{
  self->setting_text = TRUE;
  gtk_text_buffer_set_text(self->buffer, text ? text : "", -1);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(self->buffer, &end);
  gtk_text_buffer_place_cursor(self->buffer, &end);
  self->setting_text = FALSE;
  cancel_draft_timer(self);
  self->draft_pending = FALSE;
  g_clear_pointer(&self->error, g_free);
  update_state(self);
}

/* ---- the disappearing timer --------------------------------------------------------- */

#define HOUR_S (G_GINT64_CONSTANT(3600))
#define DAY_S  (24 * HOUR_S)
#define WEEK_S (7 * DAY_S)

/* "1 day", "1 week", "4 weeks": the timer in its largest whole unit (the
 * charter's timers are whole days or weeks; anything else is rounded up to
 * a whole hour, so it is never shown shorter than it is). */
static gchar *
describe_timer(gint64 seconds)
{
  if (seconds % WEEK_S == 0) {
    gulong weeks = (gulong)(seconds / WEEK_S);
    return g_strdup_printf(g_dngettext(NULL, "%lu week", "%lu weeks", weeks), weeks);
  }
  if (seconds % DAY_S == 0) {
    gulong days = (gulong)(seconds / DAY_S);
    return g_strdup_printf(g_dngettext(NULL, "%lu day", "%lu days", days), days);
  }
  gulong hours = (gulong)((seconds + HOUR_S - 1) / HOUR_S);
  return g_strdup_printf(g_dngettext(NULL, "%lu hour", "%lu hours", hours), hours);
}

/* Shown only while the timer is on, with its duration in the text, the
 * tooltip and the accessible label. */
static void
update_timer(GhComposer *self)
{
  gboolean on = self->timer > 0;
  gtk_widget_set_visible(GTK_WIDGET(self->timer_slot), on);
  if (!on) {
    gtk_label_set_text(self->timer_label, "");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->timer_button), NULL);
    gtk_accessible_reset_property(GTK_ACCESSIBLE(self->timer_button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL);
    return;
  }
  g_autofree gchar *duration = describe_timer(self->timer);
  /* TRANSLATORS: %s is a duration such as "1 day" or "4 weeks". */
  g_autofree gchar *label = g_strdup_printf(_("Messages you send disappear after %s"), duration);
  gtk_label_set_text(self->timer_label, duration);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->timer_button), label);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->timer_button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                 _("Opens Conversation Info, where the timer can be changed"),
                                 -1);
}

/* ---- input ------------------------------------------------------------------------- */

static gboolean
is_enter(guint keyval)
{
  return keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
         keyval == GDK_KEY_ISO_Enter;
}

/* The text view reports its input method's preedit, "" when it ends
 * (committed or cancelled). */
static void
on_preedit_changed(GhComposer *self, const gchar *preedit)
{
  self->preedit = preedit && *preedit;
}

/* Capture phase, before the text view inserts a newline for Enter. The text
 * view's own key controller (bubble phase) hands every key it gets to its
 * input method first; this one asks the input method itself only for an
 * Enter it would otherwise send or swallow, so no key reaches the input
 * method twice. */
/* Escape cancels a recording in progress (finding 8). */
static gboolean
on_composer_key_pressed(GtkEventControllerKey *controller G_GNUC_UNUSED, guint keyval,
                        guint keycode G_GNUC_UNUSED, GdkModifierType state G_GNUC_UNUSED,
                        GhComposer *self)
{
  if (keyval == GDK_KEY_Escape) {
    const gchar *page = gtk_stack_get_visible_child_name(self->composer_stack);
    if (page && g_str_equal(page, "recording")) {
      g_signal_emit(self, signals[SIGNAL_CANCEL_RECORDING], 0);
      return GDK_EVENT_STOP;
    }
  }
  return GDK_EVENT_PROPAGATE;
}

static void
on_recording_drag_end(GtkGestureDrag *gesture G_GNUC_UNUSED, gdouble offset_x,
                      gdouble offset_y G_GNUC_UNUSED, GhComposer *self)
{
  if (offset_x < -80.0 &&
      g_strcmp0(gtk_stack_get_visible_child_name(self->composer_stack), "recording") == 0)
    g_signal_emit(self, signals[SIGNAL_CANCEL_RECORDING], 0);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, GhComposer *self)
{
  (void)keycode;
  if (!is_enter(keyval))
    return GDK_EVENT_PROPAGATE;
  GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
  /* Enter while an input method composes text (charter §7.7) is the input
   * method's, to commit the preedit: it never sends a half-typed word, nor
   * becomes a newline, whatever the input method answers. */
  if (self->preedit) {
    if (event)
      gtk_text_view_im_context_filter_keypress(self->text_view, event);
    return GDK_EVENT_STOP;
  }
  GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask();
  if (mods != GDK_CONTROL_MASK && (mods != 0 || !self->enter_sends))
    return GDK_EVENT_PROPAGATE; /* Shift+Enter (or Enter without enter-sends): newline */
  /* An input method may take Enter without a preedit (a candidate list). */
  if (event && gtk_text_view_im_context_filter_keypress(self->text_view, event))
    return GDK_EVENT_STOP;
  gh_composer_send(self); /* Ctrl+Enter always sends, never a newline */
  return GDK_EVENT_STOP;
}

static void
on_emoji_picked(GhComposer *self, const gchar *emoji)
{
  gtk_text_buffer_insert_at_cursor(self->buffer, emoji, -1);
  gtk_widget_grab_focus(GTK_WIDGET(self->text_view));
}

static void
action_send(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  gh_composer_send(GH_COMPOSER(widget));
}

/* ---- attachments (G22) -------------------------------------------------------------- */

/* Files are offered to the owner only while the entry is shown and the
 * owner said a file can be sent here. */
static gboolean
attach_possible(GhComposer *self)
{
  return self->can_attach && editable(self);
}

static void
action_attach(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhComposer *self = GH_COMPOSER(widget);
  (void)name;
  (void)parameter;
  if (attach_possible(self))
    g_signal_emit(self, signals[SIGNAL_ATTACH_REQUESTED], 0);
}

static void
update_attach(GhComposer *self)
{
  gboolean possible = attach_possible(self);
  gtk_widget_set_visible(GTK_WIDGET(self->attach_button), self->can_attach);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "composer.attach", possible);
}

/* ---- polls (W26 slice C) ------------------------------------------------------------ */

static gboolean
poll_possible(GhComposer *self)
{
  return self->can_create_poll && editable(self);
}

static void
action_create_poll(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhComposer *self = GH_COMPOSER(widget);
  (void)name;
  (void)parameter;
  if (poll_possible(self))
    g_signal_emit(self, signals[SIGNAL_POLL_REQUESTED], 0);
}

static void
update_poll(GhComposer *self)
{
  gboolean possible = poll_possible(self);
  gtk_widget_set_visible(GTK_WIDGET(self->poll_button), self->can_create_poll);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "composer.create-poll", possible);
}

/* ---- voice recording (W27 slice A, nostrc-o1kl) ------------------------------------ */

static gboolean
voice_possible(GhComposer *self)
{
  return self->can_record_voice && editable(self);
}

static void
action_record_voice(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhComposer *self = GH_COMPOSER(widget);
  (void)name;
  (void)parameter;
  if (voice_possible(self))
    g_signal_emit(self, signals[SIGNAL_RECORD_VOICE_REQUESTED], 0);
}

static void
update_voice(GhComposer *self)
{
  gboolean possible = voice_possible(self);
  gtk_widget_set_visible(GTK_WIDGET(self->voice_button), self->can_record_voice);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "composer.record-voice", possible);
}

/* ---- recording actions (W27 slice A, nostrc-4h64) --------------------------------- */

static void
action_stop_recording(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  g_signal_emit(widget, signals[SIGNAL_STOP_RECORDING], 0);
}

static void
action_cancel_recording(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  g_signal_emit(widget, signals[SIGNAL_CANCEL_RECORDING], 0);
}

/* A dropped file (the first of several) or image. */
static gboolean
on_drop(GtkDropTarget *target, const GValue *value, gdouble x, gdouble y, GhComposer *self)
{
  (void)target;
  (void)x;
  (void)y;
  if (!attach_possible(self))
    return FALSE;
  if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
    GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
    GFile *file = files ? files->data : NULL;
    if (file)
      g_signal_emit(self, signals[SIGNAL_ATTACH_FILE], 0, file);
    g_slist_free(files);
    return file != NULL;
  }
  if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
    g_signal_emit(self, signals[SIGNAL_ATTACH_FILE], 0, g_value_get_object(value));
    return TRUE;
  }
  if (G_VALUE_HOLDS(value, GDK_TYPE_TEXTURE)) {
    g_signal_emit(self, signals[SIGNAL_ATTACH_TEXTURE], 0, g_value_get_object(value));
    return TRUE;
  }
  return FALSE;
}

static GdkDragAction
on_drop_accept(GtkDropTarget *target, GdkDrop *drop, GhComposer *self)
{
  (void)target;
  (void)drop;
  return attach_possible(self) ? GDK_ACTION_COPY : 0;
}

static void
on_texture_pasted(GObject *source, GAsyncResult *result, gpointer data)
{
  GhComposer *self = data; /* a reference */
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkTexture) texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source),
                                                                    result, &error);
  if (texture && attach_possible(self))
    g_signal_emit(self, signals[SIGNAL_ATTACH_TEXTURE], 0, texture);
  else if (!texture)
    g_debug("Composer: a pasted image could not be read: %s", error->message);
  g_object_unref(self);
}

static void
on_files_pasted(GObject *source, GAsyncResult *result, gpointer data)
{
  GhComposer *self = data; /* a reference */
  g_autoptr(GError) error = NULL;
  const GValue *value = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(source), result, &error);
  if (value && G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST) && attach_possible(self)) {
    GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
    if (files)
      g_signal_emit(self, signals[SIGNAL_ATTACH_FILE], 0, files->data);
    g_slist_free(files);
  } else if (!value) {
    g_debug("Composer: pasted files could not be read: %s", error->message);
  }
  g_object_unref(self);
}

/* Paste (Ctrl+V, the context menu) of an image, or of copied files without
 * text, offers them as an attachment; text pastes as always. */
static void
on_paste_clipboard(GtkTextView *text_view, GhComposer *self)
{
  GdkClipboard *clipboard = self->clipboard ? self->clipboard
                                            : gtk_widget_get_clipboard(GTK_WIDGET(text_view));
  GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
  if (attach_possible(self) && gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE)) {
    g_signal_stop_emission_by_name(text_view, "paste-clipboard");
    gdk_clipboard_read_texture_async(clipboard, NULL, on_texture_pasted, g_object_ref(self));
  } else if (attach_possible(self) &&
             gdk_content_formats_contain_gtype(formats, GDK_TYPE_FILE_LIST) &&
             !gdk_content_formats_contain_gtype(formats, G_TYPE_STRING)) {
    g_signal_stop_emission_by_name(text_view, "paste-clipboard");
    gdk_clipboard_read_value_async(clipboard, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, NULL,
                                   on_files_pasted, g_object_ref(self));
  }
  /* Anything else continues to GtkTextView's own paste, as text. */
}

/* ---- GObject ------------------------------------------------------------------------- */

static gboolean
gh_composer_grab_focus(GtkWidget *widget)
{
  GhComposer *self = GH_COMPOSER(widget);
  if (editable(self))
    return gtk_widget_grab_focus(GTK_WIDGET(self->text_view));
  if (gtk_widget_get_visible(GTK_WIDGET(self->disabled_button)))
    return gtk_widget_grab_focus(GTK_WIDGET(self->disabled_button));
  return FALSE;
}

static void
gh_composer_map(GtkWidget *widget)
{
  GTK_WIDGET_CLASS(gh_composer_parent_class)->map(widget);
  update_height(GH_COMPOSER(widget));
}

static void
gh_composer_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GhComposer *self = GH_COMPOSER(object);
  switch (prop_id) {
  case PROP_COMPACT:         g_value_set_boolean(value, self->compact); break;
  case PROP_MAX_LINES:       g_value_set_uint(value, self->max_lines); break;
  case PROP_ENTER_SENDS:     g_value_set_boolean(value, self->enter_sends); break;
  case PROP_DISABLED_REASON: g_value_set_string(value, self->reason); break;
  case PROP_DISAPPEARING_TIMER: g_value_set_int64(value, self->timer); break;
  case PROP_CAN_ATTACH:      g_value_set_boolean(value, self->can_attach); break;
  case PROP_CAN_CREATE_POLL:  g_value_set_boolean(value, self->can_create_poll); break;
  case PROP_CAN_RECORD_VOICE:  g_value_set_boolean(value, self->can_record_voice); break;
  case PROP_CAN_SEND:        g_value_set_boolean(value, self->can_send); break;
  case PROP_TOO_LONG:        g_value_set_boolean(value, self->too_long); break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_composer_set_property(GObject *object, guint prop_id, const GValue *value,
                         GParamSpec *pspec)
{
  GhComposer *self = GH_COMPOSER(object);
  switch (prop_id) {
  case PROP_COMPACT:     gh_composer_set_compact(self, g_value_get_boolean(value)); break;
  case PROP_MAX_LINES:   gh_composer_set_max_lines(self, g_value_get_uint(value)); break;
  case PROP_ENTER_SENDS: gh_composer_set_enter_sends(self, g_value_get_boolean(value)); break;
  case PROP_DISABLED_REASON:
    gh_composer_set_disabled_reason(self, g_value_get_string(value));
    break;
  case PROP_DISAPPEARING_TIMER:
    gh_composer_set_disappearing_timer(self, g_value_get_int64(value));
    break;
  case PROP_CAN_ATTACH:
    gh_composer_set_can_attach(self, g_value_get_boolean(value));
    break;
  case PROP_CAN_CREATE_POLL:
    gh_composer_set_can_create_poll(self, g_value_get_boolean(value));
    break;
  case PROP_CAN_RECORD_VOICE:
    gh_composer_set_can_record_voice(self, g_value_get_boolean(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_composer_dispose(GObject *object)
{
  GhComposer *self = GH_COMPOSER(object);
  /* A draft typed in the last second is still reported (window closed). */
  if (self->buffer)
    gh_composer_flush_draft(self);
  cancel_draft_timer(self);
  if (self->buffer)
    g_signal_handlers_disconnect_by_data(self->buffer, self);
  self->buffer = NULL;
  g_clear_object(&self->clipboard);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_COMPOSER);
  G_OBJECT_CLASS(gh_composer_parent_class)->dispose(object);
}

static void
gh_composer_finalize(GObject *object)
{
  GhComposer *self = GH_COMPOSER(object);
  if (self->length_destroy)
    self->length_destroy(self->length_data);
  g_free(self->reason);
  g_free(self->error);
  G_OBJECT_CLASS(gh_composer_parent_class)->finalize(object);
}

/* The bundled send-symbolic icon without a GtkApplication (tests); the
 * application's resource base path already covers it. */
static void
add_icon_path(void)
{
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return;
  GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
  g_auto(GStrv) paths = gtk_icon_theme_get_resource_path(theme);
  if (!paths || !g_strv_contains((const gchar *const *)paths, ICON_RESOURCE_PATH))
    gtk_icon_theme_add_resource_path(theme, ICON_RESOURCE_PATH);
}

static void
gh_composer_class_init(GhComposerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->get_property = gh_composer_get_property;
  object_class->set_property = gh_composer_set_property;
  object_class->dispose = gh_composer_dispose;
  object_class->finalize = gh_composer_finalize;
  widget_class->grab_focus = gh_composer_grab_focus;
  widget_class->map = gh_composer_map;

  const GParamFlags rw = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  const GParamFlags ro = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  props[PROP_COMPACT] = g_param_spec_boolean("compact", NULL, NULL, FALSE, rw);
  props[PROP_MAX_LINES] = g_param_spec_uint("max-lines", NULL, NULL, 1, 100,
                                            GH_COMPOSER_DEFAULT_MAX_LINES, rw);
  props[PROP_ENTER_SENDS] = g_param_spec_boolean("enter-sends", NULL, NULL, TRUE, rw);
  props[PROP_DISABLED_REASON] = g_param_spec_string("disabled-reason", NULL, NULL, NULL, rw);
  props[PROP_DISAPPEARING_TIMER] = g_param_spec_int64("disappearing-timer", NULL, NULL, 0,
                                                      G_MAXINT64, 0, rw);
  props[PROP_CAN_ATTACH] = g_param_spec_boolean("can-attach", NULL, NULL, FALSE, rw);
  props[PROP_CAN_CREATE_POLL] = g_param_spec_boolean("can-create-poll", NULL, NULL, FALSE, rw);
  props[PROP_CAN_RECORD_VOICE] = g_param_spec_boolean("can-record-voice", NULL, NULL, FALSE, rw);
  props[PROP_CAN_SEND] = g_param_spec_boolean("can-send", NULL, NULL, FALSE, ro);
  props[PROP_TOO_LONG] = g_param_spec_boolean("too-long", NULL, NULL, FALSE, ro);
  g_object_class_install_properties(object_class, N_PROPS, props);

  signals[SIGNAL_SEND] =
    g_signal_new("send", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                 g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 1,
                 G_TYPE_STRING);
  signals[SIGNAL_DRAFT_CHANGED] =
    g_signal_new("draft-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_ATTACH_REQUESTED] =
    g_signal_new("attach-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_ATTACH_FILE] =
    g_signal_new("attach-file", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 1, G_TYPE_FILE);
  signals[SIGNAL_ATTACH_TEXTURE] =
    g_signal_new("attach-texture", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 1, GDK_TYPE_TEXTURE);
  signals[SIGNAL_POLL_REQUESTED] =
    g_signal_new("poll-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_RECORD_VOICE_REQUESTED] =
    g_signal_new("record-voice-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_STOP_RECORDING] =
    g_signal_new("stop-recording", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_CANCEL_RECORDING] =
    g_signal_new("cancel-recording", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 0);

  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-composer.ui");
  gtk_widget_class_bind_template_child(widget_class, GhComposer, composer_stack);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, error_label);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, attach_button);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, voice_button);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, poll_button);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, timer_slot);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, timer_button);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, timer_label);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, scroller);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, text_view);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, placeholder);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, emoji_button);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, emoji_chooser);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, send_button);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, disabled_reason);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, disabled_button);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, recording_icon);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, recording_label);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, recording_time);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, recording_level);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, cancel_recording_button);
  gtk_widget_class_bind_template_child(widget_class, GhComposer, stop_recording_button);
  gtk_widget_class_install_action(widget_class, "composer.send", NULL, action_send);
  gtk_widget_class_install_action(widget_class, "composer.attach", NULL, action_attach);
  gtk_widget_class_install_action(widget_class, "composer.create-poll", NULL, action_create_poll);
  gtk_widget_class_install_action(widget_class, "composer.record-voice", NULL, action_record_voice);
  gtk_widget_class_install_action(widget_class, "composer.stop-recording", NULL, action_stop_recording);
  gtk_widget_class_install_action(widget_class, "composer.cancel-recording", NULL, action_cancel_recording);
  gtk_widget_class_set_css_name(widget_class, "composer");
  add_icon_path();
}

static void
gh_composer_init(GhComposer *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->enter_sends = TRUE;
  self->max_lines = GH_COMPOSER_DEFAULT_MAX_LINES;
  self->buffer = gtk_text_view_get_buffer(self->text_view);
  g_signal_connect_swapped(self->buffer, "changed", G_CALLBACK(on_buffer_changed), self);
  g_signal_connect_swapped(self->emoji_chooser, "emoji-picked", G_CALLBACK(on_emoji_picked),
                           self);
  g_signal_connect_swapped(self->text_view, "preedit-changed", G_CALLBACK(on_preedit_changed),
                           self);

  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_name(keys, "groundhog-composer-keys");
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self->text_view), keys);

  /* G22: a file or an image dropped on the composer, or an image pasted
   * into it, is offered for sending (the owner shows what will be sent). */
  GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
  GType types[] = { GDK_TYPE_FILE_LIST, G_TYPE_FILE, GDK_TYPE_TEXTURE };
  gtk_drop_target_set_gtypes(drop, types, G_N_ELEMENTS(types));
  gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(drop), "groundhog-composer-drop");
  g_signal_connect(drop, "accept", G_CALLBACK(on_drop_accept), self);
  g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drop));
  g_signal_connect(self->text_view, "paste-clipboard", G_CALLBACK(on_paste_clipboard), self);

  /* Escape cancels a recording (finding 8). This controller is on the
   * whole composer so it catches Escape even when the text view is not
   * focused (the recording page has buttons, not text). */
  GtkEventController *composer_keys = gtk_event_controller_key_new();
  gtk_event_controller_set_name(composer_keys, "groundhog-composer-escape");
  gtk_event_controller_set_propagation_phase(composer_keys, GTK_PHASE_CAPTURE);
  g_signal_connect(composer_keys, "key-pressed", G_CALLBACK(on_composer_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), composer_keys);

  /* Swipe left across the recording status to discard without sending. */
  GtkGesture *recording_drag = gtk_gesture_drag_new();
  gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(recording_drag),
                                "groundhog-recording-swipe");
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(recording_drag),
                                              GTK_PHASE_CAPTURE);
  g_signal_connect(recording_drag, "drag-end", G_CALLBACK(on_recording_drag_end), self);
  gtk_widget_add_controller(GTK_WIDGET(self->recording_label),
                            GTK_EVENT_CONTROLLER(recording_drag));

  gtk_stack_set_visible_child_name(self->composer_stack, "edit");
  update_state(self);
  update_timer(self);
  update_attach(self);
  update_voice(self);
}

/* ---- public ------------------------------------------------------------------------ */

GtkWidget *
gh_composer_new(void)
{
  return g_object_new(GH_TYPE_COMPOSER, NULL);
}

gchar *
gh_composer_dup_text(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), NULL);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(self->buffer, &start, &end);
  return gtk_text_buffer_get_text(self->buffer, &start, &end, FALSE);
}

void
gh_composer_set_text(GhComposer *self, const gchar *text)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  replace_text(self, text);
}

void
gh_composer_flush_draft(GhComposer *self)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  cancel_draft_timer(self);
  if (!self->draft_pending)
    return;
  self->draft_pending = FALSE;
  g_autofree gchar *text = gh_composer_dup_text(self);
  g_signal_emit(self, signals[SIGNAL_DRAFT_CHANGED], 0, text);
}

gboolean
gh_composer_get_draft_pending(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), FALSE);
  return self->draft_pending;
}

gboolean
gh_composer_send(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), FALSE);
  update_state(self);
  if (!self->can_send)
    return FALSE;
  g_autofree gchar *text = gh_composer_dup_text(self);
  /* Sent from within the composer (Enter, or the Send button reached with
   * Tab), keyboard focus stays in or returns to the entry (charter §7.14);
   * checked first, as the emptied composer makes Send insensitive, which
   * takes focus off it. */
  gboolean had_focus = focus_within(self);
  gboolean queued = FALSE;
  g_signal_emit(self, signals[SIGNAL_SEND], 0, text, &queued);
  if (!queued) {
    update_state(self); /* the owner may have set an error */
    return FALSE;
  }
  /* Queueing cleared the stored draft: nothing is reported for this. */
  replace_text(self, NULL);
  if (had_focus)
    gtk_widget_grab_focus(GTK_WIDGET(self->text_view));
  return TRUE;
}

gboolean
gh_composer_get_can_send(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), FALSE);
  return self->can_send;
}

gboolean
gh_composer_get_too_long(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), FALSE);
  return self->too_long;
}

void
gh_composer_set_length_func(GhComposer *self, GhComposerLengthFunc func, gpointer user_data,
                            GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  if (self->length_destroy)
    self->length_destroy(self->length_data);
  self->length_func = func;
  self->length_data = user_data;
  self->length_destroy = destroy;
  update_state(self);
}

void
gh_composer_revalidate(GhComposer *self)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  update_state(self);
}

void
gh_composer_set_error(GhComposer *self, const gchar *message)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  g_free(self->error);
  self->error = message && *message ? g_strdup(message) : NULL;
  update_state(self);
}

const gchar *
gh_composer_get_error(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), NULL);
  return self->too_long ? gtk_label_get_text(self->error_label) : self->error;
}

void
gh_composer_set_disabled_reason(GhComposer *self, const gchar *reason)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  if (reason && !*reason)
    reason = NULL;
  if (g_strcmp0(reason, self->reason) == 0)
    return;
  g_free(self->reason);
  self->reason = g_strdup(reason);
  gtk_label_set_text(self->disabled_reason, reason ? reason : "");
  gtk_stack_set_visible_child_name(self->composer_stack, reason ? "disabled" : "edit");
  update_state(self);
  update_attach(self);
  update_voice(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_DISABLED_REASON]);
}

const gchar *
gh_composer_get_disabled_reason(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), NULL);
  return self->reason;
}

void
gh_composer_set_disabled_action(GhComposer *self, const gchar *label,
                                const gchar *detailed_action)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  gboolean shown = label && *label && detailed_action;
  gtk_widget_set_visible(GTK_WIDGET(self->disabled_button), shown);
  if (!shown) {
    gtk_actionable_set_action_name(GTK_ACTIONABLE(self->disabled_button), NULL);
    return;
  }
  gtk_button_set_label(self->disabled_button, label);
  gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(self->disabled_button),
                                          detailed_action);
}

void
gh_composer_set_compact(GhComposer *self, gboolean compact)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  compact = !!compact;
  if (self->compact == compact)
    return;
  self->compact = compact;
  /* Ctrl+. and Ctrl+; still open the emoji chooser. */
  gtk_widget_set_visible(GTK_WIDGET(self->emoji_button), !compact);
  if (compact)
    gtk_widget_add_css_class(GTK_WIDGET(self), "compact");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self), "compact");
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_COMPACT]);
}

gboolean
gh_composer_get_compact(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), FALSE);
  return self->compact;
}

void
gh_composer_set_max_lines(GhComposer *self, guint max_lines)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  max_lines = CLAMP(max_lines, 1, 100);
  if (self->max_lines == max_lines)
    return;
  self->max_lines = max_lines;
  update_height(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_MAX_LINES]);
}

guint
gh_composer_get_max_lines(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), GH_COMPOSER_DEFAULT_MAX_LINES);
  return self->max_lines;
}

void
gh_composer_set_enter_sends(GhComposer *self, gboolean enter_sends)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  enter_sends = !!enter_sends;
  if (self->enter_sends == enter_sends)
    return;
  self->enter_sends = enter_sends;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ENTER_SENDS]);
}

gboolean
gh_composer_get_enter_sends(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), TRUE);
  return self->enter_sends;
}

GtkTextView *
gh_composer_get_text_view(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), NULL);
  return self->text_view;
}

void
gh_composer_set_clipboard(GhComposer *self, GdkClipboard *clipboard)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  g_return_if_fail(clipboard == NULL || GDK_IS_CLIPBOARD(clipboard));
  g_set_object(&self->clipboard, clipboard);
}

void
gh_composer_set_disappearing_timer(GhComposer *self, gint64 seconds)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  seconds = MAX(seconds, 0);
  if (self->timer == seconds)
    return;
  self->timer = seconds;
  update_timer(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_DISAPPEARING_TIMER]);
}

gint64
gh_composer_get_disappearing_timer(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), 0);
  return self->timer;
}

GtkBox *
gh_composer_get_timer_slot(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), NULL);
  return self->timer_slot;
}

void
gh_composer_set_can_attach(GhComposer *self, gboolean can_attach)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  can_attach = !!can_attach;
  if (self->can_attach == can_attach)
    return;
  self->can_attach = can_attach;
  update_attach(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_ATTACH]);
}

gboolean
gh_composer_get_can_attach(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), FALSE);
  return self->can_attach;
}

GtkButton *
gh_composer_get_attach_button(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), NULL);
  return self->attach_button;
}

void
gh_composer_set_can_create_poll(GhComposer *self, gboolean can_create_poll)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  can_create_poll = !!can_create_poll;
  if (self->can_create_poll == can_create_poll)
    return;
  self->can_create_poll = can_create_poll;
  update_poll(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_CREATE_POLL]);
}

gboolean
gh_composer_get_can_create_poll(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), FALSE);
  return self->can_create_poll;
}

void
gh_composer_set_can_record_voice(GhComposer *self, gboolean can_record_voice)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  can_record_voice = !!can_record_voice;
  if (self->can_record_voice == can_record_voice)
    return;
  self->can_record_voice = can_record_voice;
  update_voice(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_RECORD_VOICE]);
}

gboolean
gh_composer_get_can_record_voice(GhComposer *self)
{
  g_return_val_if_fail(GH_IS_COMPOSER(self), FALSE);
  return self->can_record_voice;
}

/* ---- recording overlay (W27, nostrc-4h64) ------------------------------------------ */

static gchar *
format_recording_time(gdouble seconds)
{
  gint total = (gint)seconds;
  gint min = total / 60;
  gint sec = total % 60;
  return g_strdup_printf("%d:%02d", min, sec);
}

void
gh_composer_show_recording(GhComposer *self)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  gtk_stack_set_visible_child_name(self->composer_stack, "recording");
  gtk_label_set_text(self->recording_time, "0:00");
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->recording_time),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Recording duration 0:00"), -1);
  gtk_level_bar_set_value(self->recording_level, 0);
}

void
gh_composer_hide_recording(GhComposer *self)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  gtk_stack_set_visible_child_name(self->composer_stack, "edit");
}

void
gh_composer_set_recording_level(GhComposer *self, gdouble level)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  gtk_level_bar_set_value(self->recording_level, CLAMP(level, 0.0, 1.0));
}

void
gh_composer_set_recording_time(GhComposer *self, gdouble seconds)
{
  g_return_if_fail(GH_IS_COMPOSER(self));
  g_autofree gchar *text = format_recording_time(seconds);
  gtk_label_set_text(self->recording_time, text);
  g_autofree gchar *announcement = g_strdup_printf(_("Recording duration %s"), text);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->recording_time),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, announcement, -1);
}
