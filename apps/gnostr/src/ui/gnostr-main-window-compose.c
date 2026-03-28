#define G_LOG_DOMAIN "gnostr-main-window-compose"

#include "gnostr-main-window-private.h"

#include "gnostr-article-composer.h"
#include "../util/media_upload.h"
#include "../util/gnostr-drafts.h"

#include <nostr-gobject-1.0/nostr_json.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gobject-1.0/storage_ndb.h>

#include <glib/gi18n.h>
#include <string.h>

/* ---- Composer signal handlers (nostr-gtk decoupled signals) ---- */

static void on_composer_toast_requested(NostrGtkComposer *composer,
                                        const char *message,
                                        gpointer user_data) {
  (void)composer;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !message) return;
  gnostr_main_window_show_toast(GTK_WIDGET(self), message);
}

static void on_composer_upload_blossom_done(GnostrBlossomBlob *blob,
                                            GError *error,
                                            gpointer user_data) {
  NostrGtkComposer *composer = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(composer)) {
    if (blob) gnostr_blossom_blob_free(blob);
    return;
  }

  if (error) {
    nostr_gtk_composer_upload_failed(composer, error->message);
    return;
  }

  if (!blob || !blob->url) {
    nostr_gtk_composer_upload_failed(composer, "Server returned no URL");
    return;
  }

  nostr_gtk_composer_upload_complete(composer, blob->url, blob->sha256,
                                     blob->mime_type, blob->size);
  gnostr_blossom_blob_free(blob);
}

static void on_composer_upload_requested(NostrGtkComposer *composer,
                                         const char *file_path,
                                         gpointer user_data) {
  (void)user_data;
  if (!NOSTR_GTK_IS_COMPOSER(composer) || !file_path) return;

  g_message("main-window: handling upload request for %s", file_path);
  gnostr_media_upload_async(file_path, NULL,
                            on_composer_upload_blossom_done, composer,
                            NULL);
}

static void on_draft_save_done(GnostrDrafts *drafts,
                               gboolean success,
                               const char *error_message,
                               gpointer user_data) {
  (void)drafts;
  NostrGtkComposer *composer = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(composer)) return;
  const char *d_tag = nostr_gtk_composer_get_current_draft_d_tag(composer);
  nostr_gtk_composer_draft_save_complete(composer, success, error_message, d_tag);
}

static void on_composer_save_draft_requested(NostrGtkComposer *composer,
                                             gpointer user_data) {
  (void)user_data;
  if (!NOSTR_GTK_IS_COMPOSER(composer)) return;

  g_autofree char *text = nostr_gtk_composer_get_text(composer);
  if (!text || !*text) return;

  GnostrDraft *draft = gnostr_draft_new();
  draft->content = g_strdup(text);
  draft->target_kind = 1;

  const char *current_d = nostr_gtk_composer_get_current_draft_d_tag(composer);
  if (current_d) draft->d_tag = g_strdup(current_d);

  const char *subject = nostr_gtk_composer_get_subject(composer);
  if (subject) draft->subject = g_strdup(subject);

  const char *reply_id = nostr_gtk_composer_get_reply_to_id(composer);
  if (reply_id) draft->reply_to_id = g_strdup(reply_id);

  const char *root_id = nostr_gtk_composer_get_root_id(composer);
  if (root_id) draft->root_id = g_strdup(root_id);

  const char *reply_pk = nostr_gtk_composer_get_reply_to_pubkey(composer);
  if (reply_pk) draft->reply_to_pubkey = g_strdup(reply_pk);

  const char *quote_id = nostr_gtk_composer_get_quote_id(composer);
  if (quote_id) draft->quote_id = g_strdup(quote_id);

  const char *quote_pk = nostr_gtk_composer_get_quote_pubkey(composer);
  if (quote_pk) draft->quote_pubkey = g_strdup(quote_pk);

  const char *quote_uri = nostr_gtk_composer_get_quote_nostr_uri(composer);
  if (quote_uri) draft->quote_nostr_uri = g_strdup(quote_uri);

  draft->is_sensitive = nostr_gtk_composer_is_sensitive(composer);

  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  gnostr_drafts_save_async(drafts_mgr, draft, on_draft_save_done, composer);

  gnostr_draft_free(draft);
}

