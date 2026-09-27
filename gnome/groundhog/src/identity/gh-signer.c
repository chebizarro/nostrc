#include "gh-signer.h"
#include "gh-identity.h"
#include "nostr-event.h"

#include <string.h>

#define SIGNER_BUS "org.nostr.Signer"
#define SIGNER_PATH "/org/nostr/signer"
#define SIGNER_INTERFACE "org.nostr.Signer"
#define MAX_RESULT (1024 * 1024)

typedef enum { OP_SIGN, OP_ENCRYPT, OP_DECRYPT } Operation;
typedef struct _Pending Pending;
struct _GhSigner {
  gint refs;
  GDBusConnection *bus;
  gchar *npub;
  gchar *pubkey;
  GPtrArray *pending; /* non-owning; each Pending keeps signer alive */
  guint generation;
};
struct _Pending {
  GhSigner *signer;
  GTask *task;
  GCancellable *cancel;
  GCancellable *caller;
  gulong caller_handler;
  guint generation;
  Operation op;
  NostrEvent *request;
};

G_DEFINE_QUARK(gh-signer-error-quark, gh_signer_error)

static GhSigner *
signer_ref(GhSigner *signer)
{
  g_atomic_int_inc(&signer->refs);
  return signer;
}

static void
signer_unref(GhSigner *signer)
{
  if (!g_atomic_int_dec_and_test(&signer->refs)) return;
  g_assert(signer->pending->len == 0);
  g_ptr_array_unref(signer->pending);
  g_clear_object(&signer->bus);
  g_free(signer->npub);
  g_free(signer->pubkey);
  g_free(signer);
}

GhSigner *
gh_signer_new(GDBusConnection *bus, const gchar *selected_npub, GError **error)
{
  g_return_val_if_fail(G_IS_DBUS_CONNECTION(bus), NULL);
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(selected_npub);
  if (!pubkey) {
    g_set_error_literal(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                        "A valid selected npub is required");
    return NULL;
  }
  GhSigner *signer = g_new0(GhSigner, 1);
  signer->refs = 1;
  signer->bus = g_object_ref(bus);
  signer->npub = g_strdup(selected_npub);
  signer->pubkey = g_steal_pointer(&pubkey);
  signer->pending = g_ptr_array_new();
  return signer;
}

void
gh_signer_free(GhSigner *signer)
{
  if (!signer) return;
  for (guint i = 0; i < signer->pending->len; i++) {
    Pending *p = g_ptr_array_index(signer->pending, i);
    g_cancellable_cancel(p->cancel);
  }
  signer_unref(signer);
}

gboolean
gh_signer_select(GhSigner *signer, const gchar *npub, GError **error)
{
  g_return_val_if_fail(signer != NULL, FALSE);
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(npub);
  if (!pubkey) {
    g_set_error_literal(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                        "A valid selected npub is required");
    return FALSE;
  }
  signer->generation++;
  for (guint i = 0; i < signer->pending->len; i++) {
    Pending *p = g_ptr_array_index(signer->pending, i);
    g_cancellable_cancel(p->cancel);
  }
  g_free(signer->npub);
  g_free(signer->pubkey);
  signer->npub = g_strdup(npub);
  signer->pubkey = g_steal_pointer(&pubkey);
  return TRUE;
}

static gboolean
hex64(const gchar *s)
{
  if (!s || strlen(s) != 64) return FALSE;
  for (const gchar *p = s; *p; p++) if (!g_ascii_isxdigit(*p)) return FALSE;
  return TRUE;
}

static gboolean
valid_ciphertext(const gchar *text)
{
  if (!text || !*text || strlen(text) > MAX_RESULT) return FALSE;
  gsize len = 0;
  g_autofree guchar *raw = g_base64_decode(text, &len);
  if (len < 99 || raw[0] != 2) return FALSE;
  g_autofree gchar *canonical = g_base64_encode(raw, len);
  return g_strcmp0(text, canonical) == 0;
}

