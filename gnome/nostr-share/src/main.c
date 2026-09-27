/* main.c - nostr-share command line
 *
 * SPDX-License-Identifier: MIT
 *
 *   nostr-share [--kind N] [--to npub|host'group [--private]] [--title T]
 *               [--dry-run] [--no-ui] [--keep-metadata]
 *               FILE… | -t TEXT | URL…
 *
 * With a display and without --no-ui/--dry-run the libadwaita dialog
 * opens (this is what "Open With → Share to Nostr" runs); otherwise the
 * share happens on the command line.
 *
 * Exit status: 0 ok, 1 usage, 2 bad input / metadata refusal,
 *              3 no signer, 4 upload / publish / nostr-dav failure,
 *              5 queued in the session relay, upstream delivery not
 *                confirmed yet (it keeps delivering; do not re-share).
 */
#include "ns-share.h"
#ifdef NS_HAVE_UI
#include "ns-dialog.h"
#endif

#include <gio/gio.h>
#include <locale.h>
#include <stdio.h>

static int
exit_code_for(const GError *e)
{
  if (e == NULL || e->domain != NS_ERROR)
    return 1;
  switch ((NsError)e->code) {
  case NS_ERROR_NO_SIGNER:  return 3;
  case NS_ERROR_BAD_INPUT:
  case NS_ERROR_BAD_KIND:
  case NS_ERROR_METADATA:
  case NS_ERROR_TOO_LARGE:
  case NS_ERROR_GIT:        return 2;
  case NS_ERROR_NO_RELAYS:
  case NS_ERROR_NO_SERVERS:
  case NS_ERROR_UPLOAD:
  case NS_ERROR_PUBLISH:
  case NS_ERROR_DAV:        return 4;
  case NS_ERROR_QUEUED:     return 5;
  }
  return 1;
}

static int
fail(const GError *e)
{
  g_printerr("nostr-share: %s\n", e ? e->message : "unknown error");
  return exit_code_for(e);
}

static void
print_progress(const gchar *msg, gpointer user_data)
{
  (void)user_data;
  g_printerr("%s\n", msg);
}

static void
print_summary(NsShare *share)
{
  if (share->private_share)
    g_printerr("note: PRIVATE to %s (NIP-17): relays see only a kind-1059 gift wrap "
               "from a throwaway key; files are uploaded AES-256-GCM encrypted (the "
               "Blossom server sees their size, and that your key uploaded them)\n",
               share->to.npub);
  else if (share->to.type == NS_RECIPIENT_MENTION)
    g_printerr("note: this is a PUBLIC post mentioning %s — anyone can read "
               "it; add --private to send it privately\n", share->to.npub);
  if (share->pubkey_hex != NULL || share->to.type == NS_RECIPIENT_GROUP) {
    g_autofree gchar *targets = ns_share_describe_targets(share);
    g_printerr("targets: %s\n", targets);
  }
  if (ns_share_needs_upload(share)) {
    g_autofree gchar *servers = ns_share_describe_servers(share);
    g_printerr("blossom: %s\n", servers);
  }
  for (guint i = 0; i < share->files->len; i++) {
    NsFile *f = g_ptr_array_index(share->files, i);
    if (f->cls != NS_CLASS_MEDIA)
      continue;
    if (f->stripped)
      g_printerr("metadata: %s: removed %u block%s\n", f->display_name,
                 f->n_meta_removed, f->n_meta_removed == 1 ? "" : "s");
    else
      g_printerr("metadata: %s: NOT removed (%s is not supported by the "
                 "stripper)%s\n", f->display_name, f->blob.mime,
                 share->keep_metadata ? "; --keep-metadata given" : "");
  }
}

