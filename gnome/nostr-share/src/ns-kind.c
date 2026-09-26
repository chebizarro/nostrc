/* ns-kind.c - Input classification and kind resolution
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-kind.h"

#include <string.h>

G_DEFINE_QUARK(nostr-share-error-quark, ns_error)

static gboolean
has_ext(const gchar *filename, const gchar *const *exts)
{
  if (filename == NULL)
    return FALSE;
  const gchar *dot = strrchr(filename, '.');
  if (dot == NULL)
    return FALSE;
  for (guint i = 0; exts[i] != NULL; i++)
    if (g_ascii_strcasecmp(dot + 1, exts[i]) == 0)
      return TRUE;
  return FALSE;
}

NsInputClass
ns_kind_classify(const gchar *mime,
                 const gchar *filename,
                 gboolean     is_dir,
                 gboolean     is_git_repo)
{
  static const gchar *const md_exts[]  = { "md", "markdown", "mkd", NULL };
  static const gchar *const ics_exts[] = { "ics", "ical", "ifb", NULL };
  static const gchar *const vcf_exts[] = { "vcf", "vcard", NULL };

  if (is_dir || g_strcmp0(mime, "inode/directory") == 0)
    return is_git_repo ? NS_CLASS_GIT_REPO : NS_CLASS_DIRECTORY;

  /* Extension first for the text families: shared-mime-info frequently
   * reports *.md as text/plain and *.vcf as text/x-vcard. */
  if (has_ext(filename, md_exts))
    return NS_CLASS_MARKDOWN;
  if (has_ext(filename, ics_exts))
    return NS_CLASS_CALENDAR;
  if (has_ext(filename, vcf_exts))
    return NS_CLASS_CONTACT;

  if (mime == NULL)
    return NS_CLASS_OTHER_FILE;

  if (g_str_equal(mime, "text/markdown") || g_str_equal(mime, "text/x-markdown"))
    return NS_CLASS_MARKDOWN;
  if (g_str_equal(mime, "text/uri-list") || g_str_equal(mime, "x-scheme-handler/http") ||
      g_str_equal(mime, "x-scheme-handler/https"))
    return NS_CLASS_URL;
  if (g_str_equal(mime, "text/calendar"))
    return NS_CLASS_CALENDAR;
  if (g_str_equal(mime, "text/vcard") || g_str_equal(mime, "text/x-vcard") ||
      g_str_equal(mime, "text/directory"))
    return NS_CLASS_CONTACT;
  if (g_str_has_prefix(mime, "image/") || g_str_has_prefix(mime, "video/") ||
      g_str_has_prefix(mime, "audio/"))
    return NS_CLASS_MEDIA;
  if (g_str_equal(mime, "text/plain"))
    return NS_CLASS_TEXT;
  return NS_CLASS_OTHER_FILE;
}

static gboolean
bad_kind(GError **error, NsInputClass cls, gint kind, const gchar *allowed)
{
  g_set_error(error, NS_ERROR, NS_ERROR_BAD_KIND,
              "--kind %d does not apply to %s input (allowed: %s)",
              kind, ns_class_name(cls), allowed);
  return FALSE;
}

