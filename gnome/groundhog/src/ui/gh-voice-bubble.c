/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-voice-bubble.h"
#include "gh-voice-meta.h"

#include <glib/gi18n.h>
#include <math.h>

/* GQuark for provider data on an ancestor widget. */
static GQuark voice_bubble_provider_quark;

struct _GhVoiceBubble {
  GtkWidget parent_instance;

  /* Template children. */
  GtkBox *content_box;
  GtkBox *download_box;
  GtkBox *player_box;
  GtkImage *download_icon;
  GtkLabel *type_label;
  GtkLabel *size_label;
  GtkButton *download_button;
  GtkSpinner *download_spinner;
  GtkButton *cancel_button;
  GtkButton *play_button;
  GtkDrawingArea *waveform_area;
  GtkLabel *position_label;
  GtkLabel *duration_label;
  GtkButton *speed_button;
  GtkLabel *error_label;

  /* Data. */
  GhMessage *message;
  guint index;
  gboolean compact;

  /* Transfer observation (finding 1). */
  GhAttachmentTransfer *transfer;
  gulong transfer_notify_handler;

  /* Playback. */
  GhVoicePlayer *player;
  gulong player_state_handler;
  gulong player_position_handler;
  gulong player_waveform_handler;
  gulong player_error_handler;
};

enum {
  PROP_0,
  PROP_MESSAGE,
  PROP_INDEX,
  PROP_COMPACT,
  PROP_SUMMARY,
  N_PROPS
};

static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhVoiceBubble, gh_voice_bubble, GTK_TYPE_WIDGET)

static void gh_voice_bubble_bind_transfer(GhVoiceBubble *self);
static void gh_voice_bubble_unbind_transfer(GhVoiceBubble *self);
static void gh_voice_bubble_update_for_transfer(GhVoiceBubble *self);

/* ---- provider lookup ---- */

typedef struct {
  GhVoiceBubbleProvider provider;
  gpointer data;
  GDestroyNotify destroy;
} VoiceBubbleProviderData;

static void
provider_data_free(gpointer p)
{
  VoiceBubbleProviderData *pd = p;
  if (pd->destroy)
    pd->destroy(pd->data);
  g_free(pd);
}

void
gh_voice_bubble_set_provider(GtkWidget *widget, const GhVoiceBubbleProvider *provider,
                              gpointer data, GDestroyNotify destroy)
{
  if (!voice_bubble_provider_quark)
    voice_bubble_provider_quark = g_quark_from_static_string("gh-voice-bubble-provider");

  if (!provider) {
    g_object_set_qdata(G_OBJECT(widget), voice_bubble_provider_quark, NULL);
    return;
  }

  VoiceBubbleProviderData *pd = g_new0(VoiceBubbleProviderData, 1);
  pd->provider = *provider;
  pd->data = data;
  pd->destroy = destroy;
  g_object_set_qdata_full(G_OBJECT(widget), voice_bubble_provider_quark, pd,
                          provider_data_free);
}

static VoiceBubbleProviderData *
find_provider(GtkWidget *widget)
{
  if (!voice_bubble_provider_quark)
    return NULL;
  for (GtkWidget *w = widget; w; w = gtk_widget_get_parent(w)) {
    VoiceBubbleProviderData *pd =
      g_object_get_qdata(G_OBJECT(w), voice_bubble_provider_quark);
    if (pd)
      return pd;
  }
  return NULL;
}

/* ---- helpers ---- */

static gchar *
format_time(gdouble seconds)
{
  gint total = (gint)seconds;
  gint min = total / 60;
  gint sec = total % 60;
  return g_strdup_printf("%d:%02d", min, sec);
}

static void
gh_voice_bubble_update_time_labels(GhVoiceBubble *self)
{
  if (self->player) {
    g_autofree gchar *pos = format_time(gh_voice_player_get_position(self->player));
    g_autofree gchar *dur = format_time(gh_voice_player_get_duration(self->player));
    gtk_label_set_text(self->position_label, pos);
    gtk_label_set_text(self->duration_label, dur);
  }
}

/* ---- waveform drawing ---- */

