/* ns-share.c - nostr-share engine
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-share.h"
#include "ns-blossom.h"
#include "ns-dav.h"
#include "ns-git.h"
#include "ns-strip.h"

#include <gio/gio.h>
#include <string.h>

/* Text bodies beyond this are not sensible as a single event. */
#define NS_MAX_TEXT_BYTES (512u * 1024u)

/* ---- NsFile / NsPost ---- */

static void
ns_file_free(gpointer p)
{
  NsFile *f = p;
  g_free(f->path);
  g_free(f->display_name);
  g_clear_pointer(&f->bytes, g_bytes_unref);
  ns_blob_meta_clear(&f->blob);
  g_free(f);
}

static void
ns_post_free(gpointer p)
{
  NsPost *post = p;
  g_clear_pointer(&post->files, g_ptr_array_unref);
  g_free(post->text);
  g_free(post->title);
  g_free(post->git_dir);
  g_free(post->unsigned_json);
  g_free(post->signed_json);
  g_free(post->result);
  g_free(post);
}

static NsPost *
ns_post_new(NsAction action, NsInputClass cls)
{
  NsPost *p = g_new0(NsPost, 1);
  p->action = action;
  p->cls = cls;
  p->files = g_ptr_array_new();   /* borrowed NsFile* */
  return p;
}

/* ---- Input loading ---- */

static gchar *
mime_for(const gchar *name, const guint8 *data, gsize len)
{
  gboolean uncertain = FALSE;
  g_autofree gchar *ct = g_content_type_guess(name, data, len, &uncertain);
  gchar *mime = ct ? g_content_type_get_mime_type(ct) : NULL;
  return mime ? mime : g_strdup("application/octet-stream");
}

static gboolean
load_file(NsShare *share, const gchar *arg, GError **error)
{
  g_autoptr(GFile) file = g_file_new_for_commandline_arg(arg);
  g_autofree gchar *path = g_file_get_path(file);
  g_autofree gchar *uri = g_file_get_uri(file);
  GError *local = NULL;
  g_autoptr(GFileInfo) info =
    g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                            G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                      G_FILE_QUERY_INFO_NONE, NULL, &local);
  if (info == NULL) {
    g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT, "%s: %s", arg, local->message);
    g_clear_error(&local);
    return FALSE;
  }

  NsFile *f = g_new0(NsFile, 1);
  f->path = g_strdup(path ? path : uri);
  f->display_name = g_strdup(g_file_info_get_display_name(info));

  if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
    if (path == NULL) {
      ns_file_free(f);
      g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT,
                  "%s: only local directories can be shared", arg);
      return FALSE;
    }
    f->cls = ns_kind_classify("inode/directory", path, TRUE, ns_git_is_repo(path));
    f->blob.mime = g_strdup("inode/directory");
    g_ptr_array_add(share->files, f);
    return TRUE;
  }

  guint64 size = (guint64)g_file_info_get_size(info);
  if (size > share->cfg->max_upload_bytes) {
    g_set_error(error, NS_ERROR, NS_ERROR_TOO_LARGE,
                "%s is %" G_GUINT64_FORMAT " MiB; the limit is %" G_GUINT64_FORMAT
                " MiB (max_upload_mib)", f->display_name, size / (1024u * 1024u),
                share->cfg->max_upload_bytes / (1024u * 1024u));
    ns_file_free(f);
    return FALSE;
  }

  gchar *contents = NULL;
  gsize len = 0;
  if (!g_file_load_contents(file, NULL, &contents, &len, NULL, &local)) {
    g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT, "%s: %s", arg, local->message);
    g_clear_error(&local);
    ns_file_free(f);
    return FALSE;
  }
  GBytes *raw = g_bytes_new_take(contents, len);
  f->blob.mime = mime_for(f->display_name, (const guint8 *)contents, len);
  f->cls = ns_kind_classify(f->blob.mime, f->display_name, FALSE, FALSE);

  if (f->cls == NS_CLASS_MEDIA) {
    GBytes *clean = NULL;
    NsStripResult r = ns_strip_metadata(f->blob.mime, (const guint8 *)contents, len,
                                        &clean, &f->n_meta_removed);
    if (r == NS_STRIP_OK) {
      f->bytes = clean;
      f->stripped = TRUE;
      g_bytes_unref(raw);
    } else if (r == NS_STRIP_MALFORMED) {
      g_set_error(error, NS_ERROR, NS_ERROR_METADATA,
                  "%s: the %s container is malformed; refusing to upload a "
                  "file whose metadata cannot be checked", f->display_name,
                  f->blob.mime);
      g_bytes_unref(raw);
      ns_file_free(f);
      return FALSE;
    } else {
      f->bytes = raw;
      f->metadata_unverified = TRUE;
    }
  } else {
    f->bytes = raw;
  }

  gsize n = 0;
  const guint8 *d = g_bytes_get_data(f->bytes, &n);
  g_autofree gchar *sha = ns_sha256_hex(d, n);
  g_strlcpy(f->blob.sha256, sha, sizeof(f->blob.sha256));
  f->blob.size = n;
  (void)ns_image_dimensions(f->blob.mime, d, n, &f->blob.width, &f->blob.height);

  if ((f->cls == NS_CLASS_TEXT || f->cls == NS_CLASS_MARKDOWN || f->cls == NS_CLASS_URL) &&
      (n > NS_MAX_TEXT_BYTES || !g_utf8_validate((const gchar *)d, (gssize)n, NULL))) {
    /* Not usable as event content: share it as a file instead. */
    f->cls = NS_CLASS_OTHER_FILE;
  }
  g_ptr_array_add(share->files, f);
  return TRUE;
}