static int
run_dry(NsShare *share)
{
  GError *err = NULL;
  gboolean have_signer = ns_share_connect(share, &err);
  GError *signer_err = err;
  err = NULL;

  if (have_signer) {
    /* Still show what we can: unresolved targets are not fatal for a
     * preview unless we need a Blossom URL to build the event. */
    if (!ns_share_resolve(share, &err)) {
      g_printerr("nostr-share: warning: %s\n", err->message);
      g_clear_error(&err);
    }
  } else if (ns_share_needs_upload(share)) {
    /* No pubkey → no kind 10063 lookup; the config list still lets us
     * show the predicted blob URL in the draft. */
    share->servers = ns_resolve_blossom_servers(share->cfg, &share->net, NULL,
                                                &share->servers_source, &err);
    if (share->servers == NULL) {
      g_printerr("nostr-share: warning: %s\n", err->message);
      g_clear_error(&err);
    }
  }
  if (!ns_share_build(share, &err)) {
    g_clear_error(&signer_err);
    return fail(err);
  }
  print_summary(share);
  g_autoptr(GString) why = g_string_new(NULL);
  if (ns_share_metadata_blocked(share, why))
    g_printerr("nostr-share: warning: a real run would refuse to upload %s "
               "without --keep-metadata\n", why->str);

  if (!have_signer) {
    /* The draft is the requested output; the diagnosis goes to stderr. */
    g_autofree gchar *draft = ns_share_preview_json(share, FALSE);
    g_print("%s", draft);
    g_printerr("nostr-share: the event above is UNSIGNED\n");
    int rc = fail(signer_err);
    g_clear_error(&signer_err);
    return rc;
  }
  if (!ns_share_sign(share, &err))
    return fail(err);

  g_autofree gchar *out = ns_share_preview_json(share, TRUE);
  g_print("%s", out);
  if (share->private_share)
    g_printerr("dry run: private posts are shown as the unsigned rumor the "
               "recipient would read; nothing was encrypted by the signer, "
               "uploaded or sent\n");
  else
    g_printerr("dry run: nothing was uploaded, published or staged\n");
  return 0;
}

static int
run_cli(NsShare *share)
{
  GError *err = NULL;
  g_autoptr(GString) why = g_string_new(NULL);
  if (ns_share_metadata_blocked(share, why)) {
    g_printerr("nostr-share: cannot strip metadata (location, device, "
               "timestamps) from %s; re-run with --keep-metadata to upload "
               "it anyway\n", why->str);
    return 2;
  }
  if (ns_share_needs_relays(share) || ns_share_needs_upload(share)) {
    if (!ns_share_connect(share, &err) || !ns_share_resolve(share, &err))
      return fail(err);
  }
  print_summary(share);
  gboolean changed = FALSE;
  if (!ns_share_upload(share, &changed, print_progress, NULL, &err) ||
      !ns_share_build(share, &err))
    return fail(err);
  gboolean published = ns_share_publish(share, print_progress, NULL, &err);
  /* Queued posts were signed and handed off: print them like published
   * ones, then exit 5 with the explanation. */
  if (!published && !g_error_matches(err, NS_ERROR, NS_ERROR_QUEUED))
    return fail(err);
  for (guint i = 0; i < share->posts->len; i++) {
    NsPost *p = g_ptr_array_index(share->posts, i);
    if (p->signed_json != NULL)
      g_print("%s\n", p->signed_json);
    if (p->result != NULL)
      g_printerr("%s\n", p->result);
  }
  return published ? 0 : fail(err);
}

static gboolean
have_display(void)
{
  const gchar *w = g_getenv("WAYLAND_DISPLAY");
  const gchar *x = g_getenv("DISPLAY");
  return (w != NULL && *w != '\0') || (x != NULL && *x != '\0');
}

static gchar *
read_stdin(void)
{
  GString *s = g_string_new(NULL);
  gchar buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0)
    g_string_append_len(s, buf, (gssize)n);
  return g_string_free(s, FALSE);
}

