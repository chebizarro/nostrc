#include "gh-signer-private.h"
#include "gh-identity.h"
#include "nostr-event.h"

#include <string.h>

#define MAX_RESULT (1024 * 1024)

typedef enum { OP_SIGN, OP_ENCRYPT, OP_DECRYPT, OP_NIP04_DECRYPT } Operation;
typedef struct _Pending Pending;
struct _GhSigner {
  gint refs;
  gchar *npub;
  gchar *pubkey;
  guint generation;
  gboolean remote;
  union { GhNip55lSigner *local; GhNip46Signer *remote; } backend;
  GPtrArray *pending; /* non-owning; each Pending holds a signer reference */
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
signer_ref(GhSigner *self)
{
  g_atomic_int_inc(&self->refs);
  return self;
}

static void
signer_unref(GhSigner *self)
{
  if (!g_atomic_int_dec_and_test(&self->refs)) return;
  g_assert_cmpuint(self->pending->len, ==, 0);
  if (self->remote)
    gh_signer_nip46_free(self->backend.remote);
  else
    gh_signer_nip55l_free(self->backend.local);
  g_ptr_array_unref(self->pending);
  g_free(self->npub);
  g_free(self->pubkey);
  g_free(self);
}

static GhSigner *
new_signer(const gchar *npub, GError **error)
{
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(npub);
  if (!pubkey) {
    g_set_error_literal(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                        "A valid selected npub is required");
    return NULL;
  }
  GhSigner *self = g_new0(GhSigner, 1);
  self->refs = 1;
  self->npub = g_strdup(npub);
  self->pubkey = g_steal_pointer(&pubkey);
  self->pending = g_ptr_array_new();
  return self;
}

GhSigner *
gh_signer_new(GDBusConnection *bus, const gchar *npub, GError **error)
{
  g_return_val_if_fail(G_IS_DBUS_CONNECTION(bus), NULL);
  GhSigner *self = new_signer(npub, error);
  if (!self) return NULL;
  self->backend.local = gh_signer_nip55l_new(bus, npub, error);
  if (!self->backend.local) {
    signer_unref(self);
    return NULL;
  }
  return self;
}

GhSigner *
gh_signer_new_nip46(GhNip46Session *session, const gchar *npub, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP46_SESSION(session), NULL);
  GhSigner *self = new_signer(npub, error);
  if (!self) return NULL;
  self->remote = TRUE;
  self->backend.remote = gh_signer_nip46_new(session);
  return self;
}

void
gh_signer_free(GhSigner *self)
{
  if (!self) return;
  self->generation++;
  for (guint i = 0; i < self->pending->len; i++) {
    Pending *p = g_ptr_array_index(self->pending, i);
    g_cancellable_cancel(p->cancel);
  }
  signer_unref(self);
}