static gchar *
file_text(const NsFile *f)
{
  gsize n = 0;
  const gchar *d = g_bytes_get_data(f->bytes, &n);
  return g_strndup(d, n);
}

static gchar *
strip_ext(const gchar *name)
{
  gchar *s = g_strdup(name);
  gchar *dot = strrchr(s, '.');
  if (dot != NULL && dot != s)
    *dot = '\0';
  return s;
}

/* ---- Planning ---- */

static void
append_para(GString *s, const gchar *para)
{
  if (para == NULL || *para == '\0')
    return;
  if (s->len > 0)
    g_string_append(s, "\n\n");
  g_string_append(s, para);
}

static gchar *
caption_with_urls(const NsShare *share)
{
  GString *s = g_string_new(share->text ? share->text : "");
  for (guint i = 0; i < share->urls->len; i++) {
    const gchar *u = g_ptr_array_index(share->urls, i);
    if (strstr(s->str, u) != NULL)
      continue;
    if (s->len > 0)
      g_string_append_c(s, '\n');
    g_string_append(s, u);
  }
  return g_string_free(s, FALSE);
}

/* Plain text follows the configured default_text_kind unless a kind was
 * chosen explicitly (--kind or the dialog's picker). */
static gint
text_forced(const NsShare *share, NsInputClass cls, gint forced)
{
  if (forced != 0 || cls != NS_CLASS_TEXT || share->cfg == NULL)
    return forced;
  return share->cfg->text_kind;
}

