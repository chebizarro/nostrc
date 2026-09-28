#include "gh-nip17-envelope.h"
#include "gh-identity.h"
#include "gh-signer.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip17/nip17.h"
#include "nostr/nip59/nip59.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  guint64 generation;
  gchar *sender;
  gchar *sender_npub;
  gchar *recipient;
  gchar *rumor_json;
  gchar *ciphertext;
  gchar *recipient_wrap_json;
  gchar *sender_wrap_json;
  gchar *recipient_outer_pubkey;
  int64_t seal_at;
  guint destination; /* 0: recipient, 1: sender self-copy */
} Build;

static void
build_free(gpointer data)
{
  Build *build = data;
  g_free(build->sender);
  g_free(build->sender_npub);
  g_free(build->recipient);
  g_free(build->rumor_json);
  g_free(build->ciphertext);
  g_free(build->recipient_wrap_json);
  g_free(build->sender_wrap_json);
  g_free(build->recipient_outer_pubkey);
  g_free(build);
}

void
gh_nip17_envelope_free(GhNip17Envelope *envelope)
{
  if (!envelope) return;
  g_free(envelope->rumor_json);
  g_free(envelope->recipient_wrap_json);
  g_free(envelope->sender_wrap_json);
  g_free(envelope);
}

static gboolean
hex64(const gchar *value)
{
  if (!value || strlen(value) != 64) return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isxdigit(*p)) return FALSE;
  return TRUE;
}

static const gchar *
destination(Build *build)
{
  return build->destination == 0 ? build->recipient : build->sender;
}

static gboolean
current(GTask *task)
{
  GhAccountController *accounts = g_task_get_source_object(task);
  Build *build = g_task_get_task_data(task);
  GCancellable *cancel = g_task_get_cancellable(task);
  return (!cancel || !g_cancellable_is_cancelled(cancel)) &&
         gh_account_controller_is_current(accounts, build->generation) &&
         g_strcmp0(gh_account_controller_get_active_npub(accounts),
                   build->sender_npub) == 0;
}

static void
fail(GTask *task, GError *error)
{
  if (!current(task)) {
    g_clear_error(&error);
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "NIP-17 account generation or request was cancelled");
  }
  g_task_return_error(task, error);
  g_object_unref(task);
}

static void
fail_local(GTask *task, const gchar *message)
{
  fail(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, message));
}

static void encrypt_done(GObject *source, GAsyncResult *result, gpointer data);
static void sign_done(GObject *source, GAsyncResult *result, gpointer data);

static void
begin_encrypt(GTask *task)
{
  GhAccountController *accounts = g_task_get_source_object(task);
  Build *build = g_task_get_task_data(task);
  if (!current(task)) {
    fail_local(task, "NIP-17 account changed");
    return;
  }
  gh_account_controller_nip44_encrypt_with_cancellable_async(
    accounts, build->rumor_json, destination(build), g_task_get_cancellable(task),
    encrypt_done, task);
}

static void
encrypt_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Build *build = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  gchar *ciphertext = gh_account_controller_nip44_finish(result, &error);
  (void)source;
  if (error) { fail(task, g_steal_pointer(&error)); return; }
  if (!current(task)) { g_free(ciphertext); fail_local(task, "NIP-17 account changed"); return; }
  if (!ciphertext || nostr_nip59_randomize_timestamp(0, 0, &build->seal_at) != NIP59_OK) {
    g_free(ciphertext);
    fail_local(task, "Could not create private NIP-17 seal");
    return;
  }
  g_free(build->ciphertext);
  build->ciphertext = ciphertext;

  NostrEvent *seal = nostr_event_new();
  if (!seal) { fail_local(task, "Could not allocate NIP-17 seal"); return; }
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, build->sender);
  nostr_event_set_content(seal, build->ciphertext);
  nostr_event_set_created_at(seal, build->seal_at);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  char *unsigned_json = nostr_event_serialize_compact(seal);
  nostr_event_free(seal);
  if (!unsigned_json) { fail_local(task, "Could not serialize NIP-17 seal"); return; }
  if (!current(task)) {
    free(unsigned_json);
    fail_local(task, "NIP-17 account changed");
    return;
  }
  GhAccountController *accounts = g_task_get_source_object(task);
  gh_account_controller_sign_with_cancellable_async(
    accounts, unsigned_json, g_task_get_cancellable(task), sign_done, task);
  free(unsigned_json);
}

