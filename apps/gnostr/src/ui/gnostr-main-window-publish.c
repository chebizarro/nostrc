#define G_LOG_DOMAIN "gnostr-main-window-publish"

#include "gnostr-main-window-private.h"

#include "gnostr-report-dialog.h"
#include "../ipc/gnostr-signer-service.h"
#include "../util/relay_info.h"
#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr-kinds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PUBLISH_ACK_TIMEOUT_SECONDS 15

typedef struct _PublishContext PublishContext;
typedef struct _LikeContext LikeContext;
typedef struct _PublishRelayAttempt PublishRelayAttempt;

struct _PublishContext {
  GnostrMainWindow *self;
  char *text;
  GWeakRef composer_ref;
  NostrEvent *event;
  char *signed_event_json;
  char *event_id;
  GPtrArray *attempts; /* PublishRelayAttempt* */
  GString *failure_reasons;
  GString *relay_outcomes;
  guint success_count;
  guint fail_count;
  guint limit_skip_count;
  char *limit_warnings;
  gboolean local_ingested;
  gboolean finalized;
};

struct _PublishRelayAttempt {
  PublishContext *ctx;
  GNostrRelay *relay;
  char *url;
  gulong ok_handler_id;
  gulong state_handler_id;
  gulong error_handler_id;
  guint ack_timeout_id;
  gboolean publish_requested;
  gboolean settled;
};

static void publish_relay_attempt_free(PublishRelayAttempt *attempt);
static void publish_finish_if_settled(PublishContext *ctx);
static void publish_relay_attempt_dispatch(PublishRelayAttempt *attempt);
static gboolean publish_relay_attempt_ack_timeout(gpointer user_data);
static void on_publish_relay_connected(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_publish_relay_ok(GNostrRelay *relay, const char *event_id, gboolean accepted, const char *message, gpointer user_data);
static void on_publish_relay_state_changed(GNostrRelay *relay, GNostrRelayState old_state, GNostrRelayState new_state, gpointer user_data);
static void on_publish_relay_error(GNostrRelay *relay, GError *error, gpointer user_data);

static void publish_context_free(PublishContext *ctx) {
  if (!ctx) return;
  g_clear_object(&ctx->self);
  g_free(ctx->text);
  g_weak_ref_clear(&ctx->composer_ref);
  if (ctx->event) nostr_event_free(ctx->event);
  g_free(ctx->signed_event_json);
  g_clear_pointer(&ctx->event_id, free);
  if (ctx->attempts) g_ptr_array_free(ctx->attempts, TRUE);
  if (ctx->failure_reasons) g_string_free(ctx->failure_reasons, TRUE);
  if (ctx->relay_outcomes) g_string_free(ctx->relay_outcomes, TRUE);
  g_free(ctx->limit_warnings);
  g_free(ctx);
}

static void
publish_relay_attempt_free(PublishRelayAttempt *attempt)
{
  if (!attempt) return;
  if (attempt->ack_timeout_id)
    g_source_remove(attempt->ack_timeout_id);
  if (attempt->relay) {
    if (attempt->ok_handler_id)
      g_signal_handler_disconnect(attempt->relay, attempt->ok_handler_id);
    if (attempt->state_handler_id)
      g_signal_handler_disconnect(attempt->relay, attempt->state_handler_id);
    if (attempt->error_handler_id)
      g_signal_handler_disconnect(attempt->relay, attempt->error_handler_id);
    g_object_unref(attempt->relay);
  }
  g_free(attempt->url);
  g_free(attempt);
}

typedef struct {
  NostrEvent *event;
  GPtrArray *relay_urls;
  char *signed_event_json;
  guint success_count;
  guint fail_count;
  guint limit_skip_count;
  char *limit_warnings;
} RelayPublishResult;

static void relay_publish_result_free(RelayPublishResult *r) {
  if (!r) return;
  if (r->event) nostr_event_free(r->event);
  if (r->relay_urls) g_ptr_array_free(r->relay_urls, TRUE);
  g_free(r->signed_event_json);
  g_free(r->limit_warnings);
  g_free(r);
}

static void relay_publish_thread(GTask *task, gpointer source_object,
                                 gpointer task_data, GCancellable *cancellable);
static void on_sign_event_complete(GObject *source, GAsyncResult *res, gpointer user_data);

struct _LikeContext {
  GnostrMainWindow *self;
  char *event_id_hex;
  GWeakRef row_ref;
};

static void like_context_free(LikeContext *ctx) {
  if (!ctx) return;
  g_clear_object(&ctx->self);
  g_free(ctx->event_id_hex);
  g_weak_ref_clear(&ctx->row_ref);
  g_free(ctx);
}

static void on_sign_like_event_complete(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_like_publish_done(GObject *source, GAsyncResult *res, gpointer user_data);

/* Worker thread: connect + publish to each relay without blocking main thread.
 * The nostr C relay layer is thread-safe (uses nsync mutexes and channels). */
static void relay_publish_thread(GTask *task, gpointer source_object,
                                  gpointer task_data, GCancellable *cancellable) {
  (void)source_object; (void)cancellable;
  RelayPublishResult *r = (RelayPublishResult*)task_data;

  const char *content = nostr_event_get_content(r->event);
  gint content_len = content ? (gint)strlen(content) : 0;
  NostrTags *tags = nostr_event_get_tags(r->event);
  gint tag_count = tags ? (gint)nostr_tags_size(tags) : 0;
  gint64 created_at = nostr_event_get_created_at(r->event);
  gssize serialized_len = r->signed_event_json ? (gssize)strlen(r->signed_event_json) : -1;

  GString *warnings = g_string_new(NULL);

  for (guint i = 0; i < r->relay_urls->len; i++) {
    const char *url = (const char*)g_ptr_array_index(r->relay_urls, i);

    /* NIP-11: Check relay limitations before publishing */
    GnostrRelayInfo *relay_info = gnostr_relay_info_cache_get(url);
    if (relay_info) {
      GnostrRelayValidationResult *validation = gnostr_relay_info_validate_event(
        relay_info, content, content_len, tag_count, created_at, serialized_len);
      if (!gnostr_relay_validation_result_is_valid(validation)) {
        gchar *errors = gnostr_relay_validation_result_format_errors(validation);
        if (errors) {
          if (warnings->len > 0) g_string_append(warnings, "\n");
          g_string_append(warnings, errors);
          g_free(errors);
        }
        gnostr_relay_validation_result_free(validation);
        gnostr_relay_info_free(relay_info);
        r->limit_skip_count++;
        continue;
      }
      gnostr_relay_validation_result_free(validation);

      GnostrRelayValidationResult *pub_validation =
          gnostr_relay_info_validate_for_publishing(relay_info);
      if (!gnostr_relay_validation_result_is_valid(pub_validation)) {
        gchar *errors = gnostr_relay_validation_result_format_errors(pub_validation);
        if (errors) {
          if (warnings->len > 0) g_string_append(warnings, "\n");
          g_string_append(warnings, errors);
          g_free(errors);
        }
        gnostr_relay_validation_result_free(pub_validation);
        gnostr_relay_info_free(relay_info);
        r->limit_skip_count++;
        continue;
      }
      gnostr_relay_validation_result_free(pub_validation);
      gnostr_relay_info_free(relay_info);
    }

    g_autoptr(GNostrRelay) relay = gnostr_relay_new(url);
    if (!relay) { r->fail_count++; continue; }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_clear_error(&conn_err);
      r->fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, r->event, &pub_err)) {
      r->success_count++;
    } else {
      g_clear_error(&pub_err);
      r->fail_count++;
    }
    /* nostrc-pub1: Do NOT disconnect — just release our reference.
     * gnostr_relay_new() returns the SHARED relay from the registry
     * (nostrc-kw9r), so calling gnostr_relay_disconnect() here:
     *   (a) Closes send_channel BEFORE LWS writes the EVENT frame to
     *       the socket — the 5s write confirmation in nostr_relay_publish
     *       only confirms enqueue to send_channel, not actual socket
     *       write.  Disconnect kills the message.
     *   (b) Disconnects the shared relay used by the live pool and all
     *       other app components — breaking subscriptions.
     *   (c) Emits GObject signals from this worker thread via
     *       gnostr_relay_set_state_internal — same class of bug as
     *       nostrc-blk2 (freezes GTK).
     *
     * The old UAF concern (relay_free_impl freeing connection before
     * workers exit) was fixed in nostrc-ws1: channels now close BEFORE
     * go_wait_group_wait.  And for shared relays, other refs keep the
     * relay alive — unref just decrements. */
  }

  r->limit_warnings = g_string_free(warnings, FALSE);
  g_task_return_pointer(task, r, NULL); /* caller frees */
}