static gboolean
replan(NsShare *share, GError **error)
{
  g_autoptr(GPtrArray) posts = g_ptr_array_new_with_free_func(ns_post_free);
  gint forced = share->forced_kind;
  gboolean caption_used = FALSE;
  g_autofree gchar *caption = caption_with_urls(share);

  /* Media: one kind-1 note carrying every item, or one 1063 per item. */
  NsPost *media_note = NULL;
  for (guint i = 0; i < share->files->len; i++) {
    NsFile *f = g_ptr_array_index(share->files, i);
    if (f->cls != NS_CLASS_MEDIA)
      continue;
    NsAction a;
    if (!ns_kind_resolve(f->cls, forced, &a, error))
      return FALSE;
    if (a == NS_ACTION_MEDIA_NOTE) {
      if (media_note == NULL) {
        media_note = ns_post_new(a, f->cls);
        media_note->text = g_strdup(caption);
        g_ptr_array_add(posts, media_note);
      }
      g_ptr_array_add(media_note->files, f);
    } else {
      NsPost *p = ns_post_new(a, f->cls);
      p->text = g_strdup(caption);
      g_ptr_array_add(p->files, f);
      g_ptr_array_add(posts, p);
    }
    caption_used = TRUE;
  }

  for (guint i = 0; i < share->files->len; i++) {
    NsFile *f = g_ptr_array_index(share->files, i);
    NsAction a;
    NsPost *p = NULL;
    switch (f->cls) {
    case NS_CLASS_MEDIA:
      continue;
    case NS_CLASS_OTHER_FILE:
      if (!ns_kind_resolve(f->cls, forced, &a, error))
        return FALSE;
      p = ns_post_new(a, f->cls);
      p->text = g_strdup(caption);
      caption_used = TRUE;
      g_ptr_array_add(p->files, f);
      break;
    case NS_CLASS_TEXT:
    case NS_CLASS_MARKDOWN: {
      g_autofree gchar *body = file_text(f);
      if (!ns_kind_resolve(f->cls, forced, &a, error))
        return FALSE;
      p = ns_post_new(a, f->cls);
      p->text = g_steal_pointer(&body);
      g_autofree gchar *base = strip_ext(f->display_name);
      p->title = f->cls == NS_CLASS_MARKDOWN ? ns_markdown_title(p->text) : NULL;
      if (p->title == NULL)
        p->title = g_strdup(base);
      break;
    }
    case NS_CLASS_URL:
      continue;    /* folded into share->urls at load time */
    case NS_CLASS_GIT_REPO:
      if (!ns_kind_resolve(f->cls, forced, &a, error))
        return FALSE;
      p = ns_post_new(a, f->cls);
      p->git_dir = g_strdup(f->path);
      break;
    case NS_CLASS_CALENDAR:
    case NS_CLASS_CONTACT: {
      g_autofree gchar *body = file_text(f);
      if (!ns_kind_resolve(f->cls, forced, &a, error) ||
          !ns_dav_check_single(f->cls, body, error)) {
        g_prefix_error(error, "%s: ", f->display_name);
        return FALSE;
      }
      p = ns_post_new(a, f->cls);
      g_ptr_array_add(p->files, f);
      break;
    }
    case NS_CLASS_DIRECTORY:
      g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT,
                  "%s: only git repositories can be shared as directories",
                  f->display_name);
      return FALSE;
    }
    g_ptr_array_add(posts, p);
  }

  /* Free text / URLs that did not become a caption. */
  if (!caption_used && *caption != '\0') {
    NsInputClass cls = share->text_class;
    if (cls == NS_CLASS_TEXT && (share->text == NULL || *share->text == '\0') &&
        share->urls->len > 0)
      cls = NS_CLASS_URL;
    NsAction a;
    if (!ns_kind_resolve(cls, text_forced(share, cls, forced), &a, error))
      return FALSE;
    NsPost *p = ns_post_new(a, cls);
    p->text = g_strdup(caption);
    p->title = g_strdup(share->title);
    if (p->title == NULL && cls == NS_CLASS_MARKDOWN)
      p->title = ns_markdown_title(caption);
    g_ptr_array_insert(posts, 0, p);
  }

  if (posts->len == 0 && share->allow_empty) {
    NsAction a;
    if (!ns_kind_resolve(share->text_class,
                         text_forced(share, share->text_class, forced), &a, error))
      return FALSE;
    NsPost *p = ns_post_new(a, share->text_class);
    p->text = g_strdup("");
    p->title = g_strdup(share->title);
    g_ptr_array_add(posts, p);
  }
  if (posts->len == 0) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_BAD_INPUT, "nothing to share");
    return FALSE;
  }
  /* --title applies to every article. */
  if (share->title != NULL)
    for (guint i = 0; i < posts->len; i++) {
      NsPost *p = g_ptr_array_index(posts, i);
      if (p->action == NS_ACTION_ARTICLE) {
        g_free(p->title);
        p->title = g_strdup(share->title);
      }
    }

  g_clear_pointer(&share->posts, g_ptr_array_unref);
  share->posts = g_steal_pointer(&posts);
  return TRUE;
}