int
main(int argc, char **argv)
{
  setlocale(LC_ALL, "");

  gint kind = 0;
  gchar *to = NULL, *title = NULL;
  gchar **texts = NULL, **rest = NULL;
  gboolean dry_run = FALSE, no_ui = FALSE, keep_metadata = FALSE, version = FALSE;
  gboolean private_share = FALSE;
  const GOptionEntry entries[] = {
    { "kind", 'k', 0, G_OPTION_ARG_INT, &kind,
      "Event kind (1, 1063, 30023, 30617); default depends on the input", "N" },
    { "to", 0, 0, G_OPTION_ARG_STRING, &to,
      "Public mention (npub/hex) or NIP-29 group (host'group-id); with --private, "
      "the one recipient", "WHO" },
    { "private", 0, 0, G_OPTION_ARG_NONE, &private_share,
      "Send privately to --to (NIP-17): encrypted, gift-wrapped to their DM inbox "
      "relays; files are encrypted before upload", NULL },
    { "text", 't', 0, G_OPTION_ARG_STRING_ARRAY, &texts,
      "Text to share (repeatable; '-' reads stdin); with files it is the caption",
      "TEXT" },
    { "title", 0, 0, G_OPTION_ARG_STRING, &title, "Title for a long-form article", "T" },
    { "dry-run", 'n', 0, G_OPTION_ARG_NONE, &dry_run,
      "Build and sign, print the event JSON; upload/publish nothing", NULL },
    { "no-ui", 0, 0, G_OPTION_ARG_NONE, &no_ui, "Never open the dialog", NULL },
    { "keep-metadata", 0, 0, G_OPTION_ARG_NONE, &keep_metadata,
      "Allow uploading media whose metadata cannot be stripped", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &version, "Print version", NULL },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &rest, NULL,
      "FILE…|URL…" },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
  };
  g_autoptr(GOptionContext) oc = g_option_context_new("FILE… | -t TEXT | URL…");
  g_option_context_set_summary(oc,
    "Publish text, links, media, articles, git repositories, calendar and\n"
    "contact files to Nostr with the right event kind.");
  g_option_context_add_main_entries(oc, entries, NULL);
  GError *err = NULL;
  if (!g_option_context_parse(oc, &argc, &argv, &err)) {
    g_printerr("nostr-share: %s\n", err->message);
    return 1;
  }
  if (version) {
    g_print("nostr-share %s\n", NS_VERSION);
    return 0;
  }

  for (guint i = 0; texts && texts[i]; i++)
    if (g_str_equal(texts[i], "-")) {
      g_free(texts[i]);
      texts[i] = read_stdin();
    }

  gboolean empty = (texts == NULL || texts[0] == NULL) && (rest == NULL || rest[0] == NULL);
  gboolean ui = !no_ui && !dry_run && have_display();
#ifndef NS_HAVE_UI
  ui = FALSE;
#endif
  if (empty && !ui) {
    g_autofree gchar *help = g_option_context_get_help(oc, TRUE, NULL);
    g_printerr("%s", help);
    return 1;
  }

  NsConfig *cfg = ns_config_load(&err);
  NsShare *share = NULL;
  if (cfg != NULL) {
    NsShareOptions opts = {
      .forced_kind   = kind,
      .to            = to,
      .title         = title,
      .keep_metadata = keep_metadata || cfg->keep_metadata,
      .private_share = private_share,
      .texts         = (const gchar *const *)texts,
      .args          = (const gchar *const *)rest,
    };
    opts.allow_empty = empty;
    share = ns_share_new(cfg, &opts, &err);
  }

  int rc;
#ifdef NS_HAVE_UI
  if (ui) {
    rc = ns_dialog_run(share, err);
    g_clear_error(&err);
    goto out;
  }
#endif
  if (share == NULL) {
    rc = fail(err);
    g_clear_error(&err);
    goto out;
  }
  rc = dry_run ? run_dry(share) : run_cli(share);
  ns_share_free(share);

out:
  g_free(to);
  g_free(title);
  g_strfreev(texts);
  g_strfreev(rest);
  return rc;
}