static void
publish_relay_attempt_mark_result(PublishRelayAttempt *attempt,
                                  gboolean             accepted,
                                  const char          *failure_class,
                                  const char          *reason)
{
  PublishContext *ctx;

  g_return_if_fail(attempt != NULL);
  g_return_if_fail(attempt->ctx != NULL);

  if (attempt->settled)
    return;

  ctx = attempt->ctx;
  attempt->settled = TRUE;

  if (attempt->ack_timeout_id) {
    g_source_remove(attempt->ack_timeout_id);
    attempt->ack_timeout_id = 0;
  }

  if (!ctx->relay_outcomes)
    ctx->relay_outcomes = g_string_new(NULL);
  if (ctx->relay_outcomes->len > 0)
    g_string_append(ctx->relay_outcomes, "\n");
  g_string_append_printf(ctx->relay_outcomes,
                         "%s | class=%s | %s",
                         attempt->url ? attempt->url : "(unknown relay)",
                         failure_class && *failure_class ? failure_class : (accepted ? "accepted" : "publish_failed"),
                         reason && *reason ? reason : (accepted ? "relay OK accepted" : "publish not acknowledged"));

  if (accepted) {
    ctx->success_count++;
    g_debug("[PUBLISH] relay=%s class=%s message=%s",
              attempt->url ? attempt->url : "(unknown relay)",
              failure_class && *failure_class ? failure_class : "accepted",
              reason && *reason ? reason : "relay OK accepted");
    if (!ctx->local_ingested && ctx->signed_event_json && *ctx->signed_event_json) {
      GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
      g_ptr_array_add(b, g_strdup(ctx->signed_event_json));
      storage_ndb_ingest_events_async(b);
      ctx->local_ingested = TRUE;
      g_debug("[PUBLISH] Ingested authored event locally after first accepted relay OK");
    }
  } else {
    ctx->fail_count++;
    if (!ctx->failure_reasons)
      ctx->failure_reasons = g_string_new(NULL);
    if (ctx->failure_reasons->len > 0)
      g_string_append(ctx->failure_reasons, "\n");
    g_string_append_printf(ctx->failure_reasons,
                           "%s [%s]: %s",
                           attempt->url ? attempt->url : "(unknown relay)",
                           failure_class && *failure_class ? failure_class : "publish_failed",
                           reason && *reason ? reason : "publish not acknowledged");
    g_warning("[PUBLISH] relay=%s class=%s message=%s",
              attempt->url ? attempt->url : "(unknown relay)",
              failure_class && *failure_class ? failure_class : "publish_failed",
              reason && *reason ? reason : "publish not acknowledged");
  }

  publish_finish_if_settled(ctx);
}

static void
publish_relay_attempt_dispatch(PublishRelayAttempt *attempt)
{
  g_autoptr(GError) pub_err = NULL;

  g_return_if_fail(attempt != NULL);
  g_return_if_fail(attempt->ctx != NULL);
  g_return_if_fail(attempt->relay != NULL);

  if (attempt->settled || attempt->publish_requested)
    return;

  if (!attempt->ctx->event) {
    publish_relay_attempt_mark_result(attempt, FALSE, "missing_event", "missing event");
    return;
  }

  if (!gnostr_relay_publish(attempt->relay, attempt->ctx->event, &pub_err)) {
    publish_relay_attempt_mark_result(attempt, FALSE,
                                      "enqueue_failed",
                                      pub_err && pub_err->message ? pub_err->message : "publish enqueue failed");
    return;
  }

  attempt->publish_requested = TRUE;
  attempt->ack_timeout_id = g_timeout_add_seconds(PUBLISH_ACK_TIMEOUT_SECONDS,
                                                  publish_relay_attempt_ack_timeout,
                                                  attempt);
  g_debug("[PUBLISH] Event sent to relay %s, waiting for OK",
          attempt->url ? attempt->url : "(unknown)");
}

static gboolean
publish_relay_attempt_ack_timeout(gpointer user_data)
{
  PublishRelayAttempt *attempt = user_data;

  g_return_val_if_fail(attempt != NULL, G_SOURCE_REMOVE);

  attempt->ack_timeout_id = 0;

  if (!attempt->settled && attempt->publish_requested) {
    publish_relay_attempt_mark_result(attempt, FALSE, "ack_timeout", "timeout waiting for relay OK");
  }

  return G_SOURCE_REMOVE;
}

