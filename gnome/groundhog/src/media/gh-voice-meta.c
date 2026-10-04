/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-voice-meta.h"

#include <string.h>

gboolean
gh_voice_meta_is_audio(const gchar *mime)
{
  if (!mime)
    return FALSE;
  return g_ascii_strncasecmp(mime, "audio/", 6) == 0 && mime[6] != '\0';
}

gboolean
gh_voice_meta_is_ogg_opus(const gchar *mime)
{
  if (!mime)
    return FALSE;
  return g_ascii_strcasecmp(mime, "audio/ogg") == 0 ||
         g_ascii_strcasecmp(mime, "audio/ogg; codecs=opus") == 0;
}