static void on_composer_load_drafts_requested(NostrGtkComposer *composer,
                                              gpointer user_data) {
  (void)user_data;
  if (!NOSTR_GTK_IS_COMPOSER(composer)) return;

  nostr_gtk_composer_clear_draft_rows(composer);

  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  GPtrArray *drafts = gnostr_drafts_load_local(drafts_mgr);
  if (!drafts || drafts->len == 0) {
    if (drafts) g_ptr_array_free(drafts, TRUE);
    return;
  }

  for (guint i = 0; i < drafts->len; i++) {
    GnostrDraft *d = (GnostrDraft *)g_ptr_array_index(drafts, i);
    const char *content = d->content ? d->content : "";
    char *preview = g_strndup(content, 50);
    for (char *p = preview; *p; p++) {
      if (*p == '\n' || *p == '\r') *p = ' ';
    }
    if (strlen(content) > 50) {
      char *tmp = g_strdup_printf("%s...", preview);
      g_free(preview);
      preview = tmp;
    }
    nostr_gtk_composer_add_draft_row(composer, d->d_tag, preview, d->updated_at);
    g_free(preview);
  }

  g_ptr_array_free(drafts, TRUE);
}

static void on_composer_draft_load_requested(NostrGtkComposer *composer,
                                             const char *d_tag,
                                             gpointer user_data) {
  (void)user_data;
  if (!NOSTR_GTK_IS_COMPOSER(composer) || !d_tag) return;

  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  GPtrArray *drafts = gnostr_drafts_load_local(drafts_mgr);
  if (!drafts) return;

  for (guint i = 0; i < drafts->len; i++) {
    GnostrDraft *d = (GnostrDraft *)g_ptr_array_index(drafts, i);
    if (d->d_tag && strcmp(d->d_tag, d_tag) == 0) {
      NostrGtkComposerDraftInfo info = {
        .d_tag = d->d_tag,
        .content = d->content,
        .subject = d->subject,
        .reply_to_id = d->reply_to_id,
        .root_id = d->root_id,
        .reply_to_pubkey = d->reply_to_pubkey,
        .quote_id = d->quote_id,
        .quote_pubkey = d->quote_pubkey,
        .quote_nostr_uri = d->quote_nostr_uri,
        .is_sensitive = d->is_sensitive,
        .target_kind = d->target_kind,
        .updated_at = d->updated_at,
      };
      nostr_gtk_composer_load_draft(composer, &info);
      break;
    }
  }

  g_ptr_array_free(drafts, TRUE);
}

static void on_draft_delete_done(GnostrDrafts *drafts,
                                 gboolean success,
                                 const char *error_message,
                                 gpointer user_data) {
  (void)drafts;
  (void)error_message;
  NostrGtkComposer *composer = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(composer)) return;
  nostr_gtk_composer_draft_delete_complete(composer, NULL, success);
}

static void on_composer_draft_delete_requested(NostrGtkComposer *composer,
                                               const char *d_tag,
                                               gpointer user_data) {
  (void)user_data;
  if (!NOSTR_GTK_IS_COMPOSER(composer) || !d_tag) return;

  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  gnostr_drafts_delete_async(drafts_mgr, d_tag, on_draft_delete_done, composer);
}

static void connect_composer_signals(NostrGtkComposer *composer,
                                     GnostrMainWindow *self) {
  g_signal_connect(composer, "toast-requested",
                   G_CALLBACK(on_composer_toast_requested), self);
  g_signal_connect(composer, "upload-requested",
                   G_CALLBACK(on_composer_upload_requested), self);
  g_signal_connect(composer, "save-draft-requested",
                   G_CALLBACK(on_composer_save_draft_requested), self);
  g_signal_connect(composer, "load-drafts-requested",
                   G_CALLBACK(on_composer_load_drafts_requested), self);
  g_signal_connect(composer, "draft-load-requested",
                   G_CALLBACK(on_composer_draft_load_requested), self);
  g_signal_connect(composer, "draft-delete-requested",
                   G_CALLBACK(on_composer_draft_delete_requested), self);
}

static gboolean
hex_to_bytes32_local(const char *hex, uint8_t out[32])
{
  if (!hex || !out)
    return FALSE;

  size_t L = strlen(hex);
  if (L != 64)
    return FALSE;

  for (int i = 0; i < 32; i++) {
    char c1 = hex[i * 2];
    char c2 = hex[i * 2 + 1];
    int v1, v2;
    if      (c1 >= '0' && c1 <= '9') v1 = c1 - '0';
    else if (c1 >= 'a' && c1 <= 'f') v1 = 10 + (c1 - 'a');
    else if (c1 >= 'A' && c1 <= 'F') v1 = 10 + (c1 - 'A');
    else return FALSE;
    if      (c2 >= '0' && c2 <= '9') v2 = c2 - '0';
    else if (c2 >= 'a' && c2 <= 'f') v2 = 10 + (c2 - 'a');
    else if (c2 >= 'A' && c2 <= 'F') v2 = 10 + (c2 - 'A');
    else return FALSE;
    out[i] = (uint8_t)((v1 << 4) | v2);
  }

  return TRUE;
}