static void
publish_finish_if_settled(PublishContext *ctx)
{
  guint unsettled = 0;

  g_return_if_fail(ctx != NULL);

  if (ctx->finalized)
    return;

  if (ctx->attempts) {
    for (guint i = 0; i < ctx->attempts->len; i++) {
      PublishRelayAttempt *attempt = g_ptr_array_index(ctx->attempts, i);
      if (attempt && !attempt->settled)
        unsettled++;
    }
  }

  if (unsettled > 0)
    return;

  ctx->finalized = TRUE;

  if (!ctx->self || !GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    publish_context_free(ctx);
    return;
  }

  if (ctx->success_count > 0) {
    g_autofree char *msg = NULL;
    if (ctx->fail_count > 0 && ctx->limit_skip_count > 0) {
      msg = g_strdup_printf("Published to %u relay%s, %u rejected/failed, %u skipped due to limits",
                            ctx->success_count, ctx->success_count == 1 ? "" : "s",
                            ctx->fail_count, ctx->limit_skip_count);
    } else if (ctx->fail_count > 0) {
      msg = g_strdup_printf("Published to %u relay%s, %u rejected/failed",
                            ctx->success_count, ctx->success_count == 1 ? "" : "s",
                            ctx->fail_count);
    } else if (ctx->limit_skip_count > 0) {
      msg = g_strdup_printf("Published to %u relay%s (%u skipped due to limits)",
                            ctx->success_count, ctx->success_count == 1 ? "" : "s",
                            ctx->limit_skip_count);
    } else {
      msg = g_strdup_printf("Published to %u relay%s",
                            ctx->success_count, ctx->success_count == 1 ? "" : "s");
    }
    gnostr_main_window_show_toast_internal(ctx->self, msg);

    g_autoptr(GObject) composer_obj = g_weak_ref_get(&ctx->composer_ref);
    if (composer_obj && NOSTR_GTK_IS_COMPOSER(composer_obj)) {
      NostrGtkComposer *composer = NOSTR_GTK_COMPOSER(composer_obj);
      GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(composer));
      AdwDialog *dialog = ADW_DIALOG(g_object_get_data(G_OBJECT(composer), "compose-dialog"));
      if (root != NULL && dialog && ADW_IS_DIALOG(dialog)) {
        adw_dialog_force_close(dialog);
        nostr_gtk_composer_clear(composer);
      }
    }

    if (ctx->self->session_view && GNOSTR_IS_SESSION_VIEW(ctx->self->session_view))
      gnostr_session_view_show_page(ctx->self->session_view, "timeline");
  } else if (ctx->limit_skip_count > 0 && ctx->limit_warnings && *ctx->limit_warnings) {
    g_autofree char *msg = g_strdup_printf("Event exceeds relay limits:\n%s", ctx->limit_warnings);
    gnostr_main_window_show_toast_internal(ctx->self, msg);
  } else {
    gnostr_main_window_show_toast_internal(ctx->self, "Failed to publish to any relay");
  }

  if (ctx->limit_warnings && *ctx->limit_warnings)
    g_warning("[PUBLISH] Relay limit violations:\n%s", ctx->limit_warnings);
  if (ctx->failure_reasons && ctx->failure_reasons->len > 0)
    g_warning("[PUBLISH] Relay publish failures:\n%s", ctx->failure_reasons->str);
  if (ctx->relay_outcomes && ctx->relay_outcomes->len > 0)
    g_message("[PUBLISH] Relay publish outcomes:\n%s", ctx->relay_outcomes->str);

  publish_context_free(ctx);
}

static void
on_publish_relay_ok(GNostrRelay *relay G_GNUC_UNUSED,
                    const char  *event_id,
                    gboolean     accepted,
                    const char  *message,
                    gpointer     user_data)
{
  PublishRelayAttempt *attempt = user_data;

  g_return_if_fail(attempt != NULL);
  g_return_if_fail(attempt->ctx != NULL);

  if (attempt->settled || !attempt->ctx->event_id)
    return;

  if (g_strcmp0(event_id, attempt->ctx->event_id) != 0)
    return;

  publish_relay_attempt_mark_result(attempt,
                                    accepted,
                                    accepted ? "accepted" : "relay_rejected",
                                    message);
}

static void
on_publish_relay_state_changed(GNostrRelay     *relay G_GNUC_UNUSED,
                               GNostrRelayState old_state G_GNUC_UNUSED,
                               GNostrRelayState new_state,
                               gpointer         user_data)
{
  PublishRelayAttempt *attempt = user_data;

  g_return_if_fail(attempt != NULL);

  if (attempt->settled)
    return;

  if (new_state == GNOSTR_RELAY_STATE_CONNECTED) {
    publish_relay_attempt_dispatch(attempt);
    return;
  }

  if (attempt->publish_requested &&
      (new_state == GNOSTR_RELAY_STATE_DISCONNECTED || new_state == GNOSTR_RELAY_STATE_ERROR)) {
    publish_relay_attempt_mark_result(attempt, FALSE, "disconnect_before_ok", "relay disconnected before OK");
  }
}

static void
on_publish_relay_error(GNostrRelay *relay G_GNUC_UNUSED,
                       GError      *error,
                       gpointer     user_data)
{
  PublishRelayAttempt *attempt = user_data;

  g_return_if_fail(attempt != NULL);

  if (attempt->settled)
    return;

  if (attempt->publish_requested) {
    publish_relay_attempt_mark_result(attempt, FALSE,
                                      "relay_error",
                                      error && error->message ? error->message : "relay error");
  }
}

static void
on_publish_relay_connected(GObject *source, GAsyncResult *res, gpointer user_data)
{
  PublishRelayAttempt *attempt = user_data;
  g_autoptr(GError) error = NULL;

  g_return_if_fail(attempt != NULL);

  if (attempt->settled)
    return;

  if (!gnostr_relay_connect_finish(GNOSTR_RELAY(source), res, &error)) {
    publish_relay_attempt_mark_result(attempt, FALSE,
                                      "connect_failed",
                                      error && error->message ? error->message : "connect failed");
    return;
  }

  publish_relay_attempt_dispatch(attempt);
}

