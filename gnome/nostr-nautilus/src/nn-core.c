/* nn-core.c - nostr-nautilus menu gating and argv construction
 *
 * SPDX-License-Identifier: MIT
 */
#include "nn-core.h"

/* nostr-share's own classifier (gnome/nostr-share/src/ns-kind.c, compiled
 * in): "Upload to Blossom" is offered exactly when `nostr-share --kind 1063`
 * accepts the input, so the menu cannot drift from the CLI. */
#include "ns-kind.h"

#include <string.h>

G_DEFINE_QUARK(nostr-nautilus-error-quark, nn_error)
#define NN_ERROR (nn_error_quark())

/* --- MIME matching -------------------------------------------------------- */

gboolean
nn_mime_in_list(const gchar *mime, const gchar *const *patterns)
{
  if (mime == NULL || *mime == '\0' || patterns == NULL)
    return FALSE;

  g_autofree gchar *ct = g_content_type_from_mime_type(mime);
  for (guint i = 0; patterns[i] != NULL; i++) {
    const gchar *pat = patterns[i];
    gsize plen = strlen(pat);

    if (g_ascii_strcasecmp(mime, pat) == 0)
      return TRUE;
    if (plen >= 2 && pat[plen - 1] == '*' && pat[plen - 2] == '/') {
      if (g_ascii_strncasecmp(mime, pat, plen - 1) == 0)
        return TRUE;
      continue;
    }
    /* Aliases (text/x-vcard → text/vcard) and subclasses (text/x-csrc ⊂
     * text/plain): what GNOME's Open With itself honours. */
    g_autofree gchar *pct = g_content_type_from_mime_type(pat);
    if (ct != NULL && pct != NULL && g_content_type_is_a(ct, pct))
      return TRUE;
  }
  return FALSE;
}

/* --- Selection predicates ------------------------------------------------- */

static gboolean
has_sealed_suffix(const NnFile *f)
{
  const gchar *n = f->name ? f->name : f->path;
  return n != NULL && g_str_has_suffix(n, NN_SEALED_SUFFIX) &&
         strlen(n) > strlen(NN_SEALED_SUFFIX);
}

gboolean
nn_file_is_sealed(const NnFile *file)
{
  return g_strcmp0(file->mime, NN_SEALED_MIME) == 0 || has_sealed_suffix(file);
}

static gboolean
file_shareable(const NnFile *f, const NnTools *t)
{
  if (f->type == G_FILE_TYPE_DIRECTORY)
    /* nostr-share only accepts directories that are git repositories
     * (NIP-34) and needs a local path to read them. */
    return f->is_git_repo && f->path != NULL &&
           nn_mime_in_list("inode/directory", t->share_mimes);
  if (f->type != G_FILE_TYPE_REGULAR)
    return FALSE;
  return nn_mime_in_list(f->mime, t->share_mimes);
}

static gboolean
file_uploadable(const NnFile *f)
{
  if (f->type != G_FILE_TYPE_REGULAR)
    return FALSE;
  NsInputClass cls = ns_kind_classify(f->mime, f->name, FALSE, FALSE);
  NsAction action;
  return ns_kind_resolve(cls, NS_KIND_FILE_METADATA, &action, NULL);
}

static gboolean
file_encryptable(const NnFile *f)
{
  /* v1: regular local files only (nostr-seal opens paths; directories
   * would need a tar step first). Sealing a sealed file is pointless. */
  return f->type == G_FILE_TYPE_REGULAR && f->path != NULL && !nn_file_is_sealed(f);
}

static gboolean
file_decryptable(const NnFile *f, const NnTools *t)
{
  if (f->type != G_FILE_TYPE_REGULAR || f->path == NULL || !nn_file_is_sealed(f))
    return FALSE;
  /* `nostr-seal decrypt` derives the output name by stripping the suffix;
   * nostr-seal-gtk copes without one. */
  return t->seal_gtk_exe != NULL || has_sealed_suffix(f);
}

gboolean
nn_action_applies(NnAction action, const NnFile *files, guint n_files,
                  const NnTools *tools)
{
  g_return_val_if_fail(tools != NULL, FALSE);
  if (files == NULL || n_files == 0)
    return FALSE;

  switch (action) {
  case NN_ACTION_SHARE:
  case NN_ACTION_UPLOAD:
    if (tools->share_exe == NULL || tools->share_mimes == NULL)
      return FALSE;
    break;
  case NN_ACTION_ENCRYPT:
    if (tools->seal_exe == NULL || tools->seal_helper == NULL)
      return FALSE;
    break;
  case NN_ACTION_DECRYPT:
    if (tools->seal_exe == NULL && tools->seal_gtk_exe == NULL)
      return FALSE;
    break;
  default:
    return FALSE;
  }

  for (guint i = 0; i < n_files; i++) {
    const NnFile *f = &files[i];
    gboolean ok = FALSE;
    switch (action) {
    case NN_ACTION_SHARE:   ok = file_shareable(f, tools); break;
    case NN_ACTION_UPLOAD:  ok = file_uploadable(f); break;
    case NN_ACTION_ENCRYPT: ok = file_encryptable(f); break;
    case NN_ACTION_DECRYPT: ok = file_decryptable(f, tools); break;
    default: break;
    }
    if (!ok)
      return FALSE;
  }
  return TRUE;
}