NsShare *
ns_share_new(NsConfig *cfg, const NsShareOptions *opts, GError **error)
{
  g_autoptr(NsShare) share = g_new0(NsShare, 1);
  share->cfg = cfg;
  ns_net_init(&share->net);
  share->files = g_ptr_array_new_with_free_func(ns_file_free);
  share->urls = g_ptr_array_new_with_free_func(g_free);
  share->forced_kind = opts->forced_kind;
  share->keep_metadata = opts->keep_metadata;
  share->created_at = g_get_real_time() / G_USEC_PER_SEC;
  share->text_class = NS_CLASS_TEXT;
  share->title = g_strdup(opts->title);
  share->allow_empty = opts->allow_empty;

  if (!ns_recipient_parse(opts->to, &share->to, error))
    return NULL;

  GString *text = g_string_new(NULL);
  for (guint i = 0; opts->texts && opts->texts[i]; i++)
    append_para(text, opts->texts[i]);

  for (guint i = 0; opts->args && opts->args[i]; i++) {
    const gchar *a = opts->args[i];
    if (g_str_has_prefix(a, "http://") || g_str_has_prefix(a, "https://")) {
      g_ptr_array_add(share->urls, g_strdup(a));
    } else if (g_str_has_prefix(a, "nostr:")) {
      append_para(text, a);    /* NIP-27 reference: shared verbatim */
    } else if (!load_file(share, a, error)) {
      g_string_free(text, TRUE);
      return NULL;
    }
  }

  /* text/uri-list files contribute URLs; a lone text/markdown file with
   * no -t text becomes the editable free text. */
  guint n_text_files = 0;
  NsFile *lone = NULL;
  for (guint i = 0; i < share->files->len; i++) {
    NsFile *f = g_ptr_array_index(share->files, i);
    if (f->cls == NS_CLASS_URL) {
      g_autofree gchar *body = file_text(f);
      g_auto(GStrv) lines = g_strsplit(body, "\n", -1);
      for (guint j = 0; lines[j]; j++) {
        gchar *l = g_strstrip(lines[j]);
        if (g_str_has_prefix(l, "http://") || g_str_has_prefix(l, "https://"))
          g_ptr_array_add(share->urls, g_strdup(l));
      }
    } else if (f->cls == NS_CLASS_TEXT || f->cls == NS_CLASS_MARKDOWN) {
      n_text_files++;
      lone = f;
    }
  }
  if (n_text_files == 1 && text->len == 0) {
    g_autofree gchar *body = file_text(lone);
    g_string_append(text, body);
    share->text_path = g_strdup(lone->path);
    share->text_path_body = g_strdup(body);
    share->text_class = lone->cls;
    if (share->title == NULL) {
      share->title = lone->cls == NS_CLASS_MARKDOWN ? ns_markdown_title(body) : NULL;
      if (share->title == NULL && lone->cls == NS_CLASS_MARKDOWN)
        share->title = strip_ext(lone->display_name);
    }
    g_ptr_array_remove(share->files, lone);
  }
  for (guint i = share->files->len; i > 0; i--) {
    NsFile *f = g_ptr_array_index(share->files, i - 1);
    if (f->cls == NS_CLASS_URL)
      g_ptr_array_remove_index(share->files, i - 1);
  }
  share->text = g_string_free(text, FALSE);

  if (!replan(share, error))
    return NULL;
  return g_steal_pointer(&share);
}

void
ns_share_free(NsShare *share)
{
  if (share == NULL)
    return;
  g_clear_pointer(&share->posts, g_ptr_array_unref);
  g_clear_pointer(&share->files, g_ptr_array_unref);
  g_clear_pointer(&share->urls, g_ptr_array_unref);
  g_free(share->text);
  g_free(share->text_path);
  g_free(share->text_path_body);
  g_free(share->title);
  ns_recipient_clear(&share->to);
  g_clear_pointer(&share->signer, nostr_publish_signer_unref);
  g_free(share->pubkey_hex);
  g_strfreev(share->servers);
  g_free(share->servers_source);
  ns_targets_clear(&share->targets);
  ns_net_clear(&share->net);
  ns_config_free(share->cfg);
  g_free(share);
}

gboolean
ns_share_set_kind(NsShare *share, gint kind, GError **error)
{
  gint old = share->forced_kind;
  share->forced_kind = kind;
  if (!replan(share, error)) {
    share->forced_kind = old;
    (void)replan(share, NULL);
    return FALSE;
  }
  return TRUE;
}

gboolean
ns_share_set_text(NsShare *share, const gchar *text, GError **error)
{
  g_free(share->text);
  share->text = g_strdup(text ? text : "");
  return replan(share, error);
}

NsInputClass
ns_share_primary_class(const NsShare *share)
{
  const NsPost *p = g_ptr_array_index(share->posts, 0);
  return p->cls;
}