/* Callback when unified signer service completes signing */
static void on_sign_event_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  PublishContext *ctx = (PublishContext*)user_data;
  (void)source; /* Not used with unified signer service */

  if (!ctx || !GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    publish_context_free(ctx);
    return;
  }
  GnostrMainWindow *self = ctx->self;

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    g_autofree char *msg = g_strdup_printf("Signing failed: %s", error ? error->message : "unknown error");
    gnostr_main_window_show_toast_internal(self, msg);
    g_clear_error(&error);
    publish_context_free(ctx);
    return;
  }

  g_debug("[PUBLISH] Signed event: %.100s...", signed_event_json);

  /* Parse the signed event JSON into a NostrEvent */
  NostrEvent *event = nostr_event_new();
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
  if (parse_rc != 1) {
    gnostr_main_window_show_toast_internal(self, "Failed to parse signed event");
    nostr_event_free(event);
    g_free(signed_event_json);
    publish_context_free(ctx);
    return;
  }

  ctx->event = event;
  ctx->event_id = nostr_event_get_id(event);
  ctx->attempts = g_ptr_array_new_with_free_func((GDestroyNotify)publish_relay_attempt_free);

  if (!ctx->event_id || !*ctx->event_id) {
    gnostr_main_window_show_toast_internal(self, "Failed to determine signed event ID");
    g_free(signed_event_json);
    publish_context_free(ctx);
    return;
  }

  g_autoptr(GPtrArray) relay_urls = gnostr_get_write_relay_urls();
  if (!relay_urls || relay_urls->len == 0) {
    gnostr_main_window_show_toast_internal(self, "No write relays configured");
    g_free(signed_event_json);
    publish_context_free(ctx);
    return;
  }

  const char *content = nostr_event_get_content(event);
  gint content_len = content ? (gint)strlen(content) : 0;
  NostrTags *tags = nostr_event_get_tags(event);
  gint tag_count = tags ? (gint)nostr_tags_size(tags) : 0;
  gint64 created_at = nostr_event_get_created_at(event);
  gssize serialized_len = (gssize)strlen(signed_event_json);
  GString *warnings = g_string_new(NULL);

  for (guint i = 0; i < relay_urls->len; i++) {
    const char *url = g_ptr_array_index(relay_urls, i);
    gboolean allowed = TRUE;

    GnostrRelayInfo *relay_info = gnostr_relay_info_cache_get(url);
    if (relay_info) {
      GnostrRelayValidationResult *validation =
        gnostr_relay_info_validate_event(relay_info, content, content_len, tag_count, created_at, serialized_len);
      if (!gnostr_relay_validation_result_is_valid(validation)) {
        gchar *errors = gnostr_relay_validation_result_format_errors(validation);
        if (errors) {
          if (warnings->len > 0) g_string_append(warnings, "\n");
          g_string_append(warnings, errors);
          g_free(errors);
        }
        ctx->limit_skip_count++;
        allowed = FALSE;
      }
      gnostr_relay_validation_result_free(validation);

      if (allowed) {
        GnostrRelayValidationResult *pub_validation =
          gnostr_relay_info_validate_for_publishing(relay_info);
        if (!gnostr_relay_validation_result_is_valid(pub_validation)) {
          gchar *errors = gnostr_relay_validation_result_format_errors(pub_validation);
          if (errors) {
            if (warnings->len > 0) g_string_append(warnings, "\n");
            g_string_append(warnings, errors);
            g_free(errors);
          }
          ctx->limit_skip_count++;
          allowed = FALSE;
        }
        gnostr_relay_validation_result_free(pub_validation);
      }
      gnostr_relay_info_free(relay_info);
    }

    if (!allowed)
      continue;

    PublishRelayAttempt *attempt = g_new0(PublishRelayAttempt, 1);
    attempt->ctx = ctx;
    attempt->relay = gnostr_relay_new(url);
    attempt->url = g_strdup(url);
    if (!attempt->relay) {
      g_free(attempt->url);
      g_free(attempt);
      ctx->fail_count++;
      continue;
    }

    attempt->ok_handler_id = g_signal_connect(attempt->relay, "ok",
                                              G_CALLBACK(on_publish_relay_ok), attempt);
    attempt->state_handler_id = g_signal_connect(attempt->relay, "state-changed",
                                                 G_CALLBACK(on_publish_relay_state_changed), attempt);
    attempt->error_handler_id = g_signal_connect(attempt->relay, "error",
                                                 G_CALLBACK(on_publish_relay_error), attempt);
    g_ptr_array_add(ctx->attempts, attempt);
  }

  ctx->limit_warnings = g_string_free(warnings, FALSE);
  ctx->signed_event_json = signed_event_json;

  if (ctx->attempts->len == 0) {
    publish_finish_if_settled(ctx);
    return;
  }

  g_autofree char *progress_msg = g_strdup_printf("Publishing to %u relay%s...",
                                                  ctx->attempts->len,
                                                  ctx->attempts->len == 1 ? "" : "s");
  gnostr_main_window_show_toast_internal(self, progress_msg);

  for (guint i = 0; i < ctx->attempts->len; i++) {
    PublishRelayAttempt *attempt = g_ptr_array_index(ctx->attempts, i);
    if (gnostr_relay_get_connected(attempt->relay))
      publish_relay_attempt_dispatch(attempt);
    else
      gnostr_relay_connect_async(attempt->relay, NULL, on_publish_relay_connected, attempt);
  }
}

/* Public wrapper for requesting a repost (kind 6) - must be after PublishContext is defined */
void gnostr_main_window_request_repost(GtkWidget *window, const char *id_hex, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[REPOST] Request repost of id=%s pubkey=%.8s...",
            id_hex ? id_hex : "(null)",
            pubkey_hex ? pubkey_hex : "(null)");

  if (!id_hex || strlen(id_hex) != 64) {
    gnostr_main_window_show_toast_internal(self, "Invalid event ID for repost");
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    gnostr_main_window_show_toast_internal(self, "Signer not available");
    return;
  }

  gnostr_main_window_show_toast_internal(self, "Reposting...");

  /* Build unsigned kind 6 repost event JSON using GNostrJsonBuilder */
  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, 6);
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, "");

  /* Build tags array: e-tag and p-tag */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* e-tag: ["e", "<reposted-event-id>", "<relay-hint>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "e");
  gnostr_json_builder_add_string(builder, id_hex);
  gnostr_json_builder_add_string(builder, ""); /* relay hint - empty for now */
  gnostr_json_builder_end_array(builder);

  /* p-tag: ["p", "<original-author-pubkey>"] */
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p");
    gnostr_json_builder_add_string(builder, pubkey_hex);
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  /* Serialize */
  char *event_json = gnostr_json_builder_finish(builder);

  if (!event_json) {
    gnostr_main_window_show_toast_internal(self, "Failed to serialize repost event");
    return;
  }

  g_debug("[REPOST] Unsigned event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = g_object_ref(self);
  ctx->text = g_strdup(""); /* repost has no text content */
  g_weak_ref_init(&ctx->composer_ref, NULL);

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_event_complete,
    ctx
  );
  g_free(event_json);
}