static gboolean
tags_match(const NostrTags *a, const NostrTags *b)
{
  size_t count = a ? nostr_tags_size(a) : 0;
  if (count != (b ? nostr_tags_size(b) : 0)) return FALSE;
  for (size_t i = 0; i < count; i++) {
    NostrTag *ta = nostr_tags_get(a, i);
    NostrTag *tb = nostr_tags_get(b, i);
    size_t n = nostr_tag_size(ta);
    if (n != nostr_tag_size(tb)) return FALSE;
    for (size_t j = 0; j < n; j++)
      if (g_strcmp0(nostr_tag_get(ta, j), nostr_tag_get(tb, j)) != 0) return FALSE;
  }
  return TRUE;
}

static GhSignerError
signed_event_error(Pending *p, const gchar *json)
{
  if (!json || strlen(json) > MAX_RESULT) return GH_SIGNER_ERROR_INVALID_RESULT;
  NostrEvent *signed_event = nostr_event_new();
  if (!signed_event) return GH_SIGNER_ERROR_INVALID_RESULT;
  GhSignerError result = GH_SIGNER_ERROR_INVALID_RESULT;
  if (nostr_event_deserialize_compact(signed_event, json, NULL) == 1 &&
      nostr_event_validate(signed_event, NULL) == NOSTR_EVENT_VALIDATION_OK) {
    if (g_strcmp0(signed_event->pubkey, p->request->pubkey) != 0)
      result = GH_SIGNER_ERROR_KEY_MISMATCH;
    else if (signed_event->created_at == p->request->created_at &&
             signed_event->kind == p->request->kind &&
             g_strcmp0(signed_event->content, p->request->content) == 0 &&
             tags_match(signed_event->tags, p->request->tags))
      result = 0;
  }
  nostr_event_free(signed_event);
  return result;
}

static void
cancel_from_caller(GCancellable *caller, gpointer data)
{
  (void)caller;
  g_cancellable_cancel(G_CANCELLABLE(data));
}

static void
pending_free(Pending *p)
{
  if (p->caller_handler) g_cancellable_disconnect(p->caller, p->caller_handler);
  g_clear_object(&p->caller);
  g_clear_object(&p->cancel);
  if (p->request) nostr_event_free(p->request);
  g_object_unref(p->task);
  signer_unref(p->signer);
  g_free(p);
}

static GError *
map_bus_error(GError *error)
{
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                               "Signer operation cancelled");
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
      g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_TIMEOUT) ||
      g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY))
    return g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_TIMED_OUT,
                               "Signer operation timed out");
  g_autofree gchar *remote = g_dbus_error_get_remote_error(error);
  if (g_strcmp0(remote, "org.nostr.Signer.Error.ApprovalDenied") == 0)
    return g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED,
                               "Signer approval denied");
  if (g_strcmp0(remote, "org.nostr.Signer.Error.ApprovalTimedOut") == 0)
    return g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_TIMED_OUT,
                               "Signer approval timed out");
  if (g_strcmp0(remote, "org.nostr.Signer.Error.NoApprovalAgent") == 0)
    return g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_NO_APPROVER,
                               "Signer approval agent is not running");
  if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
      g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
      g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
    return g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE,
                               "Required signer service or method is unavailable");
  return g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE,
                             "Signer service failed");
}

static void
call_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Pending *p = data;
  GhSigner *signer = p->signer;
  g_autoptr(GError) bus_error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                                              &bus_error);
  g_ptr_array_remove(signer->pending, p);
  if (g_cancellable_is_cancelled(p->cancel) || p->generation != signer->generation) {
    g_task_return_new_error(p->task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                            "Signer operation cancelled");
  } else if (bus_error) {
    g_task_return_error(p->task, map_bus_error(bus_error));
  } else if (!reply || !g_variant_is_of_type(reply, G_VARIANT_TYPE("(s)"))) {
    g_task_return_new_error(p->task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_RESULT,
                            "Signer returned an invalid result");
  } else {
    const gchar *value;
    g_variant_get(reply, "(&s)", &value);
    GhSignerError validation = p->op == OP_SIGN ? signed_event_error(p, value) :
      p->op == OP_ENCRYPT ? (valid_ciphertext(value) ? 0 : GH_SIGNER_ERROR_INVALID_RESULT) :
      (value && strlen(value) <= MAX_RESULT && g_utf8_validate(value, -1, NULL)
        ? 0 : GH_SIGNER_ERROR_INVALID_RESULT);
    if (validation)
      g_task_return_new_error(p->task, GH_SIGNER_ERROR, validation,
                              "Signer returned an unverified operation result");
    else
      g_task_return_pointer(p->task, g_strdup(value), g_free);
  }
  pending_free(p);
}