/* --- argv ----------------------------------------------------------------- */

static const gchar *
file_arg(const NnFile *f)
{
  /* nostr-share takes local paths and GVfs URIs alike (Exec=nostr-share %U).
   * Local paths are absolute, URIs start with a scheme: never an option. */
  return f->path != NULL ? f->path : f->uri;
}

static GStrv
steal_strv(GPtrArray *a)
{
  g_ptr_array_add(a, NULL);
  return (GStrv)g_ptr_array_free(a, FALSE);
}

GPtrArray *
nn_action_argvs(NnAction action, const NnFile *files, guint n_files,
                const NnTools *tools)
{
  if (!nn_action_applies(action, files, n_files, tools))
    return NULL;

  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
  GPtrArray *a = NULL;

  switch (action) {
  case NN_ACTION_SHARE:
  case NN_ACTION_UPLOAD:
    a = g_ptr_array_new();
    g_ptr_array_add(a, g_strdup(tools->share_exe));
    if (action == NN_ACTION_UPLOAD) {
      g_ptr_array_add(a, g_strdup("--kind"));
      g_ptr_array_add(a, g_strdup_printf("%d", NS_KIND_FILE_METADATA));
    }
    for (guint i = 0; i < n_files; i++)
      g_ptr_array_add(a, g_strdup(file_arg(&files[i])));
    g_ptr_array_add(out, steal_strv(a));
    break;

  case NN_ACTION_ENCRYPT:
    a = g_ptr_array_new();
    g_ptr_array_add(a, g_strdup(tools->seal_helper));
    for (guint i = 0; i < n_files; i++)
      g_ptr_array_add(a, g_strdup(files[i].path));
    g_ptr_array_add(out, steal_strv(a));
    break;

  case NN_ACTION_DECRYPT:
    if (tools->seal_gtk_exe != NULL) {
      /* nostr-seal's own decrypt UI: recipient picker, signer prompt hint,
       * non-clobbering output name, errors shown in the window. */
      a = g_ptr_array_new();
      g_ptr_array_add(a, g_strdup(tools->seal_gtk_exe));
      for (guint i = 0; i < n_files; i++)
        g_ptr_array_add(a, g_strdup(files[i].path));
      g_ptr_array_add(out, steal_strv(a));
    } else {
      for (guint i = 0; i < n_files; i++) {
        a = g_ptr_array_new();
        g_ptr_array_add(a, g_strdup(tools->seal_exe));
        g_ptr_array_add(a, g_strdup("decrypt"));
        g_ptr_array_add(a, g_strdup(files[i].path));
        g_ptr_array_add(out, steal_strv(a));
      }
    }
    break;

  default:
    break;
  }
  return out;
}

gchar *
nn_exec_line(const gchar *const *argv)
{
  g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);
  GString *s = g_string_new(NULL);
  for (guint i = 0; argv[i] != NULL; i++) {
    g_autofree gchar *q = g_shell_quote(argv[i]);
    if (i > 0)
      g_string_append_c(s, ' ');
    /* GDesktopAppInfo expands field codes on the raw line before it
     * shell-parses it, so a literal '%' must be written "%%". */
    for (const gchar *p = q; *p; p++) {
      if (*p == '%')
        g_string_append_c(s, '%');
      g_string_append_c(s, *p);
    }
  }
  return g_string_free(s, FALSE);
}

gboolean
nn_dir_is_git_repo(const gchar *path)
{
  /* Same test as nostr-share's ns_git_is_repo() (ns-git.c): a .git entry,
   * or a bare repository layout. Four stat()s at most, local paths only. */
  if (path == NULL)
    return FALSE;
  g_autofree gchar *dotgit = g_build_filename(path, ".git", NULL);
  if (g_file_test(dotgit, G_FILE_TEST_EXISTS))
    return TRUE;
  g_autofree gchar *head = g_build_filename(path, "HEAD", NULL);
  if (!g_file_test(head, G_FILE_TEST_IS_REGULAR))
    return FALSE;
  g_autofree gchar *objects = g_build_filename(path, "objects", NULL);
  g_autofree gchar *refs = g_build_filename(path, "refs", NULL);
  return g_file_test(objects, G_FILE_TEST_IS_DIR) && g_file_test(refs, G_FILE_TEST_IS_DIR);
}

