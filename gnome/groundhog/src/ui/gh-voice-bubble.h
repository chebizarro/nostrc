/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GH_VOICE_BUBBLE_H
#define GH_VOICE_BUBBLE_H

#include <adwaita.h>

#include "gh-attachment-transfer.h"
#include "gh-message.h"
#include "gh-voice-player.h"
#include "gh-voice-waveform.h"

G_BEGIN_DECLS

/*
 * GhVoiceBubble (data/ui/gh-voice-bubble.blp; W27, nostrc-o1kl): a voice
 * message bubble in place of the text body or attachment card, shown when a
 * message's declared MIME type starts with 'audio/' (gh_voice_meta_is_audio()).
 *
 * Before download: the Download button, duration (from size estimate) and
 * "from <sender>".
 *
 * After download: play/pause, the waveform visualisation (computed from the
 * decrypted audio), elapsed/total time, and a speed selector (1×, 1.5×,
 * 2×). Download starts manually, or automatically in accepted conversations
 * when the media preference is enabled.
 *
 * Privacy: playback via GhVoicePlayer (GStreamer, no MPRIS);
 * nothing surfaces in system "now playing" metadata.
 *
 * Keyboard: every button focusable (Tab), labelled. The bubble's accessible
 * label describes the message, its state and its duration.
 *
 * Properties: "message", "index" (which file of a multi-file message),
 * "compact", "summary" (read-only accessible text).
 */

/* The same provider interface as GhAttachmentCard. */
typedef struct {
  GhAttachmentTransfer *(*lookup)(GhMessage *message, gpointer data);
  void (*download)(GhAttachmentTransfer *transfer, gpointer data);
  void (*cancel)(GhAttachmentTransfer *transfer, gpointer data);
  void (*save)(GhAttachmentTransfer *transfer, GtkWidget *bubble, gpointer data);
  gchar *(*download_note)(GhAttachmentTransfer *transfer, gpointer data);
  GhAttachmentTransfer *(*lookup_at)(GhMessage *message, guint index, gpointer data);
  void (*auto_download)(GtkWidget *bubble, GhMessage *message, guint index,
                        GhAttachmentTransfer *transfer, gpointer data);
} GhVoiceBubbleProvider;

void gh_voice_bubble_set_provider(GtkWidget *widget, const GhVoiceBubbleProvider *provider,
                                   gpointer data, GDestroyNotify destroy);

#define GH_TYPE_VOICE_BUBBLE (gh_voice_bubble_get_type())
G_DECLARE_FINAL_TYPE(GhVoiceBubble, gh_voice_bubble, GH, VOICE_BUBBLE, GtkWidget)

GtkWidget *gh_voice_bubble_new(void);
void gh_voice_bubble_set_message(GhVoiceBubble *self, GhMessage *message);
GhMessage *gh_voice_bubble_get_message(GhVoiceBubble *self);
void gh_voice_bubble_set_index(GhVoiceBubble *self, guint index);
void gh_voice_bubble_maybe_auto_download(GhVoiceBubble *self);
guint gh_voice_bubble_get_index(GhVoiceBubble *self);
/* Load decrypted audio bytes for playback; switches from download_box to
 * player_box. Called by the attachment transfer completion path. */
void gh_voice_bubble_load_audio(GhVoiceBubble *self, GBytes *audio_bytes);
void gh_voice_bubble_set_compact(GhVoiceBubble *self, gboolean compact);
const gchar *gh_voice_bubble_get_summary(GhVoiceBubble *self);

G_END_DECLS
#endif