/* NIP-29: text and media posted into a group are kind-9 chat messages
 * (what group clients render); every other kind keeps its own number
 * and just gains the h tag. */
static gint
post_kind(const NsShare *share, const NsPost *p)
{
  gint kind = ns_action_kind(p->action);
  if (share->to.type == NS_RECIPIENT_GROUP && kind == NS_KIND_NOTE)
    return NS_KIND_GROUP_CHAT;
  return kind;
}

gint
ns_share_primary_kind(const NsShare *share)
{
  const NsPost *p = g_ptr_array_index(share->posts, 0);
  return ns_action_kind(p->action);
}

gboolean
ns_share_needs_upload(const NsShare *share)
{
  for (guint i = 0; i < share->posts->len; i++) {
    const NsPost *p = g_ptr_array_index(share->posts, i);
    if (p->action == NS_ACTION_MEDIA_NOTE || p->action == NS_ACTION_FILE_METADATA)
      return TRUE;
  }
  return FALSE;
}

gboolean
ns_share_needs_relays(const NsShare *share)
{
  for (guint i = 0; i < share->posts->len; i++) {
    const NsPost *p = g_ptr_array_index(share->posts, i);
    if (p->action != NS_ACTION_DAV_CALENDAR && p->action != NS_ACTION_DAV_CONTACT)
      return TRUE;
  }
  return FALSE;
}

gboolean
ns_share_metadata_blocked(const NsShare *share, GString *why)
{
  if (share->keep_metadata)
    return FALSE;
  gboolean blocked = FALSE;
  for (guint i = 0; i < share->files->len; i++) {
    const NsFile *f = g_ptr_array_index(share->files, i);
    if (!f->metadata_unverified)
      continue;
    blocked = TRUE;
    if (why != NULL)
      g_string_append_printf(why, "%s%s (%s)", why->len ? ", " : "",
                             f->display_name, f->blob.mime);
  }
  return blocked;
}

/* ---- Network phases ---- */

gboolean
ns_share_connect(NsShare *share, GError **error)
{
  if (share->signer != NULL)
    return TRUE;
  share->signer = ns_signer_connect(&share->pubkey_hex, error);
  return share->signer != NULL;
}

gboolean
ns_share_resolve(NsShare *share, GError **error)
{
  ns_targets_clear(&share->targets);
  if (ns_share_needs_relays(share) &&
      !ns_resolve_targets(share->cfg, &share->net, share->pubkey_hex, &share->to,
                          &share->targets, error))
    return FALSE;
  if (ns_share_needs_upload(share) && share->servers == NULL) {
    g_clear_pointer(&share->servers_source, g_free);
    share->servers = ns_resolve_blossom_servers(share->cfg, &share->net,
                                                share->pubkey_hex,
                                                &share->servers_source, error);
    if (share->servers == NULL)
      return FALSE;
  }
  share->resolved = TRUE;
  return TRUE;
}

/* ---- Event construction ---- */

static void
predict_urls(NsShare *share)
{
  if (share->servers == NULL || share->servers[0] == NULL)
    return;
  for (guint i = 0; i < share->files->len; i++) {
    NsFile *f = g_ptr_array_index(share->files, i);
    if (f->uploaded || (f->cls != NS_CLASS_MEDIA && f->cls != NS_CLASS_OTHER_FILE))
      continue;
    g_free(f->blob.url);
    f->blob.url = ns_blossom_blob_url(share->servers[0], f->blob.sha256, f->blob.mime);
  }
}

static gchar *
content_with_mention(const NsShare *share, const gchar *text)
{
  if (share->to.type != NS_RECIPIENT_MENTION)
    return g_strdup(text ? text : "");
  g_autofree gchar *ref = g_strdup_printf("nostr:%s", share->to.npub);
  if (text != NULL && strstr(text, ref) != NULL)
    return g_strdup(text);
  if (text == NULL || *text == '\0')
    return g_steal_pointer(&ref);
  return g_strdup_printf("%s\n\n%s", text, ref);
}