static void
waveform_draw_func(GtkDrawingArea *area G_GNUC_UNUSED, cairo_t *cr, int width, int height,
                   gpointer user_data)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(user_data);

  GhVoiceWaveform *waveform = self->player ?
    gh_voice_player_dup_waveform(self->player) : NULL;

  /* Default appearance: dim bars. */
  GdkRGBA bar_color = { 0.5, 0.5, 0.5, 0.5 };
  GdkRGBA played_color = { 0.2, 0.6, 1.0, 0.8 };

  gdouble position_frac = 0.0;
  if (self->player) {
    gdouble dur = gh_voice_player_get_duration(self->player);
    gdouble pos = gh_voice_player_get_position(self->player);
    if (dur > 0.0)
      position_frac = pos / dur;
  }

  guint n_samples = waveform ? gh_voice_waveform_get_n_samples(waveform) : 0;
  const gdouble *samples = waveform ? gh_voice_waveform_get_samples(waveform) : NULL;

  if (n_samples == 0) {
    /* No waveform: draw a simple line. */
    gdk_cairo_set_source_rgba(cr, &bar_color);
    cairo_move_to(cr, 0, height / 2.0);
    cairo_line_to(cr, width, height / 2.0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    g_clear_pointer(&waveform, gh_voice_waveform_unref);
    return;
  }

  gdouble bar_width = (gdouble)width / n_samples;
  gdouble gap = MAX(bar_width * 0.3, 1.0);
  gdouble actual_bar = bar_width - gap;
  if (actual_bar < 1.0)
    actual_bar = 1.0;

  gdouble min_bar_height = 2.0;

  for (guint i = 0; i < n_samples; i++) {
    gdouble frac = (gdouble)i / n_samples;
    gdouble amplitude = samples[i];
    gdouble bar_h = MAX(amplitude * height, min_bar_height);
    gdouble x = i * bar_width + gap / 2.0;
    gdouble y = (height - bar_h) / 2.0;

    if (frac <= position_frac)
      gdk_cairo_set_source_rgba(cr, &played_color);
    else
      gdk_cairo_set_source_rgba(cr, &bar_color);

    cairo_rectangle(cr, x, y, actual_bar, bar_h);
    cairo_fill(cr);
  }

  g_clear_pointer(&waveform, gh_voice_waveform_unref);
}

/* ---- player signal handlers ---- */

static void
on_player_position(GhVoicePlayer *player G_GNUC_UNUSED, gdouble position G_GNUC_UNUSED,
                   gpointer user_data)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(user_data);
  gh_voice_bubble_update_time_labels(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->waveform_area));
}

static void
on_player_state(GhVoicePlayer *player G_GNUC_UNUSED, guint state, gpointer user_data)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(user_data);

  const gchar *icon = (state == GH_VOICE_PLAYER_STATE_PLAYING)
    ? "media-playback-pause-symbolic"
    : "media-playback-start-symbolic";
  gtk_button_set_icon_name(self->play_button, icon);

  /* Accessibility: update both tooltip and accessible label (finding 8). */
  const gchar *label = (state == GH_VOICE_PLAYER_STATE_PLAYING)
    ? _("Pause voice message")
    : _("Play voice message");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->play_button), label);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->play_button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);

  /* Update time labels after stop (EOS resets position to 0). */
  gh_voice_bubble_update_time_labels(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->waveform_area));
}

static void
on_player_waveform(GhVoicePlayer *player G_GNUC_UNUSED, gpointer user_data)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(user_data);
  gtk_widget_queue_draw(GTK_WIDGET(self->waveform_area));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->waveform_area),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Voice waveform"), -1);
}

static void
on_player_error(GhVoicePlayer *player G_GNUC_UNUSED, const gchar *message,
                gpointer user_data)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(user_data);
  gtk_label_set_text(self->error_label, _(message));
  gtk_widget_set_visible(GTK_WIDGET(self->error_label), TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->download_box), TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->player_box), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->download_button), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->download_spinner), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->cancel_button), FALSE);
}

/* ---- transfer observation (finding 1) ---- */

static void
on_transfer_notify(GhVoiceBubble *self)
{
  gh_voice_bubble_update_for_transfer(self);
}

static void
gh_voice_bubble_unbind_transfer(GhVoiceBubble *self)
{
  if (self->transfer) {
    g_clear_signal_handler(&self->transfer_notify_handler, self->transfer);
    g_clear_object(&self->transfer);
  }
}

static void
gh_voice_bubble_bind_transfer(GhVoiceBubble *self)
{
  gh_voice_bubble_unbind_transfer(self);
  if (!self->message)
    return;

  VoiceBubbleProviderData *pd = find_provider(GTK_WIDGET(self));
  if (!pd)
    return;

  GhAttachmentTransfer *transfer = pd->provider.lookup_at
    ? pd->provider.lookup_at(self->message, self->index, pd->data)
    : pd->provider.lookup(self->message, pd->data);

  if (transfer) {
    self->transfer = g_object_ref(transfer);
    self->transfer_notify_handler =
      g_signal_connect_object(transfer, "notify", G_CALLBACK(on_transfer_notify), self,
                              G_CONNECT_SWAPPED);
  }
  gh_voice_bubble_update_for_transfer(self);
}