/* ================= NIP-09 Event Deletion Implementation ================= */

/* Public wrapper for requesting deletion of a note (kind 5) per NIP-09 */
void gnostr_main_window_request_delete_note(GtkWidget *window, const char *id_hex, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[DELETE] Request deletion of id=%s pubkey=%.8s...",
            id_hex ? id_hex : "(null)",
            pubkey_hex ? pubkey_hex : "(null)");

  if (!id_hex || strlen(id_hex) != 64) {
    gnostr_main_window_show_toast_internal(self, "Invalid event ID for deletion");
    return;
  }

  /* Verify user is signed in and owns the note */
  if (!self->user_pubkey_hex || !*self->user_pubkey_hex) {
    gnostr_main_window_show_toast_internal(self, "Sign in to delete notes");
    return;
  }

  /* Security check: Only allow deletion of own notes */
  if (!pubkey_hex || strlen(pubkey_hex) != 64 ||
      g_ascii_strcasecmp(pubkey_hex, self->user_pubkey_hex) != 0) {
    gnostr_main_window_show_toast_internal(self, "Can only delete your own notes");
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    gnostr_main_window_show_toast_internal(self, "Signer not available");
    return;
  }

  gnostr_main_window_show_toast_internal(self, "Deleting note...");

  /* Build unsigned kind 5 deletion event JSON per NIP-09 using GNostrJsonBuilder */
  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, 5);  /* NOSTR_KIND_DELETION */
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, ""); /* Optional deletion reason */

  /* Build tags array per NIP-09:
   * - ["e", "<event-id-to-delete>"]
   * - ["k", "<kind-of-deleted-event>"] (kind 1 for text notes)
   */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* e-tag: ["e", "<event-id-to-delete>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "e");
  gnostr_json_builder_add_string(builder, id_hex);
  gnostr_json_builder_end_array(builder);

  /* k-tag: ["k", "1"] to indicate we're deleting a kind 1 note */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "k");
  gnostr_json_builder_add_string(builder, "1");
  gnostr_json_builder_end_array(builder);

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  /* Serialize */
  char *event_json = gnostr_json_builder_finish(builder);

  if (!event_json) {
    gnostr_main_window_show_toast_internal(self, "Failed to serialize deletion event");
    return;
  }

  g_debug("[DELETE] Unsigned deletion event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = g_object_ref(self);
  ctx->text = g_strdup(""); /* deletion has no text content */
  g_weak_ref_init(&ctx->composer_ref, NULL);

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_event_complete,
    ctx
  );
  g_free(event_json);
}

/* ================= NIP-56 Report Implementation ================= */

/* Public wrapper for reporting a note/user (kind 1984) per NIP-56 */
void gnostr_main_window_request_report_note(GtkWidget *window, const char *id_hex, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[NIP-56] Request report of id=%s pubkey=%.8s...",
            id_hex ? id_hex : "(null)",
            pubkey_hex ? pubkey_hex : "(null)");

  /* Verify user is signed in */
  if (!self->user_pubkey_hex || !*self->user_pubkey_hex) {
    gnostr_main_window_show_toast_internal(self, "Sign in to report content");
    return;
  }

  /* Validate pubkey */
  if (!pubkey_hex || strlen(pubkey_hex) != 64) {
    gnostr_main_window_show_toast_internal(self, "Invalid target for report");
    return;
  }

  /* Create and show report dialog */
  GnostrReportDialog *dialog = gnostr_report_dialog_new(GTK_WINDOW(self));
  gnostr_report_dialog_set_target(dialog, id_hex, pubkey_hex);

  /* Connect to signals for feedback */
  g_signal_connect_swapped(dialog, "report-sent", G_CALLBACK(gtk_window_destroy), dialog);

  gtk_window_present(GTK_WINDOW(dialog));
}

/* ================= NIP-32 Labeling Implementation ================= */

/* Public wrapper for adding a label to a note (kind 1985) per NIP-32 */
void gnostr_main_window_request_label_note(GtkWidget *window, const char *id_hex, const char *namespace, const char *label, const char *pubkey_hex) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  g_debug("[NIP-32] Request label of id=%s namespace=%s label=%s",
            id_hex ? id_hex : "(null)",
            namespace ? namespace : "(null)",
            label ? label : "(null)");

  /* Verify user is signed in */
  if (!self->user_pubkey_hex || !*self->user_pubkey_hex) {
    gnostr_main_window_show_toast_internal(self, "Sign in to add labels");
    return;
  }

  /* Validate inputs */
  if (!id_hex || strlen(id_hex) != 64) {
    gnostr_main_window_show_toast_internal(self, "Invalid event ID for labeling");
    return;
  }

  if (!namespace || !*namespace || !label || !*label) {
    gnostr_main_window_show_toast_internal(self, "Label and namespace are required");
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    gnostr_main_window_show_toast_internal(self, "Signer not available");
    return;
  }

  /* Build unsigned kind 1985 label event JSON per NIP-32 using GNostrJsonBuilder */
  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, 1985);  /* NOSTR_KIND_LABEL */
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, "");

  /* Build tags array per NIP-32:
   * - ["L", "<namespace>"]
   * - ["l", "<label>", "<namespace>"]
   * - ["e", "<event-id>"]
   * - ["p", "<event-author-pubkey>"]
   */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* L-tag: ["L", "<namespace>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "L");
  gnostr_json_builder_add_string(builder, namespace);
  gnostr_json_builder_end_array(builder);

  /* l-tag: ["l", "<label>", "<namespace>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "l");
  gnostr_json_builder_add_string(builder, label);
  gnostr_json_builder_add_string(builder, namespace);
  gnostr_json_builder_end_array(builder);

  /* e-tag: ["e", "<event-id>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "e");
  gnostr_json_builder_add_string(builder, id_hex);
  gnostr_json_builder_end_array(builder);

  /* p-tag: ["p", "<event-author-pubkey>"] */
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p");
    gnostr_json_builder_add_string(builder, pubkey_hex);
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  char *event_json = gnostr_json_builder_finish(builder);

  if (!event_json) {
    gnostr_main_window_show_toast_internal(self, "Failed to create label event");
    return;
  }

  g_debug("[NIP-32] Unsigned label event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = g_object_ref(self);
  ctx->text = g_strdup(""); /* label has no text content */
  g_weak_ref_init(&ctx->composer_ref, NULL);

  /* Call unified signer service */
  gnostr_sign_event_async(
    event_json,
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_event_complete,
    ctx
  );
  g_free(event_json);
}

/* ================= NIP-25 Like/Reaction Implementation ================= */

/* Callback when unified signer completes signing for like/reaction */
static void on_sign_like_event_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  LikeContext *ctx = (LikeContext*)user_data;
  (void)source;
  if (!ctx || !GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    like_context_free(ctx);
    return;
  }
  GnostrMainWindow *self = ctx->self;

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    g_autofree char *msg = g_strdup_printf("Like signing failed: %s", error ? error->message : "unknown error");
    gnostr_main_window_show_toast_internal(self, msg);
    g_clear_error(&error);
    like_context_free(ctx);
    return;
  }

  g_debug("[LIKE] Signed reaction event: %.100s...", signed_event_json);

  /* Parse the signed event JSON into a NostrEvent */
  NostrEvent *event = nostr_event_new();
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
  if (parse_rc != 1) {
    gnostr_main_window_show_toast_internal(self, "Failed to parse signed reaction event");
    nostr_event_free(event);
    g_free(signed_event_json);
    like_context_free(ctx);
    return;
  }

  /* Dispatch connect+publish to worker thread to avoid blocking the
   * main loop with synchronous relay connections. */
  RelayPublishResult *r = g_new0(RelayPublishResult, 1);
  r->event = event;                          /* transfer ownership */
  r->relay_urls = gnostr_get_write_relay_urls();
  r->signed_event_json = signed_event_json;  /* transfer ownership */

  GTask *task = g_task_new(NULL, NULL, on_like_publish_done, ctx);
  g_task_set_task_data(task, r, NULL);
  g_task_run_in_thread(task, relay_publish_thread);
  g_object_unref(task);
}

