/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GH_VOICE_META_H
#define GH_VOICE_META_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Voice message metadata utilities (W27, nostrc-o1kl).
 *
 * A voice note is a short audio recording whose MIME type starts with
 * "audio/" (the
 * same rule MDK v0.11.0 uses in its presentation layer). On the wire it is
 * an ordinary encrypted attachment (NIP-17 kind-15 or MIP-04 v2 imeta) with
 * no special tags — interoperability comes from the MIME type and the Opus
 * codec inside an OGG container, matching what White Noise sends. Duration
 * and waveform are local display data, never transmitted in the imeta tag
 * (MDK v0.11.0 does the same).
 *
 * Network-free, GTK-free.
 */

/* The MIME type Groundhog writes for a recorded voice note, matching MDK's
 * wire format. */
#define GH_VOICE_MIME "audio/ogg"

/* The filename Groundhog writes, matching MDK's wire format. */
#define GH_VOICE_FILENAME "voice.ogg"

/* Maximum recording duration in seconds (5 minutes). */
#define GH_VOICE_MAX_DURATION_S 300

/* Maximum number of waveform samples stored (enough for a 5-minute clip at
 * roughly 4 samples per second). */
#define GH_VOICE_MAX_WAVEFORM_SAMPLES 1200

/* TRUE when the declared MIME type (lowercased, parameters already stripped)
 * identifies a voice-eligible audio message. Every audio attachment is
 * shown as a voice bubble. */
gboolean gh_voice_meta_is_audio(const gchar *mime);

/* TRUE when the declared MIME type is specifically Opus-in-OGG, the codec
 * Groundhog records. Other audio types (audio/mp4, audio/mpeg) are played
 * but displayed with a simpler card. */
gboolean gh_voice_meta_is_ogg_opus(const gchar *mime);

G_END_DECLS
#endif