static void
sign_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Build *build = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  (void)source;
  if (error) { fail(task, g_steal_pointer(&error)); return; }
  if (!current(task)) { fail_local(task, "NIP-17 account changed"); return; }

  NostrEvent *seal = nostr_event_new();
  if (!seal || nostr_event_deserialize_compact(seal, signed_json, NULL) != 1 ||
      nostr_event_validate(seal, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_get_kind(seal) != 13 ||
      g_strcmp0(nostr_event_get_pubkey(seal), build->sender) != 0 ||
      g_strcmp0(nostr_event_get_content(seal), build->ciphertext) != 0 ||
      nostr_event_get_created_at(seal) != build->seal_at) {
    if (seal) nostr_event_free(seal);
    fail(task, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_RESULT,
                                   "Signer returned an invalid NIP-17 seal"));
    return;
  }
  NostrEvent *wrap = nostr_nip59_wrap(seal, destination(build), NULL);
  nostr_event_free(seal);
  char *wrapped_for = wrap ? nostr_nip59_get_recipient(wrap) : NULL;
  gboolean correct_recipient = g_strcmp0(wrapped_for, destination(build)) == 0;
  free(wrapped_for);
  if (!wrap || !nostr_nip59_validate_gift_wrap(wrap) || !correct_recipient ||
      g_strcmp0(nostr_event_get_pubkey(wrap), build->sender) == 0 ||
      (build->destination == 1 &&
       g_strcmp0(nostr_event_get_pubkey(wrap), build->recipient_outer_pubkey) == 0)) {
    if (wrap) nostr_event_free(wrap);
    fail_local(task, "Could not create private NIP-17 gift wrap");
    return;
  }
  if (build->destination == 0)
    build->recipient_outer_pubkey = g_strdup(nostr_event_get_pubkey(wrap));
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
  if (!json || !current(task)) {
    free(json);
    fail_local(task, "NIP-17 gift wrap was cancelled or could not serialize");
    return;
  }
  if (build->destination == 0) {
    build->recipient_wrap_json = g_strdup(json);
    free(json);
    build->destination = 1;
    begin_encrypt(task);
    return;
  }
  build->sender_wrap_json = g_strdup(json);
  free(json);
  GhNip17Envelope *envelope = g_new0(GhNip17Envelope, 1);
  envelope->rumor_json = g_steal_pointer(&build->rumor_json);
  envelope->recipient_wrap_json = g_steal_pointer(&build->recipient_wrap_json);
  envelope->sender_wrap_json = g_steal_pointer(&build->sender_wrap_json);
  g_task_return_pointer(task, envelope, (GDestroyNotify)gh_nip17_envelope_free);
  g_object_unref(task);
}

static void
build_start(GhAccountController *accounts, const gchar *recipient_pubkey_hex,
            gboolean self_only, const gchar *content, GCancellable *cancellable,
            GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(accounts, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_nip17_envelope_build_async);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  const gchar *npub = gh_account_controller_get_active_npub(accounts);
  g_autofree gchar *sender = npub ? gh_identity_pubkey_hex(npub) : NULL;
  if (self_only && sender)
    recipient_pubkey_hex = sender;
  if (!sender || !hex64(recipient_pubkey_hex) || !content || !*content ||
      !g_utf8_validate(content, -1, NULL)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Active account, recipient pubkey and UTF-8 content are required");
    g_object_unref(task);
    return;
  }
  g_autofree gchar *recipient = g_ascii_strdown(recipient_pubkey_hex, -1);
  if (!self_only && g_strcmp0(sender, recipient) == 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Recipient must differ from the selected sender");
    g_object_unref(task);
    return;
  }
  Build *build = g_new0(Build, 1);
  build->generation = gh_account_controller_get_generation(accounts);
  build->sender = g_strdup(sender);
  build->sender_npub = g_strdup(npub);
  build->recipient = g_strdup(recipient);
  /* A note to self skips straight to the single self wrap. */
  build->destination = self_only ? 1 : 0;
  g_task_set_task_data(task, build, build_free);
  NostrEvent *rumor = nostr_nip17_create_rumor(sender, recipient, content, 0);
  if (!rumor) { fail_local(task, "Could not create NIP-17 rumor"); return; }
  rumor->id = nostr_event_get_id(rumor);
  if (!rumor->id || nostr_event_validate_id(rumor, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      rumor->sig != NULL) {
    nostr_event_free(rumor);
    fail_local(task, "Could not canonicalize NIP-17 rumor");
    return;
  }
  char *json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);
  if (!json) { fail_local(task, "Could not serialize NIP-17 rumor"); return; }
  build->rumor_json = g_strdup(json);
  free(json);
  begin_encrypt(task);
}

void
gh_nip17_envelope_build_async(GhAccountController *accounts,
                                    const gchar *recipient_pubkey_hex,
                                    const gchar *content,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  build_start(accounts, recipient_pubkey_hex, FALSE, content, cancellable,
              callback, user_data);
}

void
gh_nip17_envelope_build_self_async(GhAccountController *accounts,
                                   const gchar *content,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  build_start(accounts, NULL, TRUE, content, cancellable, callback, user_data);
}

GhNip17Envelope *
gh_nip17_envelope_build_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) ==
                       gh_nip17_envelope_build_async, NULL);
  GhNip17Envelope *envelope = g_task_propagate_pointer(G_TASK(result), error);
  if (envelope && !current(G_TASK(result))) {
    gh_nip17_envelope_free(envelope);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "NIP-17 account generation or request was cancelled");
    return NULL;
  }
  return envelope;
}