/* Main-thread callback after like publish worker completes */
static void on_like_publish_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  LikeContext *ctx = (LikeContext*)user_data;
  RelayPublishResult *r = g_task_propagate_pointer(G_TASK(res), NULL);
  if (!r) { like_context_free(ctx); return; }

  if (!ctx || !GNOSTR_IS_MAIN_WINDOW(ctx->self)) {
    relay_publish_result_free(r);
    like_context_free(ctx);
    return;
  }
  GnostrMainWindow *self = ctx->self;

  if (r->success_count > 0) {
    if (r->limit_skip_count > 0) {
      g_autofree char *msg = g_strdup_printf("Liked! (%u relays skipped)", r->limit_skip_count);
      gnostr_main_window_show_toast_internal(self, msg);
    } else {
      gnostr_main_window_show_toast_internal(self, "Liked!");
    }

    /* Mark event as liked in local cache */
    if (ctx->event_id_hex && self->liked_events) {
      g_hash_table_insert(self->liked_events, g_strdup(ctx->event_id_hex), GINT_TO_POINTER(1));
    }

    /* Update note card row to show liked state */
    g_autoptr(GObject) row_obj = g_weak_ref_get(&ctx->row_ref);
    if (row_obj && NOSTR_GTK_IS_NOTE_CARD_ROW(row_obj)) {
      NostrGtkNoteCardRow *row = NOSTR_GTK_NOTE_CARD_ROW(row_obj);
      const char *row_event_id = nostr_gtk_note_card_row_get_event_id(row);
      if (!nostr_gtk_note_card_row_is_disposed(row) &&
          gtk_widget_get_root(GTK_WIDGET(row)) != NULL &&
          g_strcmp0(row_event_id, ctx->event_id_hex) == 0) {
        nostr_gtk_note_card_row_set_liked(row, TRUE);
      }
    }

    /* Store reaction in local NostrdB cache (background) */
    if (r->signed_event_json) {
      GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
      g_ptr_array_add(b, g_strdup(r->signed_event_json));
      storage_ndb_ingest_events_async(b);
    }
  } else {
    gnostr_main_window_show_toast_internal(self, "Failed to publish reaction");
  }

  relay_publish_result_free(r);
  like_context_free(ctx);
}

/* Public function to request a like/reaction (kind 7) - NIP-25
 * @event_kind: the kind of the event being reacted to (for k-tag)
 * @reaction_content: the reaction content ("+" for like, "-" for dislike, or emoji) */
void gnostr_main_window_request_like(GtkWidget *window, const char *id_hex, const char *pubkey_hex, gint event_kind, const char *reaction_content, NostrGtkNoteCardRow *row) {
  if (!window || !GTK_IS_APPLICATION_WINDOW(window)) return;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Default reaction content to "+" if not specified */
  if (!reaction_content || !*reaction_content) {
    reaction_content = "+";
  }

  g_debug("[LIKE] Request reaction '%s' on id=%s pubkey=%.8s... kind=%d",
            reaction_content,
            id_hex ? id_hex : "(null)",
            pubkey_hex ? pubkey_hex : "(null)",
            event_kind);

  if (!id_hex || strlen(id_hex) != 64) {
    gnostr_main_window_show_toast_internal(self, "Invalid event ID for reaction");
    return;
  }

  /* Check if already liked (only for "+" reactions) */
  if (strcmp(reaction_content, "+") == 0 && self->liked_events && g_hash_table_contains(self->liked_events, id_hex)) {
    gnostr_main_window_show_toast_internal(self, "Already liked!");
    return;
  }

  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    gnostr_main_window_show_toast_internal(self, "Signer not available");
    return;
  }

  /* Show appropriate toast based on reaction type */
  if (strcmp(reaction_content, "+") == 0) {
    gnostr_main_window_show_toast_internal(self, "Liking...");
  } else if (strcmp(reaction_content, "-") == 0) {
    gnostr_main_window_show_toast_internal(self, "Reacting...");
  } else {
    g_autofree char *msg = g_strdup_printf("Reacting with %s...", reaction_content);
    gnostr_main_window_show_toast_internal(self, msg);
  }

  /* Build unsigned kind 7 reaction event JSON (NIP-25) using GNostrJsonBuilder */
  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, NOSTR_KIND_REACTION);
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, reaction_content);

  /* Build tags array: e-tag, p-tag, and k-tag per NIP-25 */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* e-tag: ["e", "<event-id-being-reacted-to>"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "e");
  gnostr_json_builder_add_string(builder, id_hex);
  gnostr_json_builder_end_array(builder);

  /* p-tag: ["p", "<pubkey-of-event-author>"] */
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p");
    gnostr_json_builder_add_string(builder, pubkey_hex);
    gnostr_json_builder_end_array(builder);
  }

  /* k-tag: ["k", "<kind-of-reacted-event>"] per NIP-25 */
  char kind_str[16];
  snprintf(kind_str, sizeof(kind_str), "%d", event_kind > 0 ? event_kind : 1);
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "k");
  gnostr_json_builder_add_string(builder, kind_str);
  gnostr_json_builder_end_array(builder);

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  /* Serialize */
  char *event_json = gnostr_json_builder_finish(builder);

  if (!event_json) {
    gnostr_main_window_show_toast_internal(self, "Failed to serialize reaction event");
    return;
  }

  g_debug("[LIKE] Unsigned reaction event: %s", event_json);

  /* Create async context */
  LikeContext *ctx = g_new0(LikeContext, 1);
  ctx->self = g_object_ref(self);
  ctx->event_id_hex = g_strdup(id_hex);
  g_weak_ref_init(&ctx->row_ref, row ? G_OBJECT(row) : NULL);

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_like_event_complete,
    ctx
  );
  g_free(event_json);
}