static char *
lookup_display_name_local(const char *pubkey_hex)
{
  if (!pubkey_hex || strlen(pubkey_hex) != 64)
    return NULL;

  char *display_name = NULL;
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
    uint8_t pk32[32];
    if (hex_to_bytes32_local(pubkey_hex, pk32)) {
      char *meta_json = NULL;
      int meta_len = 0;
      if (storage_ndb_get_profile_by_pubkey(txn, pk32, &meta_json, &meta_len, NULL) == 0 && meta_json) {
        char *dn = gnostr_json_get_string(meta_json, "display_name", NULL);
        if (!dn || !*dn) {
          g_free(dn);
          dn = gnostr_json_get_string(meta_json, "name", NULL);
        }
        if (dn && *dn) {
          display_name = dn;
        } else {
          g_free(dn);
        }
      }
    }
    storage_ndb_end_query(txn);
  }

  return display_name;
}

void gnostr_main_window_compose_context_free_internal(ComposeContext *ctx) {
  if (!ctx) return;
  g_free(ctx->reply_to_id);
  g_free(ctx->root_id);
  g_free(ctx->reply_to_pubkey);
  g_free(ctx->display_name);
  g_free(ctx->quote_id);
  g_free(ctx->quote_pubkey);
  g_free(ctx->nostr_uri);
  g_free(ctx->comment_root_id);
  g_free(ctx->comment_root_pubkey);
  g_free(ctx);
}

void gnostr_main_window_open_compose_dialog_internal(GnostrMainWindow *self,
                                                     ComposeContext *context) {
  if (!GNOSTR_IS_MAIN_WINDOW(self)) {
    gnostr_main_window_compose_context_free_internal(context);
    return;
  }

  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_content_width(dialog, 500);
  adw_dialog_set_content_height(dialog, 400);

  const char *title = _("New Note");
  if (context) {
    switch (context->type) {
      case COMPOSE_CONTEXT_REPLY:
        title = _("Reply");
        break;
      case COMPOSE_CONTEXT_QUOTE:
        title = _("Quote");
        break;
      case COMPOSE_CONTEXT_COMMENT:
        title = _("Comment");
        break;
      default:
        break;
    }
  }
  adw_dialog_set_title(dialog, title);

  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));

  GtkWidget *composer = nostr_gtk_composer_new();
  g_signal_connect(composer, "post-requested",
                   G_CALLBACK(gnostr_main_window_handle_composer_post_requested), self);
  connect_composer_signals(NOSTR_GTK_COMPOSER(composer), self);
  g_object_set_data(G_OBJECT(composer), "compose-dialog", dialog);

  if (context) {
    switch (context->type) {
      case COMPOSE_CONTEXT_REPLY:
        nostr_gtk_composer_set_reply_context(NOSTR_GTK_COMPOSER(composer),
                                             context->reply_to_id,
                                             context->root_id,
                                             context->reply_to_pubkey,
                                             context->display_name);
        break;
      case COMPOSE_CONTEXT_QUOTE:
        nostr_gtk_composer_set_quote_context(NOSTR_GTK_COMPOSER(composer),
                                             context->quote_id,
                                             context->quote_pubkey,
                                             context->nostr_uri,
                                             context->display_name);
        break;
      case COMPOSE_CONTEXT_COMMENT:
        nostr_gtk_composer_set_comment_context(NOSTR_GTK_COMPOSER(composer),
                                               context->comment_root_id,
                                               context->comment_root_kind,
                                               context->comment_root_pubkey,
                                               context->display_name);
        break;
      default:
        break;
    }
  }

  adw_toolbar_view_set_content(toolbar, composer);
  adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));
  adw_dialog_present(dialog, GTK_WIDGET(self));

  gnostr_main_window_compose_context_free_internal(context);
}