static void
gh_voice_bubble_update_for_transfer(GhVoiceBubble *self)
{
  if (!self->transfer) {
    gtk_widget_set_visible(GTK_WIDGET(self->download_box), self->message != NULL);
    gtk_widget_set_visible(GTK_WIDGET(self->player_box), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->error_label), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->download_button), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->download_spinner), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->cancel_button), FALSE);
    return;
  }

  GhAttachmentState state = gh_attachment_transfer_get_state(self->transfer);

  switch (state) {
  case GH_ATTACHMENT_STATE_IDLE:
    if (self->player) {
      gh_voice_player_stop(self->player);
      g_clear_signal_handler(&self->player_state_handler, self->player);
      g_clear_signal_handler(&self->player_position_handler, self->player);
      g_clear_signal_handler(&self->player_waveform_handler, self->player);
      g_clear_signal_handler(&self->player_error_handler, self->player);
      g_clear_object(&self->player);
    }
    gtk_widget_set_visible(GTK_WIDGET(self->download_box), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->player_box), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->error_label), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->download_button), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->download_spinner), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->cancel_button), FALSE);
    break;

  case GH_ATTACHMENT_STATE_DOWNLOADING:
    gtk_widget_set_visible(GTK_WIDGET(self->download_box), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->player_box), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->error_label), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->download_button), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->download_spinner), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->cancel_button), TRUE);
    break;

  case GH_ATTACHMENT_STATE_READY: {
    /* Transition to playback: load the audio bytes (finding 1). */
    GBytes *plaintext = gh_attachment_transfer_get_plaintext(self->transfer);
    if (plaintext && !self->player)
      gh_voice_bubble_load_audio(self, plaintext);
    gtk_widget_set_visible(GTK_WIDGET(self->download_spinner), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->cancel_button), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->download_button), FALSE);
    break;
  }

  case GH_ATTACHMENT_STATE_FAILED: {
    const gchar *error = gh_attachment_transfer_get_error(self->transfer);
    gtk_label_set_text(self->error_label, error ? error : _("Download failed"));
    gtk_widget_set_visible(GTK_WIDGET(self->error_label), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->download_box), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->player_box), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->download_button),
                           gh_attachment_transfer_get_can_retry(self->transfer));
    gtk_widget_set_visible(GTK_WIDGET(self->download_spinner), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->cancel_button), FALSE);
    break;
  }
  }
}

/* ---- actions ---- */

static void
voice_play_action(GtkWidget *widget, const gchar *action_name G_GNUC_UNUSED,
                  GVariant *param G_GNUC_UNUSED)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(widget);
  if (!self->player)
    return;

  GhVoicePlayerState state = gh_voice_player_get_state(self->player);
  if (state == GH_VOICE_PLAYER_STATE_PLAYING)
    gh_voice_player_pause(self->player);
  else
    gh_voice_player_play(self->player);
}

static void
voice_cycle_speed_action(GtkWidget *widget, const gchar *action_name G_GNUC_UNUSED,
                         GVariant *param G_GNUC_UNUSED)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(widget);
  if (!self->player)
    return;

  gdouble speed = gh_voice_player_get_speed(self->player);
  if (speed < 1.25)
    speed = 1.5;
  else if (speed < 1.75)
    speed = 2.0;
  else
    speed = 1.0;

  gh_voice_player_set_speed(self->player, speed);

  g_autofree gchar *label = NULL;
  if (speed == 1.0)
    label = g_strdup("1\xC3\x97");
  else if (speed == 1.5)
    label = g_strdup("1.5\xC3\x97");
  else
    label = g_strdup("2\xC3\x97");
  gtk_button_set_label(self->speed_button, label);

  /* Accessibility: announce the selected speed (finding 8). */
  g_autofree gchar *a11y = g_strdup_printf(_("Playback speed %s"), label);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->speed_button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, a11y, -1);
}

static void
voice_download_action(GtkWidget *widget, const gchar *action_name G_GNUC_UNUSED,
                      GVariant *param G_GNUC_UNUSED)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(widget);

  /* Bind transfer if not yet bound (e.g. first interaction after rooting). */
  if (!self->transfer)
    gh_voice_bubble_bind_transfer(self);

  VoiceBubbleProviderData *pd = find_provider(widget);
  if (!pd || !self->transfer)
    return;

  if (pd->provider.download)
    pd->provider.download(self->transfer, pd->data);
}