gboolean
gh_signer_select(GhSigner *self, const gchar *npub, GError **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(npub);
  if (!pubkey) {
    g_set_error_literal(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                        "A valid selected npub is required");
    return FALSE;
  }
  if (self->remote && g_strcmp0(self->npub, npub) != 0) {
    g_set_error_literal(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                        "A remote signer is bound to one account");
    return FALSE;
  }
  self->generation++;
  for (guint i = 0; i < self->pending->len; i++) {
    Pending *p = g_ptr_array_index(self->pending, i);
    g_cancellable_cancel(p->cancel);
  }
  if (!self->remote && !gh_signer_nip55l_select(self->backend.local, npub, error))
    return FALSE;
  g_free(self->npub);
  g_free(self->pubkey);
  self->npub = g_strdup(npub);
  self->pubkey = g_steal_pointer(&pubkey);
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
valid_nip04_ciphertext(const gchar *text)
{
  if (!text || strlen(text) > MAX_RESULT) return FALSE;
  const gchar *iv = strstr(text, "?iv=");
  if (!iv || iv == text || !iv[4]) return FALSE;
  for (const gchar *c = text; *c; c++)
    if (c < iv || c >= iv + 4)
      if (!g_ascii_isalnum(*c) && *c != '+' && *c != '/' && *c != '=') return FALSE;
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

#define SIGN_CREATED_AT_DRIFT 600

static GhSignerError
signed_event_error(Pending *p, const gchar *json)
{
  if (!json || strlen(json) > MAX_RESULT) return GH_SIGNER_ERROR_INVALID_RESULT;
  NostrEvent *event = nostr_event_new();
  if (!event) return GH_SIGNER_ERROR_INVALID_RESULT;
  GhSignerError code = GH_SIGNER_ERROR_INVALID_RESULT;
  if (nostr_event_deserialize_compact(event, json, NULL) == 1 &&
      nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_OK) {
    if (g_strcmp0(event->pubkey, p->request->pubkey) != 0)
      code = GH_SIGNER_ERROR_KEY_MISMATCH;
    /* nostr_event_validate() above re-derived the id from the signed
     * content. Some signers re-stamp created_at; allow a bounded drift
     * (nostrc-8xfib.1) but keep kind, content and tags exact. */
    else if (ABS(event->created_at - p->request->created_at) <= SIGN_CREATED_AT_DRIFT &&
             event->kind == p->request->kind &&
             g_strcmp0(event->content, p->request->content) == 0 &&
             tags_match(event->tags, p->request->tags))
      code = 0;
  }
  nostr_event_free(event);
  return code;
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

static void
cancel_from_caller(GCancellable *caller, gpointer data)
{
  (void)caller;
  Pending *p = data;
  g_cancellable_cancel(p->cancel);
}

static void
backend_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Pending *p = data;
  GhSigner *self = p->signer;
  g_autoptr(GError) error = NULL;
  gchar *value;
  if (self->remote)
    value = gh_signer_nip46_call_finish(self->backend.remote, result, &error);
  else if (p->op == OP_SIGN)
    value = gh_signer_nip55l_sign_finish(result, &error);
  else
    value = gh_signer_nip55l_nip44_finish(result, &error);
  g_ptr_array_remove(self->pending, p);
  if (g_cancellable_is_cancelled(p->cancel) || p->generation != self->generation) {
    g_free(value);
    g_task_return_new_error(p->task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                            "Signer operation cancelled");
  } else if (error) {
    g_free(value);
    g_task_return_error(p->task, g_steal_pointer(&error));
  } else {
    GhSignerError code = p->op == OP_SIGN ? signed_event_error(p, value) :
      p->op == OP_ENCRYPT ? (valid_ciphertext(value) ? 0 : GH_SIGNER_ERROR_INVALID_RESULT) :
      (value && strlen(value) <= MAX_RESULT && g_utf8_validate(value, -1, NULL)
        ? 0 : GH_SIGNER_ERROR_INVALID_RESULT);
    if (code) {
      g_free(value);
      g_task_return_new_error(p->task, GH_SIGNER_ERROR, code,
                              "Signer returned an unverified operation result");
    } else {
      g_task_return_pointer(p->task, value, g_free);
    }
  }
  pending_free(p);
}

static void
start_call(GhSigner *self, Operation op, const gchar *input, const gchar *peer,
           GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  if (op == OP_SIGN)
    g_task_set_source_tag(task, gh_signer_sign_async);
  else if (op == OP_ENCRYPT)
    g_task_set_source_tag(task, gh_signer_nip44_encrypt_async);
  else if (op == OP_DECRYPT)
    g_task_set_source_tag(task, gh_signer_nip44_decrypt_async);
  else
    g_task_set_source_tag(task, gh_signer_nip04_decrypt_async);
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
        g_strcmp0(request->pubkey, self->pubkey) != 0 ||
        request->id || request->sig || !request->content) {
      if (request) nostr_event_free(request);
      g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                              "Unsigned event does not match the selected identity");
      g_object_unref(task);
      return;
    }
  } else if (op == OP_NIP04_DECRYPT && !valid_nip04_ciphertext(input)) {
    g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                            "Invalid NIP-04 ciphertext");
    g_object_unref(task);
    return;
  } else if (op == OP_DECRYPT && !valid_ciphertext(input)) {
    g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                            "Invalid NIP-44 ciphertext");
    g_object_unref(task);
    return;
  }
  Pending *p = g_new0(Pending, 1);
  p->signer = signer_ref(self);
  p->task = task;
  p->cancel = g_cancellable_new();
  p->caller = cancellable ? g_object_ref(cancellable) : NULL;
  p->generation = self->generation;
  p->op = op;
  p->request = request;
  g_ptr_array_add(self->pending, p);
  if (p->caller)
    p->caller_handler = g_cancellable_connect(p->caller, G_CALLBACK(cancel_from_caller),
                                               p, NULL);
  if (self->remote) {
    const gchar *method = op == OP_SIGN ? "sign_event" :
      op == OP_ENCRYPT ? "nip44_encrypt" :
      op == OP_DECRYPT ? "nip44_decrypt" : "nip04_decrypt";
    gh_signer_nip46_call_async(self->backend.remote, method, input, peer,
                               p->cancel, backend_done, p);
  } else if (op == OP_SIGN) {
    gh_signer_nip55l_sign_async(self->backend.local, input, p->cancel, backend_done, p);
  } else if (op == OP_ENCRYPT) {
    gh_signer_nip55l_nip44_encrypt_async(self->backend.local, input, peer,
                                         p->cancel, backend_done, p);
  } else if (op == OP_DECRYPT) {
    gh_signer_nip55l_nip44_decrypt_async(self->backend.local, input, peer,
                                         p->cancel, backend_done, p);
  } else {
    gh_signer_nip55l_nip04_decrypt_async(self->backend.local, input, peer,
                                         p->cancel, backend_done, p);
  }
}

void
gh_signer_sign_async(GhSigner *self, const gchar *event, GCancellable *cancel,
                     GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(self != NULL);
  start_call(self, OP_SIGN, event, NULL, cancel, callback, data);
}

gchar *
gh_signer_sign_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == gh_signer_sign_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void
gh_signer_nip44_encrypt_async(GhSigner *self, const gchar *text, const gchar *peer,
                               GCancellable *cancel, GAsyncReadyCallback cb, gpointer data)
{
  g_return_if_fail(self != NULL);
  start_call(self, OP_ENCRYPT, text, peer, cancel, cb, data);
}
void
gh_signer_nip44_decrypt_async(GhSigner *self, const gchar *text, const gchar *peer,
                               GCancellable *cancel, GAsyncReadyCallback cb, gpointer data)
{
  g_return_if_fail(self != NULL);
  start_call(self, OP_DECRYPT, text, peer, cancel, cb, data);
}
void
gh_signer_nip04_decrypt_async(GhSigner *self, const gchar *text, const gchar *peer,
                               GCancellable *cancel, GAsyncReadyCallback cb, gpointer data)
{
  g_return_if_fail(self != NULL);
  start_call(self, OP_NIP04_DECRYPT, text, peer, cancel, cb, data);
}

gchar *
gh_signer_nip44_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  gpointer tag = g_task_get_source_tag(G_TASK(result));
  g_return_val_if_fail(tag == gh_signer_nip44_encrypt_async ||
                       tag == gh_signer_nip44_decrypt_async ||
                       tag == gh_signer_nip04_decrypt_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