void gnostr_main_window_request_reply(GtkWidget *window,
                                       const char *id_hex,
                                       const char *root_id,
                                       const char *pubkey_hex)
 {
   if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
   GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
   if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
 
   g_debug("[REPLY] Request reply to id=%s root=%s pubkey=%.8s...",
           id_hex ? id_hex : "(null)",
           root_id ? root_id : "(null)",
           pubkey_hex ? pubkey_hex : "(null)");
 
   ComposeContext *ctx = g_new0(ComposeContext, 1);
   ctx->type = COMPOSE_CONTEXT_REPLY;
   ctx->reply_to_id = g_strdup(id_hex);
   ctx->root_id = g_strdup(root_id ? root_id : id_hex);
   ctx->reply_to_pubkey = g_strdup(pubkey_hex);
   ctx->display_name = lookup_display_name_local(pubkey_hex);
   gnostr_main_window_open_compose_dialog_internal(self, ctx);
 }
 
 void gnostr_main_window_request_quote(GtkWidget *window,
                                       const char *id_hex,
                                       const char *pubkey_hex)
 {
   if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
   GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
   if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
 
   g_debug("[QUOTE] Request quote of id=%s pubkey=%.8s...",
           id_hex ? id_hex : "(null)",
           pubkey_hex ? pubkey_hex : "(null)");
 
   if (!id_hex || strlen(id_hex) != 64) {
     gnostr_main_window_show_toast(GTK_WIDGET(self), "Invalid event ID for quote");
     return;
   }
 
   g_autoptr(GNostrNip19) n19_note = gnostr_nip19_encode_note(id_hex, NULL);
   if (!n19_note) {
     gnostr_main_window_show_toast(GTK_WIDGET(self), "Failed to encode note ID");
     return;
   }
 
   ComposeContext *ctx = g_new0(ComposeContext, 1);
   ctx->type = COMPOSE_CONTEXT_QUOTE;
   ctx->quote_id = g_strdup(id_hex);
   ctx->quote_pubkey = g_strdup(pubkey_hex);
   ctx->nostr_uri = g_strdup_printf("nostr:%s", gnostr_nip19_get_bech32(n19_note));
   ctx->display_name = lookup_display_name_local(pubkey_hex);
   gnostr_main_window_open_compose_dialog_internal(self, ctx);
 }
 
 void gnostr_main_window_request_comment(GtkWidget *window,
                                         const char *id_hex,
                                         int kind,
                                         const char *pubkey_hex)
 {
   if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
   GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
   if (!GNOSTR_IS_MAIN_WINDOW(self)) return;
 
   g_debug("[COMMENT] Request comment on id=%s kind=%d pubkey=%.8s...",
           id_hex ? id_hex : "(null)",
           kind,
           pubkey_hex ? pubkey_hex : "(null)");
 
   if (!id_hex || strlen(id_hex) != 64) {
     gnostr_main_window_show_toast(GTK_WIDGET(self), "Invalid event ID for comment");
     return;
   }
 
   ComposeContext *ctx = g_new0(ComposeContext, 1);
   ctx->type = COMPOSE_CONTEXT_COMMENT;
   ctx->comment_root_id = g_strdup(id_hex);
   ctx->comment_root_kind = kind;
   ctx->comment_root_pubkey = g_strdup(pubkey_hex);
   ctx->display_name = lookup_display_name_local(pubkey_hex);
   gnostr_main_window_open_compose_dialog_internal(self, ctx);
 }
 
 void gnostr_main_window_on_compose_requested_internal(GnostrSessionView *session_view,
                                                      gpointer user_data) {
  (void)session_view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  gnostr_main_window_open_compose_dialog_internal(self, NULL);
}

static void on_article_compose_publish(GnostrArticleComposer *composer,
                                       gboolean is_draft,
                                       gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  const char *title = gnostr_article_composer_get_title(composer);
  g_autofree char *content = gnostr_article_composer_get_content(composer);
  const char *d_tag = gnostr_article_composer_get_d_tag(composer);

  if (!title || !*title) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), "Title is required");
    return;
  }
  if (!content || !*content) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), "Content is required");
    return;
  }

  const char *action = is_draft ? "Draft saved" : "Article published";
  g_autofree char *msg = g_strdup_printf("%s: %s", action, title);
  gnostr_main_window_show_toast(GTK_WIDGET(self), msg);

  g_debug("[ARTICLE-COMPOSER] %s: title=%s, d_tag=%s, draft=%d",
          action, title, d_tag ? d_tag : "(none)", is_draft);

  AdwDialog *dialog = ADW_DIALOG(g_object_get_data(G_OBJECT(composer), "compose-dialog"));
  if (dialog)
    adw_dialog_close(dialog);
}

void gnostr_main_window_compose_article(GtkWidget *window) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, _("Write Article"));
  adw_dialog_set_content_width(dialog, 700);
  adw_dialog_set_content_height(dialog, 600);

  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));

  GtkWidget *composer = gnostr_article_composer_new();
  g_object_set_data(G_OBJECT(composer), "compose-dialog", dialog);
  g_signal_connect(composer, "publish-requested",
                   G_CALLBACK(on_article_compose_publish), self);
  adw_toolbar_view_set_content(toolbar, composer);
  adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));
  adw_dialog_present(dialog, GTK_WIDGET(self));
}
