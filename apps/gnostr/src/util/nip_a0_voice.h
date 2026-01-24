/**
 * @file nip_a0_voice.h
 * @brief NIP-A0 (160) Voice Messages Utilities
 *
 * NIP-A0 defines kind 160 (0xA0) events for voice messages.
 * This module provides utilities for parsing and creating voice message
 * metadata from event tags.
 *
 * Required tags:
 * - "url" - URL to the audio file
 *
 * Optional tags:
 * - "m" - MIME type (audio/webm, audio/ogg, audio/mp3, etc.)
 * - "duration" - Duration in seconds
 * - "size" - File size in bytes
 * - "blurhash" - Waveform visualization hash
 * - "x" - SHA-256 content hash for verification
 * - "e" - Reply to event (event_id, relay_url)
 * - "p" - Mention/recipient pubkey
 *
 * Event content field may contain an optional transcript or description.
 */

#ifndef GNOSTR_NIP_A0_VOICE_H
#define GNOSTR_NIP_A0_VOICE_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Kind number for voice message events (0xA0 hex = 160 decimal) */
#define NIPA0_KIND_VOICE 160

/**
 * GnostrVoiceMessage:
 * Structure containing parsed NIP-A0 voice message metadata.
 * All strings are owned by the structure and freed with gnostr_voice_message_free().
 */
typedef struct {
  gchar *audio_url;      /* Audio file URL (required) */
  gchar *mime_type;      /* MIME type (e.g., "audio/webm", "audio/ogg") */
  gint64 duration_secs;  /* Duration in seconds (0 if not specified) */
  gint64 size_bytes;     /* File size in bytes (0 if not specified) */
  gchar *blurhash;       /* Waveform visualization hash */
  gchar *content_hash;   /* SHA-256 hash for verification ("x" tag) */
  gchar *transcript;     /* Optional transcript or description (content field) */
  gchar *reply_to_id;    /* Event ID being replied to ("e" tag) */
  gchar *reply_to_relay; /* Relay URL for the reply event */
  gchar *recipient;      /* Recipient pubkey ("p" tag) */
} GnostrVoiceMessage;

/**
 * gnostr_voice_message_new:
 *
 * Creates a new empty voice message metadata structure.
 * Use gnostr_voice_message_free() to free.
 *
 * Returns: (transfer full): New voice message metadata.
 */
GnostrVoiceMessage *gnostr_voice_message_new(void);

/**
 * gnostr_voice_message_free:
 * @msg: The voice message to free, may be NULL.
 *
 * Frees a voice message structure and all its contents.
 */
void gnostr_voice_message_free(GnostrVoiceMessage *msg);

/**
 * gnostr_voice_message_copy:
 * @msg: The voice message to copy.
 *
 * Creates a deep copy of voice message metadata.
 *
 * Returns: (transfer full) (nullable): Copy of the metadata or NULL on error.
 */
GnostrVoiceMessage *gnostr_voice_message_copy(const GnostrVoiceMessage *msg);

/**
 * gnostr_voice_message_parse_tags:
 * @tags_json: JSON array string containing event tags.
 * @content: Event content (optional transcript/description), may be NULL.
 *
 * Parses NIP-A0 specific tags from an event's tags array.
 * The tags_json should be the JSON representation of the tags array.
 *
 * Returns: (transfer full) (nullable): Parsed metadata or NULL on error.
 */
GnostrVoiceMessage *gnostr_voice_message_parse_tags(const char *tags_json,
                                                     const char *content);

/**
 * gnostr_voice_message_build_tags:
 * @msg: Voice message metadata.
 *
 * Creates a JSON array string of tags for a voice message event.
 * Useful when creating new voice message events.
 *
 * Returns: (transfer full) (nullable): JSON array string or NULL on error.
 */
char *gnostr_voice_message_build_tags(const GnostrVoiceMessage *msg);

/**
 * gnostr_voice_is_voice:
 * @kind: Event kind number.
 *
 * Checks if an event kind is a voice message (kind 160).
 *
 * Returns: TRUE if kind is a voice message event.
 */
gboolean gnostr_voice_is_voice(int kind);

/**
 * gnostr_voice_validate_url:
 * @url: URL string to validate.
 *
 * Validates that a URL is suitable for audio content.
 * Checks for valid scheme (http, https) and non-empty path.
 *
 * Returns: TRUE if the URL appears valid for audio content.
 */
gboolean gnostr_voice_validate_url(const char *url);

/**
 * gnostr_voice_validate_mime_type:
 * @mime_type: MIME type string to validate.
 *
 * Validates that a MIME type is an audio format.
 *
 * Returns: TRUE if this is a valid audio MIME type.
 */
gboolean gnostr_voice_validate_mime_type(const char *mime_type);

/**
 * gnostr_voice_is_audio_mime:
 * @mime_type: MIME type string to check.
 *
 * Checks if a MIME type is an audio format (starts with "audio/").
 *
 * Returns: TRUE if this is an audio MIME type.
 */
gboolean gnostr_voice_is_audio_mime(const char *mime_type);

/**
 * gnostr_voice_detect_mime_type:
 * @file_path: Path or URL to the audio file.
 *
 * Detects MIME type from audio file extension.
 * Common audio types: audio/webm, audio/ogg, audio/mpeg, audio/mp4, etc.
 *
 * Returns: MIME type string (static, do not free), or NULL if not recognized.
 */
const char *gnostr_voice_detect_mime_type(const char *file_path);

/**
 * gnostr_voice_format_duration:
 * @duration_seconds: Duration in seconds.
 *
 * Formats duration as human-readable string (e.g., "0:45" or "2:30").
 *
 * Returns: (transfer full): Formatted duration string.
 */
char *gnostr_voice_format_duration(gint64 duration_seconds);

/**
 * gnostr_voice_format_duration_short:
 * @duration_seconds: Duration in seconds.
 *
 * Formats duration as a compact string (e.g., "45s", "2m30s", "1h5m").
 *
 * Returns: (transfer full): Compact formatted duration string.
 */
char *gnostr_voice_format_duration_short(gint64 duration_seconds);

/**
 * gnostr_voice_format_size:
 * @size_bytes: File size in bytes.
 *
 * Formats file size as human-readable string (e.g., "1.2 MB", "456 KB").
 *
 * Returns: (transfer full): Formatted size string.
 */
char *gnostr_voice_format_size(gint64 size_bytes);

/**
 * gnostr_voice_get_kind:
 *
 * Gets the NIP-A0 voice message event kind number.
 *
 * Returns: The voice message kind (160).
 */
int gnostr_voice_get_kind(void);

G_END_DECLS

#endif /* GNOSTR_NIP_A0_VOICE_H */