static gboolean
build_post(NsShare *share, NsPost *post, GError **error)
{
  g_clear_pointer(&post->unsigned_json, g_free);
  g_clear_pointer(&post->signed_json, g_free);
  g_autoptr(JsonArray) tags = json_array_new();
  g_autofree gchar *content = NULL;
  gint kind = post_kind(share, post);

  if ((post->action == NS_ACTION_NOTE || post->action == NS_ACTION_ARTICLE) &&
      (post->text == NULL || strspn(post->text, " \t\r\n") == strlen(post->text))) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_BAD_INPUT,
                        "nothing to share: the text is empty");
    return FALSE;
  }
  switch (post->action) {
  case NS_ACTION_NOTE:
    content = content_with_mention(share, post->text);
    ns_tags_add_urls(tags, content);
    break;

  case NS_ACTION_ARTICLE: {
    content = content_with_mention(share, post->text);
    g_autofree gchar *slug = ns_slugify(post->title);
    ns_tags_add_article(tags, slug, post->title, share->created_at);
    break;
  }

  case NS_ACTION_MEDIA_NOTE: {
    GString *c = g_string_new(post->text ? post->text : "");
    for (guint i = 0; i < post->files->len; i++) {
      NsFile *f = g_ptr_array_index(post->files, i);
      if (f->blob.url == NULL) {
        g_string_free(c, TRUE);
        g_set_error_literal(error, NS_ERROR, NS_ERROR_NO_SERVERS,
                            "no Blossom server resolved for media upload");
        return FALSE;
      }
      if (c->len > 0)
        g_string_append_c(c, '\n');
      g_string_append(c, f->blob.url);
      json_array_add_array_element(tags, ns_imeta_tag_new(&f->blob));
    }
    g_autofree gchar *body = g_string_free(c, FALSE);
    content = content_with_mention(share, body);
    /* r tags only for links the user wrote, not for our blob URLs. */
    g_autofree gchar *user_text = content_with_mention(share, post->text);
    ns_tags_add_urls(tags, user_text);
    break;
  }

  case NS_ACTION_FILE_METADATA: {
    NsFile *f = g_ptr_array_index(post->files, 0);
    if (f->blob.url == NULL) {
      g_set_error_literal(error, NS_ERROR, NS_ERROR_NO_SERVERS,
                          "no Blossom server resolved for file upload");
      return FALSE;
    }
    content = content_with_mention(share, post->text);
    ns_tags_add_file_metadata(tags, &f->blob);
    break;
  }

  case NS_ACTION_GIT_REPO: {
    json_array_unref(g_steal_pointer(&tags));
    const gchar *const *relays = (const gchar *const *)share->targets.direct;
    if (!ns_git_announcement(post->git_dir, relays, &tags, &content, NULL, error))
      return FALSE;
    break;
  }

  case NS_ACTION_DAV_CALENDAR:
  case NS_ACTION_DAV_CONTACT:
    return TRUE;   /* nostr-dav builds the event */
  }

  ns_tags_add_recipient(tags, &share->to);
  post->unsigned_json = ns_event_unsigned_json(kind, share->created_at,
                                               share->pubkey_hex, tags, content,
                                               FALSE);
  return TRUE;
}

gboolean
ns_share_build(NsShare *share, GError **error)
{
  predict_urls(share);
  for (guint i = 0; i < share->posts->len; i++)
    if (!build_post(share, g_ptr_array_index(share->posts, i), error))
      return FALSE;
  return TRUE;
}

gboolean
ns_share_sign(NsShare *share, GError **error)
{
  if (!ns_share_connect(share, error))
    return FALSE;
  for (guint i = 0; i < share->posts->len; i++) {
    NsPost *p = g_ptr_array_index(share->posts, i);
    if (p->unsigned_json == NULL || p->signed_json != NULL)
      continue;
    GError *local = NULL;
    p->signed_json = nostr_publish_signer_sign_event_json(share->signer,
                                                          p->unsigned_json, NULL,
                                                          &local);
    if (p->signed_json == NULL) {
      g_set_error(error, NS_ERROR, NS_ERROR_NO_SIGNER, "signing failed: %s",
                  local ? local->message : "unknown");
      g_clear_error(&local);
      return FALSE;
    }
  }
  return TRUE;
}