/* --- Recipients ----------------------------------------------------------- */

static const gchar BECH32_CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static guint32
bech32_polymod_step(guint32 chk, guint8 v)
{
  static const guint32 gen[5] = { 0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3 };
  guint8 top = chk >> 25;
  chk = ((chk & 0x1ffffff) << 5) ^ v;
  for (guint i = 0; i < 5; i++)
    if ((top >> i) & 1)
      chk ^= gen[i];
  return chk;
}

/* Early typo check only: nostr-seal re-decodes every key (nips/nip19) and
 * remains the authority. @s is lower-case, starts with "npub1", 63 chars. */
static gboolean
npub_checksum_ok(const gchar *s)
{
  static const gchar hrp[] = "npub";
  guint32 chk = 1;
  for (guint i = 0; hrp[i]; i++)
    chk = bech32_polymod_step(chk, (guint8)(hrp[i] >> 5));
  chk = bech32_polymod_step(chk, 0);
  for (guint i = 0; hrp[i]; i++)
    chk = bech32_polymod_step(chk, (guint8)(hrp[i] & 31));
  for (const gchar *p = s + 5; *p; p++) {
    const gchar *c = strchr(BECH32_CHARSET, *p);
    if (c == NULL)
      return FALSE;
    chk = bech32_polymod_step(chk, (guint8)(c - BECH32_CHARSET));
  }
  return chk == 1;
}

static gchar *
normalize_recipient(const gchar *tok, GError **error)
{
  const gchar *t = tok;
  if (g_ascii_strncasecmp(t, "nostr:", 6) == 0)
    t += 6;
  g_autofree gchar *low = g_ascii_strdown(t, -1);
  gsize len = strlen(low);

  if (len == 64) {
    gboolean hex = TRUE;
    for (gsize i = 0; i < len && hex; i++)
      hex = g_ascii_isxdigit(low[i]);
    if (hex)
      return g_steal_pointer(&low);
  }
  if (len == 63 && g_str_has_prefix(low, "npub1")) {
    /* bech32 is single-case: reject mixed case before folding it away. */
    gboolean has_lower = FALSE, has_upper = FALSE;
    for (const gchar *p = t; *p; p++) {
      has_lower |= g_ascii_islower(*p);
      has_upper |= g_ascii_isupper(*p);
    }
    if (!(has_lower && has_upper) && npub_checksum_ok(low))
      return g_steal_pointer(&low);
    g_set_error(error, NN_ERROR, 1, "“%s” is not a valid npub (checksum mismatch)", tok);
    return NULL;
  }
  g_set_error(error, NN_ERROR, 1, "“%s” is not an npub1… or 64-character hex public key", tok);
  return NULL;
}

gboolean
nn_parse_recipients(const gchar *text, GStrv *out, GError **error)
{
  g_return_val_if_fail(out != NULL, FALSE);
  *out = NULL;
  g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(g_free);
  g_auto(GStrv) toks = g_strsplit_set(text ? text : "", " \t\r\n,;", -1);

  for (guint i = 0; toks[i] != NULL; i++) {
    if (toks[i][0] == '\0')
      continue;
    gchar *k = normalize_recipient(toks[i], error);
    if (k == NULL)
      return FALSE;
    gboolean dup = FALSE;
    for (guint j = 0; j < keys->len && !dup; j++)
      dup = g_str_equal(g_ptr_array_index(keys, j), k);
    if (dup)
      g_free(k);
    else
      g_ptr_array_add(keys, k);
  }
  g_ptr_array_add(keys, NULL);
  *out = (GStrv)g_ptr_array_free(g_steal_pointer(&keys), FALSE);
  return TRUE;
}

GStrv
nn_seal_encrypt_argv(const gchar *seal_exe, const gchar *const *recipients,
                     gboolean to_self, const gchar *path)
{
  g_return_val_if_fail(seal_exe != NULL && path != NULL, NULL);
  /* Absolute path: can never be mistaken for an option by GOption. */
  g_return_val_if_fail(g_path_is_absolute(path), NULL);
  GPtrArray *a = g_ptr_array_new();
  g_ptr_array_add(a, g_strdup(seal_exe));
  g_ptr_array_add(a, g_strdup("encrypt"));
  for (guint i = 0; recipients != NULL && recipients[i] != NULL; i++) {
    g_ptr_array_add(a, g_strdup("--to"));
    g_ptr_array_add(a, g_strdup(recipients[i]));
  }
  if (to_self)
    g_ptr_array_add(a, g_strdup("--to-self"));
  g_ptr_array_add(a, g_strdup(path));
  return steal_strv(a);
}