static void
start_call(GhSigner *signer, Operation op, const gchar *input, const gchar *peer,
           GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  if (op == OP_SIGN)
    g_task_set_source_tag(task, gh_signer_sign_async);
  else if (op == OP_ENCRYPT)
    g_task_set_source_tag(task, gh_signer_nip44_encrypt_async);
  else
    g_task_set_source_tag(task, gh_signer_nip44_decrypt_async);
  if (!input || strlen(input) > MAX_RESULT || !g_utf8_validate(input, -1, NULL) ||
      (op != OP_SIGN && !hex64(peer))) {
    g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                            "Invalid signer input");
    g_object_unref(task);
    return;
  }
  NostrEvent *request = NULL;
  if (op == OP_SIGN) {
    request = nostr_event_new();
    if (!request || nostr_event_deserialize_compact(request, input, NULL) != 1 ||
        request->created_at <= 0 || request->kind < 0 ||
        g_strcmp0(request->pubkey, signer->pubkey) != 0 ||
        request->id || request->sig || !request->content) {
      if (request) nostr_event_free(request);
      g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                              "Unsigned event does not match the selected identity");
      g_object_unref(task);
      return;
    }
  } else if (op == OP_DECRYPT && !valid_ciphertext(input)) {
    g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                            "Invalid NIP-44 ciphertext");
    g_object_unref(task);
    return;
  }
  Pending *p = g_new0(Pending, 1);
  p->signer = signer_ref(signer);
  p->task = task;
  p->cancel = g_cancellable_new();
  p->caller = cancellable ? g_object_ref(cancellable) : NULL;
  if (p->caller)
    p->caller_handler = g_cancellable_connect(p->caller, G_CALLBACK(cancel_from_caller),
                                               p->cancel, NULL);
  p->generation = signer->generation;
  p->op = op;
  p->request = request;
  g_ptr_array_add(signer->pending, p);
  /* The opt-in is per bus connection and lost if the service restarts, so it
   * precedes every gated call; the service handles one connection's calls in
   * order. No reply is requested, so a pre-0.5.0 service's UnknownMethod
   * error is never sent; its failures then all arrive as ApprovalDenied. */
  g_dbus_connection_call(signer->bus, SIGNER_BUS, SIGNER_PATH, SIGNER_INTERFACE,
                         "EnableTypedApprovalErrors", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                         -1, NULL, NULL, NULL);
  g_dbus_connection_call(signer->bus, SIGNER_BUS, SIGNER_PATH, SIGNER_INTERFACE,
                         op == OP_SIGN ? "SignEvent" :
                         op == OP_ENCRYPT ? "NIP44Encrypt" : "NIP44Decrypt",
                         op == OP_SIGN ? g_variant_new("(sss)", input, signer->npub, "") :
                                         g_variant_new("(sss)", input, peer, signer->npub),
                         G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 30000,
                         p->cancel, call_done, p);
}

void
gh_signer_sign_async(GhSigner *signer, const gchar *unsigned_event,
                     GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(signer != NULL);
  start_call(signer, OP_SIGN, unsigned_event, NULL, cancellable, callback, user_data);
}

gchar *
gh_signer_sign_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == gh_signer_sign_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void
gh_signer_nip44_encrypt_async(GhSigner *signer, const gchar *plaintext,
                              const gchar *peer_pubkey_hex, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(signer != NULL);
  start_call(signer, OP_ENCRYPT, plaintext, peer_pubkey_hex, cancellable, callback, user_data);
}

void
gh_signer_nip44_decrypt_async(GhSigner *signer, const gchar *ciphertext,
                              const gchar *peer_pubkey_hex, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(signer != NULL);
  start_call(signer, OP_DECRYPT, ciphertext, peer_pubkey_hex, cancellable, callback, user_data);
}

gchar *
gh_signer_nip44_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  gpointer tag = g_task_get_source_tag(G_TASK(result));
  g_return_val_if_fail(tag == gh_signer_nip44_encrypt_async ||
                       tag == gh_signer_nip44_decrypt_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