gchar *
ns_share_preview_json(const NsShare *share, gboolean signed_if_available)
{
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < share->posts->len; i++) {
    const NsPost *p = g_ptr_array_index(share->posts, i);
    const gchar *j = signed_if_available && p->signed_json ? p->signed_json
                                                           : p->unsigned_json;
    if (s->len > 0)
      g_string_append_c(s, '\n');
    if (j == NULL) {
      NsFile *f = p->files->len ? g_ptr_array_index(p->files, 0) : NULL;
      g_string_append_printf(s, "// %s → handed to nostr-dav (%s)\n",
                             f ? f->display_name : "?",
                             p->action == NS_ACTION_DAV_CALENDAR
                               ? "calendars/nostr/, NIP-52" : "contacts/nostr/, kind 30085");
      continue;
    }
    g_autofree gchar *pretty = ns_json_pretty(j);
    g_string_append(s, pretty);
    g_string_append_c(s, '\n');
  }
  return g_string_free(s, FALSE);
}

static void
say(NsProgressFunc progress, gpointer user_data, const gchar *fmt, ...) G_GNUC_PRINTF(3, 4);

static void
say(NsProgressFunc progress, gpointer user_data, const gchar *fmt, ...)
{
  if (progress == NULL)
    return;
  va_list ap;
  va_start(ap, fmt);
  g_autofree gchar *msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  progress(msg, user_data);
}

gboolean
ns_share_upload(NsShare *share, gboolean *urls_changed, NsProgressFunc progress,
                gpointer user_data, GError **error)
{
  if (urls_changed) *urls_changed = FALSE;
  if (!ns_share_needs_upload(share))
    return TRUE;
  g_autoptr(GString) why = g_string_new(NULL);
  if (ns_share_metadata_blocked(share, why)) {
    g_set_error(error, NS_ERROR, NS_ERROR_METADATA,
                "cannot strip metadata (location, device, timestamps) from %s; "
                "re-run with --keep-metadata to upload it anyway", why->str);
    return FALSE;
  }
  if (!ns_share_connect(share, error))
    return FALSE;
  if (!share->resolved && !ns_share_resolve(share, error))
    return FALSE;

  for (guint i = 0; i < share->posts->len; i++) {
    NsPost *p = g_ptr_array_index(share->posts, i);
    if (p->action != NS_ACTION_MEDIA_NOTE && p->action != NS_ACTION_FILE_METADATA)
      continue;
    for (guint j = 0; j < p->files->len; j++) {
      NsFile *f = g_ptr_array_index(p->files, j);
      if (f->uploaded)
        continue;
      say(progress, user_data, "Uploading %s (%" G_GUINT64_FORMAT " KiB)…",
          f->display_name, f->blob.size / 1024u);
      g_autofree gchar *url = NULL, *server = NULL;
      if (!ns_blossom_upload((const gchar *const *)share->servers, share->signer,
                             share->pubkey_hex, f->blob.mime, f->bytes,
                             f->blob.sha256, &url, &server, error))
        return FALSE;
      if (urls_changed != NULL && g_strcmp0(url, f->blob.url) != 0)
        *urls_changed = TRUE;
      g_free(f->blob.url);
      f->blob.url = g_steal_pointer(&url);
      f->uploaded = TRUE;
      say(progress, user_data, "Uploaded %s → %s", f->display_name, f->blob.url);
    }
  }
  return TRUE;
}

gboolean
ns_share_mark_published(const gchar *path, const gchar *event_id_hex)
{
  if (path == NULL || event_id_hex == NULL)
    return FALSE;
  g_autoptr(GFile) f = g_file_new_for_path(path);
  g_autoptr(GError) err = NULL;
  if (!g_file_set_attribute_string(f, NS_XATTR_EVENT, event_id_hex,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &err)) {
    g_debug("nostr-share: no %s on %s: %s", NS_XATTR_EVENT, path, err->message);
    return FALSE;
  }
  return TRUE;
}

/* nostrc-tepd: tag what was just published at its source — media files,
 * a git repository, or the lone text/markdown file the post was made from
 * (only while the post still says what the file says: an edited note is
 * not that file). */
static void
mark_sources(const NsShare *share, const NsPost *p)
{
  g_autofree gchar *id = ns_event_id_from_signed_json(p->signed_json);
  if (id == NULL)
    return;
  for (guint i = 0; p->files && i < p->files->len; i++)
    (void)ns_share_mark_published(((NsFile *)g_ptr_array_index(p->files, i))->path, id);
  if (p->git_dir)
    (void)ns_share_mark_published(p->git_dir, id);
  if ((p->files == NULL || p->files->len == 0) && p->git_dir == NULL && share->text_path &&
      g_strcmp0(p->text, share->text_path_body) == 0)
    (void)ns_share_mark_published(share->text_path, id);
}