gboolean
ns_kind_resolve(NsInputClass  cls,
                gint          forced_kind,
                NsAction     *out_action,
                GError      **error)
{
  g_return_val_if_fail(out_action != NULL, FALSE);

  switch (cls) {
  case NS_CLASS_TEXT:
    if (forced_kind == 0 || forced_kind == NS_KIND_NOTE)
      *out_action = NS_ACTION_NOTE;
    else if (forced_kind == NS_KIND_ARTICLE)
      *out_action = NS_ACTION_ARTICLE;
    else
      return bad_kind(error, cls, forced_kind, "1, 30023");
    return TRUE;

  case NS_CLASS_MARKDOWN:
    if (forced_kind == 0 || forced_kind == NS_KIND_ARTICLE)
      *out_action = NS_ACTION_ARTICLE;
    else if (forced_kind == NS_KIND_NOTE)
      *out_action = NS_ACTION_NOTE;
    else
      return bad_kind(error, cls, forced_kind, "30023, 1");
    return TRUE;

  case NS_CLASS_URL:
    if (forced_kind != 0 && forced_kind != NS_KIND_NOTE)
      return bad_kind(error, cls, forced_kind, "1");
    *out_action = NS_ACTION_NOTE;
    return TRUE;

  case NS_CLASS_MEDIA:
    if (forced_kind == 0 || forced_kind == NS_KIND_NOTE)
      *out_action = NS_ACTION_MEDIA_NOTE;
    else if (forced_kind == NS_KIND_FILE_METADATA)
      *out_action = NS_ACTION_FILE_METADATA;
    else
      return bad_kind(error, cls, forced_kind, "1, 1063");
    return TRUE;

  case NS_CLASS_OTHER_FILE:
    if (forced_kind == 0 || forced_kind == NS_KIND_FILE_METADATA)
      *out_action = NS_ACTION_FILE_METADATA;
    else if (forced_kind == NS_KIND_NOTE)
      *out_action = NS_ACTION_MEDIA_NOTE;
    else
      return bad_kind(error, cls, forced_kind, "1063, 1");
    return TRUE;

  case NS_CLASS_GIT_REPO:
    if (forced_kind != 0 && forced_kind != NS_KIND_GIT_REPO)
      return bad_kind(error, cls, forced_kind, "30617");
    *out_action = NS_ACTION_GIT_REPO;
    return TRUE;

  case NS_CLASS_CALENDAR:
  case NS_CLASS_CONTACT:
    if (forced_kind != 0) {
      g_set_error(error, NS_ERROR, NS_ERROR_BAD_KIND,
                  "--kind does not apply to %s files: they are handed to "
                  "nostr-dav, which picks the event kind",
                  ns_class_name(cls));
      return FALSE;
    }
    *out_action = cls == NS_CLASS_CALENDAR ? NS_ACTION_DAV_CALENDAR
                                           : NS_ACTION_DAV_CONTACT;
    return TRUE;

  case NS_CLASS_DIRECTORY:
    g_set_error_literal(error, NS_ERROR, NS_ERROR_BAD_INPUT,
                        "only git repositories can be shared as directories");
    return FALSE;
  }

  g_set_error_literal(error, NS_ERROR, NS_ERROR_BAD_INPUT, "unknown input");
  return FALSE;
}

gint
ns_action_kind(NsAction action)
{
  switch (action) {
  case NS_ACTION_NOTE:
  case NS_ACTION_MEDIA_NOTE:    return NS_KIND_NOTE;
  case NS_ACTION_ARTICLE:       return NS_KIND_ARTICLE;
  case NS_ACTION_FILE_METADATA: return NS_KIND_FILE_METADATA;
  case NS_ACTION_GIT_REPO:      return NS_KIND_GIT_REPO;
  case NS_ACTION_DAV_CALENDAR:
  case NS_ACTION_DAV_CONTACT:   return 0;
  }
  return 0;
}

guint
ns_kind_choices(NsInputClass cls, gint out_kinds[3])
{
  switch (cls) {
  case NS_CLASS_TEXT:
    out_kinds[0] = NS_KIND_NOTE; out_kinds[1] = NS_KIND_ARTICLE;
    return 2;
  case NS_CLASS_MARKDOWN:
    out_kinds[0] = NS_KIND_ARTICLE; out_kinds[1] = NS_KIND_NOTE;
    return 2;
  case NS_CLASS_URL:
    out_kinds[0] = NS_KIND_NOTE;
    return 1;
  case NS_CLASS_MEDIA:
    out_kinds[0] = NS_KIND_NOTE; out_kinds[1] = NS_KIND_FILE_METADATA;
    return 2;
  case NS_CLASS_OTHER_FILE:
    out_kinds[0] = NS_KIND_FILE_METADATA; out_kinds[1] = NS_KIND_NOTE;
    return 2;
  case NS_CLASS_GIT_REPO:
    out_kinds[0] = NS_KIND_GIT_REPO;
    return 1;
  case NS_CLASS_CALENDAR:
  case NS_CLASS_CONTACT:
  case NS_CLASS_DIRECTORY:
    return 0;
  }
  return 0;
}

const gchar *
ns_class_name(NsInputClass cls)
{
  switch (cls) {
  case NS_CLASS_TEXT:       return "text";
  case NS_CLASS_MARKDOWN:   return "markdown";
  case NS_CLASS_URL:        return "URL";
  case NS_CLASS_MEDIA:      return "media";
  case NS_CLASS_CALENDAR:   return "calendar";
  case NS_CLASS_CONTACT:    return "contact";
  case NS_CLASS_GIT_REPO:   return "git repository";
  case NS_CLASS_DIRECTORY:  return "directory";
  case NS_CLASS_OTHER_FILE: return "file";
  }
  return "unknown";
}

const gchar *
ns_kind_label(gint kind)
{
  switch (kind) {
  case NS_KIND_NOTE:          return "Note (kind 1)";
  case NS_KIND_GROUP_CHAT:    return "Group message (kind 9)";
  case NS_KIND_FILE_METADATA: return "File metadata (kind 1063)";
  case NS_KIND_ARTICLE:       return "Article (kind 30023)";
  case NS_KIND_GIT_REPO:      return "Git repository (kind 30617)";
  default:                    return "Other";
  }
}