void gnostr_main_window_handle_composer_post_requested(NostrGtkComposer *composer, const char *text, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) return;

  /* Validate input */
  if (!text || !*text) {
    gnostr_main_window_show_toast_internal(self, "Cannot post empty note");
    return;
  }

  /* Check if signer is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    gnostr_main_window_show_toast_internal(self, "Signer not available - please sign in");
    return;
  }

  gnostr_main_window_show_toast_internal(self, "Signing...");

  /* Build unsigned event JSON using GNostrJsonBuilder */
  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  /* NIP-22: Check if this is a comment (kind 1111) - takes precedence over reply/quote */
  gboolean is_comment = (composer && NOSTR_GTK_IS_COMPOSER(composer) &&
                         nostr_gtk_composer_is_comment(NOSTR_GTK_COMPOSER(composer)));

  if (is_comment) {
    const char *comment_root_id = nostr_gtk_composer_get_comment_root_id(NOSTR_GTK_COMPOSER(composer));
    int comment_root_kind = nostr_gtk_composer_get_comment_root_kind(NOSTR_GTK_COMPOSER(composer));
    const char *comment_root_pubkey = nostr_gtk_composer_get_comment_root_pubkey(NOSTR_GTK_COMPOSER(composer));

    g_debug("[PUBLISH] Building NIP-22 comment event: root_id=%s root_kind=%d pubkey=%.8s...",
            comment_root_id ? comment_root_id : "(null)",
            comment_root_kind,
            comment_root_pubkey ? comment_root_pubkey : "(null)");

    /* Set kind 1111 for comment */
    gnostr_json_builder_set_key(builder, "kind");
    gnostr_json_builder_add_int(builder, 1111);

    gnostr_json_builder_set_key(builder, "created_at");
    gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
    gnostr_json_builder_set_key(builder, "content");
    gnostr_json_builder_add_string(builder, text);

    /* Build tags array */
    gnostr_json_builder_set_key(builder, "tags");
    gnostr_json_builder_begin_array(builder);

    /* NIP-22 requires these tags:
     * - ["K", "<root-kind>"] - kind of the root event
     * - ["E", "<root-id>", "<relay>", "<pubkey>"] - root event reference
     * - ["P", "<root-pubkey>"] - root event author
     */

    /* K tag: root event kind */
    char kind_str[16];
    g_snprintf(kind_str, sizeof(kind_str), "%d", comment_root_kind);
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "K");
    gnostr_json_builder_add_string(builder, kind_str);
    gnostr_json_builder_end_array(builder);

    /* E tag: root event reference */
    if (comment_root_id && strlen(comment_root_id) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "E");
      gnostr_json_builder_add_string(builder, comment_root_id);
      gnostr_json_builder_add_string(builder, ""); /* relay hint */
      if (comment_root_pubkey && strlen(comment_root_pubkey) == 64) {
        gnostr_json_builder_add_string(builder, comment_root_pubkey); /* author hint */
      }
      gnostr_json_builder_end_array(builder);
    }

    /* P tag: root event author */
    if (comment_root_pubkey && strlen(comment_root_pubkey) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "P");
      gnostr_json_builder_add_string(builder, comment_root_pubkey);
      gnostr_json_builder_end_array(builder);
    }

    gnostr_json_builder_end_array(builder); /* end tags */
    gnostr_json_builder_end_object(builder);

    char *event_json = gnostr_json_builder_finish(builder);

    if (!event_json) {
      gnostr_main_window_show_toast_internal(self, "Failed to build event JSON");
      return;
    }

    g_debug("[PUBLISH] Unsigned NIP-22 comment event: %s", event_json);

    /* Create async context and sign */
    PublishContext *ctx = g_new0(PublishContext, 1);
    ctx->self = g_object_ref(self);
    ctx->text = g_strdup(text);
    g_weak_ref_init(&ctx->composer_ref, composer ? G_OBJECT(composer) : NULL);

    gnostr_sign_event_async(
      event_json,
      "",              /* current_user: ignored */
      "gnostr",        /* app_id: ignored */
      NULL,            /* cancellable */
      on_sign_event_complete,
      ctx
    );
    g_free(event_json);
    return;
  }

  /* Regular kind 1 text note */
  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, 1);
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int(builder, (int64_t)time(NULL));
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, text);

  /* Build tags array */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* Check if this is a reply - add NIP-10 threading tags (only for kind 1, not comments) */
  if (!is_comment && composer && NOSTR_GTK_IS_COMPOSER(composer) && nostr_gtk_composer_is_reply(NOSTR_GTK_COMPOSER(composer))) {
    const char *reply_to_id = nostr_gtk_composer_get_reply_to_id(NOSTR_GTK_COMPOSER(composer));
    const char *root_id = nostr_gtk_composer_get_root_id(NOSTR_GTK_COMPOSER(composer));
    const char *reply_to_pubkey = nostr_gtk_composer_get_reply_to_pubkey(NOSTR_GTK_COMPOSER(composer));

    g_debug("[PUBLISH] Building reply event: reply_to=%s root=%s pubkey=%.8s...",
            reply_to_id ? reply_to_id : "(null)",
            root_id ? root_id : "(null)",
            reply_to_pubkey ? reply_to_pubkey : "(null)");

    /* NIP-10 recommends using positional markers for replies:
     * - ["e", "<root-id>", "", "root"] for the thread root
     * - ["e", "<reply-id>", "", "reply"] for the direct parent
     * - ["p", "<pubkey>"] for all mentioned pubkeys
     */

    /* Add root e-tag (always present for replies) */
    if (root_id && strlen(root_id) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "e");
      gnostr_json_builder_add_string(builder, root_id);
      gnostr_json_builder_add_string(builder, ""); /* relay hint */
      gnostr_json_builder_add_string(builder, "root");
      gnostr_json_builder_end_array(builder);
    }

    /* Add reply e-tag if different from root (nested reply) */
    if (reply_to_id && strlen(reply_to_id) == 64 &&
        (!root_id || strcmp(reply_to_id, root_id) != 0)) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "e");
      gnostr_json_builder_add_string(builder, reply_to_id);
      gnostr_json_builder_add_string(builder, ""); /* relay hint */
      gnostr_json_builder_add_string(builder, "reply");
      gnostr_json_builder_end_array(builder);
    }

    /* Add p-tag for the author being replied to */
    if (reply_to_pubkey && strlen(reply_to_pubkey) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "p");
      gnostr_json_builder_add_string(builder, reply_to_pubkey);
      gnostr_json_builder_end_array(builder);
    }
  }

  /* Check if this is a quote post - add q-tag and p-tag per NIP-18 (only for kind 1, not comments) */
  if (!is_comment && composer && NOSTR_GTK_IS_COMPOSER(composer) && nostr_gtk_composer_is_quote(NOSTR_GTK_COMPOSER(composer))) {
    const char *quote_id = nostr_gtk_composer_get_quote_id(NOSTR_GTK_COMPOSER(composer));
    const char *quote_pubkey = nostr_gtk_composer_get_quote_pubkey(NOSTR_GTK_COMPOSER(composer));

    g_debug("[PUBLISH] Building quote post: quote_id=%s pubkey=%.8s...",
            quote_id ? quote_id : "(null)",
            quote_pubkey ? quote_pubkey : "(null)");

    /* q-tag: ["q", "<quoted-event-id>", "<relay-hint>"] */
    if (quote_id && strlen(quote_id) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "q");
      gnostr_json_builder_add_string(builder, quote_id);
      gnostr_json_builder_add_string(builder, ""); /* relay hint */
      gnostr_json_builder_end_array(builder);
    }

    /* p-tag: ["p", "<quoted-author-pubkey>"] */
    if (quote_pubkey && strlen(quote_pubkey) == 64) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "p");
      gnostr_json_builder_add_string(builder, quote_pubkey);
      gnostr_json_builder_end_array(builder);
    }
  }

  /* NIP-14: Add subject tag if present */
  if (composer && NOSTR_GTK_IS_COMPOSER(composer)) {
    const char *subject = nostr_gtk_composer_get_subject(NOSTR_GTK_COMPOSER(composer));
    if (subject && *subject) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "subject");
      gnostr_json_builder_add_string(builder, subject);
      gnostr_json_builder_end_array(builder);
      g_debug("[PUBLISH] Added subject tag: %s", subject);
    }
  }

  /* NIP-92: Add imeta tags for uploaded media */
  if (composer && NOSTR_GTK_IS_COMPOSER(composer)) {
    gsize media_count = nostr_gtk_composer_get_uploaded_media_count(NOSTR_GTK_COMPOSER(composer));
    if (media_count > 0) {
      NostrGtkComposerMedia **media_list = nostr_gtk_composer_get_uploaded_media(NOSTR_GTK_COMPOSER(composer));
      for (gsize i = 0; i < media_count && media_list && media_list[i]; i++) {
        NostrGtkComposerMedia *m = media_list[i];
        if (!m->url) continue;

        /* Build imeta tag: ["imeta", "url <url>", "m <mime>", "x <sha256>", "size <bytes>"] */
        gnostr_json_builder_begin_array(builder);
        gnostr_json_builder_add_string(builder, "imeta");

        /* url field (required) */
        g_autofree char *url_field = g_strdup_printf("url %s", m->url);
        gnostr_json_builder_add_string(builder, url_field);

        /* m field (MIME type) */
        if (m->mime_type && *m->mime_type) {
          g_autofree char *mime_field = g_strdup_printf("m %s", m->mime_type);
          gnostr_json_builder_add_string(builder, mime_field);
        }

        /* x field (SHA-256 hash) */
        if (m->sha256 && *m->sha256) {
          g_autofree char *hash_field = g_strdup_printf("x %s", m->sha256);
          gnostr_json_builder_add_string(builder, hash_field);
        }

        /* size field */
        if (m->size > 0) {
          g_autofree char *size_field = g_strdup_printf("size %" G_GINT64_FORMAT, m->size);
          gnostr_json_builder_add_string(builder, size_field);
        }

        gnostr_json_builder_end_array(builder);
        g_debug("[PUBLISH] Added imeta tag for: %s (type=%s, sha256=%.16s...)",
                m->url, m->mime_type ? m->mime_type : "?",
                m->sha256 ? m->sha256 : "?");
      }
    }
  }

  /* NIP-40: Add expiration tag if set */
  if (composer && NOSTR_GTK_IS_COMPOSER(composer)) {
    gint64 expiration = nostr_gtk_composer_get_expiration(NOSTR_GTK_COMPOSER(composer));
    if (expiration > 0) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "expiration");
      char expiration_str[32];
      g_snprintf(expiration_str, sizeof(expiration_str), "%" G_GINT64_FORMAT, expiration);
      gnostr_json_builder_add_string(builder, expiration_str);
      gnostr_json_builder_end_array(builder);
      g_debug("[PUBLISH] Added expiration tag: %s", expiration_str);
    }
  }

  /* NIP-36: Add content-warning tag if note is marked as sensitive */
  if (composer && NOSTR_GTK_IS_COMPOSER(composer)) {
    if (nostr_gtk_composer_is_sensitive(NOSTR_GTK_COMPOSER(composer))) {
      gnostr_json_builder_begin_array(builder);
      gnostr_json_builder_add_string(builder, "content-warning");
      /* Empty reason - users can customize in future */
      gnostr_json_builder_add_string(builder, "");
      gnostr_json_builder_end_array(builder);
      g_debug("[PUBLISH] Added content-warning tag (sensitive content)");
    }
  }

  gnostr_json_builder_end_array(builder); /* end tags */
  gnostr_json_builder_end_object(builder);

  char *event_json = gnostr_json_builder_finish(builder);

  if (!event_json) {
    gnostr_main_window_show_toast_internal(self, "Failed to build event JSON");
    return;
  }

  g_debug("[PUBLISH] Unsigned event: %s", event_json);

  /* Create async context */
  PublishContext *ctx = g_new0(PublishContext, 1);
  ctx->self = g_object_ref(self);
  ctx->text = g_strdup(text);
  g_weak_ref_init(&ctx->composer_ref, composer ? G_OBJECT(composer) : NULL);

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,      /* event_json */
    "",              /* current_user: ignored */
    "gnostr",        /* app_id: ignored */
    NULL,            /* cancellable */
    on_sign_event_complete,
    ctx
  );

  g_free(event_json);
}