static void
voice_cancel_action(GtkWidget *widget, const gchar *action_name G_GNUC_UNUSED,
                    GVariant *param G_GNUC_UNUSED)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(widget);
  VoiceBubbleProviderData *pd = find_provider(widget);
  if (!pd || !self->transfer)
    return;

  if (pd->provider.cancel)
    pd->provider.cancel(self->transfer, pd->data);
}

/* ---- GObject ---- */

static void
gh_voice_bubble_root(GtkWidget *widget)
{
  GTK_WIDGET_CLASS(gh_voice_bubble_parent_class)->root(widget);
  GhVoiceBubble *self = GH_VOICE_BUBBLE(widget);
  gh_voice_bubble_bind_transfer(self);
}

static void
gh_voice_bubble_unroot(GtkWidget *widget)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(widget);
  gh_voice_bubble_unbind_transfer(self);
  GTK_WIDGET_CLASS(gh_voice_bubble_parent_class)->unroot(widget);
}

static void
gh_voice_bubble_dispose(GObject *object)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(object);

  gh_voice_bubble_unbind_transfer(self);

  if (self->player) {
    gh_voice_player_stop(self->player);
    g_clear_signal_handler(&self->player_state_handler, self->player);
    g_clear_signal_handler(&self->player_position_handler, self->player);
    g_clear_signal_handler(&self->player_waveform_handler, self->player);
    g_clear_signal_handler(&self->player_error_handler, self->player);
    g_clear_object(&self->player);
  }

  g_clear_object(&self->message);
  gtk_widget_dispose_template(GTK_WIDGET(self), GH_TYPE_VOICE_BUBBLE);

  G_OBJECT_CLASS(gh_voice_bubble_parent_class)->dispose(object);
}

static void
gh_voice_bubble_get_property(GObject *object, guint prop_id, GValue *value,
                              GParamSpec *pspec)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(object);
  switch (prop_id) {
  case PROP_MESSAGE:
    g_value_set_object(value, self->message);
    break;
  case PROP_INDEX:
    g_value_set_uint(value, self->index);
    break;
  case PROP_COMPACT:
    g_value_set_boolean(value, self->compact);
    break;
  case PROP_SUMMARY:
    g_value_set_string(value, gh_voice_bubble_get_summary(self));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_voice_bubble_set_property(GObject *object, guint prop_id, const GValue *value,
                              GParamSpec *pspec)
{
  GhVoiceBubble *self = GH_VOICE_BUBBLE(object);
  switch (prop_id) {
  case PROP_MESSAGE:
    gh_voice_bubble_set_message(self, g_value_get_object(value));
    break;
  case PROP_INDEX:
    gh_voice_bubble_set_index(self, g_value_get_uint(value));
    break;
  case PROP_COMPACT:
    gh_voice_bubble_set_compact(self, g_value_get_boolean(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_voice_bubble_class_init(GhVoiceBubbleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gh_voice_bubble_dispose;
  object_class->get_property = gh_voice_bubble_get_property;
  object_class->set_property = gh_voice_bubble_set_property;

  widget_class->root = gh_voice_bubble_root;
  widget_class->unroot = gh_voice_bubble_unroot;

  props[PROP_MESSAGE] =
    g_param_spec_object("message", NULL, NULL, GH_TYPE_MESSAGE,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_INDEX] =
    g_param_spec_uint("index", NULL, NULL, 0, G_MAXUINT, 0,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_COMPACT] =
    g_param_spec_boolean("compact", NULL, NULL, FALSE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  props[PROP_SUMMARY] =
    g_param_spec_string("summary", NULL, NULL, NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  gtk_widget_class_set_template_from_resource(
    widget_class, "/org/nostr/Groundhog/ui/gh-voice-bubble.ui");
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, content_box);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, download_box);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, player_box);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, download_icon);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, type_label);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, size_label);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, download_button);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, download_spinner);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, cancel_button);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, play_button);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, waveform_area);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, position_label);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, duration_label);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, speed_button);
  gtk_widget_class_bind_template_child(widget_class, GhVoiceBubble, error_label);

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "groundhog-voice-bubble");

  gtk_widget_class_install_action(widget_class, "voice.play", NULL, voice_play_action);
  gtk_widget_class_install_action(widget_class, "voice.cycle-speed", NULL,
                                  voice_cycle_speed_action);
  gtk_widget_class_install_action(widget_class, "voice.download", NULL,
                                  voice_download_action);
  gtk_widget_class_install_action(widget_class, "voice.cancel", NULL,
                                  voice_cancel_action);
}