gboolean
ns_share_publish(NsShare *share, NsProgressFunc progress, gpointer user_data,
                 GError **error)
{
  gboolean any_relay = ns_share_needs_relays(share);
  if (any_relay) {
    if (!ns_share_connect(share, error))
      return FALSE;
    if (!share->resolved && !ns_share_resolve(share, error))
      return FALSE;
  }

  for (guint i = 0; i < share->posts->len; i++) {
    NsPost *p = g_ptr_array_index(share->posts, i);
    g_clear_pointer(&p->result, g_free);

    if (p->action == NS_ACTION_DAV_CALENDAR || p->action == NS_ACTION_DAV_CONTACT) {
      NsFile *f = g_ptr_array_index(p->files, 0);
      g_autofree gchar *url = NULL;
      say(progress, user_data, "Handing %s to nostr-dav…", f->display_name);
      if (!ns_dav_stage(share->cfg->dav_url, p->cls, f->bytes, FALSE, &url, error))
        return FALSE;
      p->result = g_strdup_printf("%s → %s (nostr-dav publishes it)",
                                  f->display_name, url);
      continue;
    }

    if (p->signed_json == NULL) {
      GError *local = NULL;
      say(progress, user_data, "Waiting for the signer…");
      p->signed_json = nostr_publish_signer_sign_event_json(share->signer,
                                                            p->unsigned_json,
                                                            NULL, &local);
      if (p->signed_json == NULL) {
        g_set_error(error, NS_ERROR, NS_ERROR_NO_SIGNER, "signing failed: %s",
                    local ? local->message : "unknown");
        g_clear_error(&local);
        return FALSE;
      }
    }

    say(progress, user_data, "Publishing kind %d…", post_kind(share, p));
    NsPublishReport rep;
    gboolean ok = ns_net_publish(&share->net, share->cfg, p->signed_json,
                                 &share->targets, &rep, error);
    GString *r = g_string_new(NULL);
    g_string_append_printf(r, "kind %d: %u/%u relays accepted",
                           post_kind(share, p), rep.n_accepted, rep.n_targets);
    for (guint k = 0; rep.lines && k < rep.lines->len; k++) {
      say(progress, user_data, "  %s", (const gchar *)g_ptr_array_index(rep.lines, k));
      g_string_append_printf(r, "\n  %s", (const gchar *)g_ptr_array_index(rep.lines, k));
    }
    p->result = g_string_free(r, FALSE);
    ns_publish_report_clear(&rep);
    if (!ok)
      return FALSE;
    mark_sources(share, p);
  }
  return TRUE;
}

gchar *
ns_share_describe_targets(const NsShare *share)
{
  if (!ns_share_needs_relays(share))
    return g_strdup("nostr-dav (it publishes calendar/contact events itself)");
  if (share->targets.targets == NULL)
    return g_strdup("not resolved yet");
  GString *s = g_string_new(NULL);
  if (share->targets.session_included)
    g_string_append(s, "session relay (relay.sock)");
  guint n = share->targets.direct ? g_strv_length(share->targets.direct) : 0;
  if (n > 0) {
    if (s->len) g_string_append(s, " + ");
    g_string_append_printf(s, "%u relay%s from %s: ", n, n == 1 ? "" : "s",
                           share->targets.write_source);
    for (guint i = 0; i < n; i++)
      g_string_append_printf(s, "%s%s", i ? ", " : "", share->targets.direct[i]);
  } else if (share->targets.session_included) {
    g_string_append(s, " only — the session relay does not federate upstream "
                       "yet, so this stays on this machine");
  }
  return g_string_free(s, FALSE);
}

gchar *
ns_share_describe_servers(const NsShare *share)
{
  if (!ns_share_needs_upload(share))
    return g_strdup("none (nothing to upload)");
  if (share->servers == NULL)
    return g_strdup("not resolved");
  g_autofree gchar *list = g_strjoinv(", ", share->servers);
  return g_strdup_printf("%s (from %s)", list, share->servers_source);
}