static void
gh_voice_bubble_init(GhVoiceBubble *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_drawing_area_set_draw_func(self->waveform_area, waveform_draw_func, self, NULL);
}

GtkWidget *
gh_voice_bubble_new(void)
{
  return g_object_new(GH_TYPE_VOICE_BUBBLE, NULL);
}

void
gh_voice_bubble_set_message(GhVoiceBubble *self, GhMessage *message)
{
  g_return_if_fail(GH_IS_VOICE_BUBBLE(self));

  if (self->message == message)
    return;

  /* Stop playback of the previous message. */
  if (self->player) {
    gh_voice_player_stop(self->player);
    g_clear_signal_handler(&self->player_state_handler, self->player);
    g_clear_signal_handler(&self->player_position_handler, self->player);
    g_clear_signal_handler(&self->player_waveform_handler, self->player);
    g_clear_signal_handler(&self->player_error_handler, self->player);
    g_clear_object(&self->player);
  }

  gh_voice_bubble_unbind_transfer(self);
  g_set_object(&self->message, message);

  /* Show download state. */
  gtk_widget_set_visible(GTK_WIDGET(self->download_box), message != NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->player_box), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->error_label), FALSE);

  /* Bind the transfer if we are rooted. */
  if (gtk_widget_get_root(GTK_WIDGET(self)))
    gh_voice_bubble_bind_transfer(self);

  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_MESSAGE]);
}

GhMessage *
gh_voice_bubble_get_message(GhVoiceBubble *self)
{
  g_return_val_if_fail(GH_IS_VOICE_BUBBLE(self), NULL);
  return self->message;
}

void
gh_voice_bubble_set_index(GhVoiceBubble *self, guint index)
{
  g_return_if_fail(GH_IS_VOICE_BUBBLE(self));
  if (self->index == index)
    return;
  self->index = index;
  if (self->message && gtk_widget_get_root(GTK_WIDGET(self)))
    gh_voice_bubble_bind_transfer(self);
}

guint
gh_voice_bubble_get_index(GhVoiceBubble *self)
{
  g_return_val_if_fail(GH_IS_VOICE_BUBBLE(self), 0);
  return self->index;
}

void
gh_voice_bubble_load_audio(GhVoiceBubble *self, GBytes *audio_bytes)
{
  g_return_if_fail(GH_IS_VOICE_BUBBLE(self));
  g_return_if_fail(audio_bytes != NULL);

  /* Tear down any previous player. */
  if (self->player) {
    gh_voice_player_stop(self->player);
    g_clear_signal_handler(&self->player_state_handler, self->player);
    g_clear_signal_handler(&self->player_position_handler, self->player);
    g_clear_signal_handler(&self->player_waveform_handler, self->player);
    g_clear_signal_handler(&self->player_error_handler, self->player);
    g_clear_object(&self->player);
  }

  self->player = gh_voice_player_new();
  GError *error = NULL;
  if (!gh_voice_player_load(self->player, audio_bytes, NULL, &error)) {
    gtk_label_set_text(self->error_label, error ? error->message : "Load failed");
    gtk_widget_set_visible(GTK_WIDGET(self->error_label), TRUE);
    g_clear_error(&error);
    g_clear_object(&self->player);
    return;
  }

  self->player_state_handler =
    g_signal_connect(self->player, "state-changed",
                     G_CALLBACK(on_player_state), self);
  self->player_position_handler =
    g_signal_connect(self->player, "position-updated",
                     G_CALLBACK(on_player_position), self);
  self->player_waveform_handler =
    g_signal_connect(self->player, "waveform-ready",
                     G_CALLBACK(on_player_waveform), self);
  self->player_error_handler =
    g_signal_connect(self->player, "playback-error",
                     G_CALLBACK(on_player_error), self);

  /* Switch from download to player view. */
  gtk_widget_set_visible(GTK_WIDGET(self->download_box), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->player_box), TRUE);
  gh_voice_bubble_update_time_labels(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->waveform_area));
}

void
gh_voice_bubble_set_compact(GhVoiceBubble *self, gboolean compact)
{
  g_return_if_fail(GH_IS_VOICE_BUBBLE(self));
  self->compact = compact;
}

const gchar *
gh_voice_bubble_get_summary(GhVoiceBubble *self)
{
  g_return_val_if_fail(GH_IS_VOICE_BUBBLE(self), NULL);
  if (self->player)
    return "Voice message";
  return "Voice message. Not downloaded.";
}
