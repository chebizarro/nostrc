#include "gh-signer-private.h"
#include "gh-identity.h"
#include "nostr-event.h"

#include <string.h>
#ifdef G_OS_UNIX
#include <errno.h>
#include <sys/socket.h>
#endif

#define SIGNER_BUS "org.nostr.Signer"
#define SIGNER_PATH "/org/nostr/signer"
#define SIGNER_INTERFACE "org.nostr.Signer"
/* A named claim, not executable attestation. The daemon labels it accordingly
 * when this platform cannot verify the caller. */
#define SIGNER_APP_ID "org.nostr.Groundhog"
#define MAX_RESULT (1024 * 1024)
/* The signer can hold an approval for 300 seconds. Leave room for its typed
 * result instead of reporting a local transport timeout at 30 seconds. */
#define SIGNER_CALL_TIMEOUT_MS (330 * 1000)
/* Operations retained locally while one declaration is unanswered. The
 * declaration itself ends within SIGNER_CALL_TIMEOUT_MS, so the wait is time
 * bounded; this bounds its memory and fails further work visibly. */
#define WAITING_MAX 64

typedef enum { OP_SIGN, OP_ENCRYPT, OP_DECRYPT, OP_NIP04_DECRYPT, OP_DECLARE } Operation;
typedef struct _Pending Pending;
struct _GhNip55lSigner {
  gint refs;
  GDBusConnection *bus; /* shared session bus, used only to verify the private one */
  gchar *npub;
  gchar *pubkey;
  GPtrArray *pending; /* non-owning; each Pending keeps signer alive */
  /* G18: while a RequestPermissions declaration is unanswered, ordinary
   * operations wait here (a subset of pending) so a slow first review is
   * never joined by per-kind prompts; they start once it is answered. */
  GPtrArray *waiting;
  gboolean declaring;
  guint generation;
};
static void release_waiting(GhNip55lSigner *signer, const GError *outcome);
static void fail_pending(Pending *p, GError *error);
struct _Pending {
  GhNip55lSigner *signer;
  GTask *task;
  GCancellable *cancel;
  GDBusConnection *bus; /* sender private to this operation */
  GCancellable *caller;
  GMutex bus_lock; /* caller cancellation may arrive from another thread */
  gboolean revoked;
  gboolean close_started; /* guarded by bus_lock */
  gulong caller_handler;
  guint generation;
  Operation op;
  NostrEvent *request;
  gchar *input;
  gchar *peer;
  GVariant *context;
  gboolean wrapped;
  gboolean named;
  GError *outcome; /* OP_DECLARE: how it ended, for the operations waiting on it */
};
/* Every failure of a Pending goes through here so a declaration remembers
 * its outcome for the operations that waited on it. Takes the error. */
static void
fail_pending(Pending *p, GError *error)
{
  if (p->op == OP_DECLARE) {
    g_clear_error(&p->outcome);
    p->outcome = g_error_copy(error);
  }
  g_task_return_error(p->task, error);
}

static GhNip55lSigner *
signer_ref(GhNip55lSigner *signer)
{
  g_atomic_int_inc(&signer->refs);
  return signer;
}

static void
signer_unref(GhNip55lSigner *signer)
{
  if (!g_atomic_int_dec_and_test(&signer->refs)) return;
  g_assert(signer->pending->len == 0);
  g_assert(signer->waiting->len == 0);
  g_ptr_array_unref(signer->pending);
  g_ptr_array_unref(signer->waiting);
  g_clear_object(&signer->bus);
  g_free(signer->npub);
  g_free(signer->pubkey);
  g_free(signer);
}

GhNip55lSigner *
gh_signer_nip55l_new(GDBusConnection *bus, const gchar *selected_npub, GError **error)
{
  g_return_val_if_fail(G_IS_DBUS_CONNECTION(bus), NULL);
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(selected_npub);
  if (!pubkey) {
    g_set_error_literal(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                        "A valid selected npub is required");
    return NULL;
  }
  GhNip55lSigner *signer = g_new0(GhNip55lSigner, 1);
  signer->refs = 1;
  signer->bus = g_object_ref(bus);
  signer->npub = g_strdup(selected_npub);
  signer->pubkey = g_steal_pointer(&pubkey);
  signer->pending = g_ptr_array_new();
  signer->waiting = g_ptr_array_new();
  return signer;
}

typedef struct {
  GDBusConnection *bus;
  GMainContext *context;
  gulong closed_handler;
  gboolean requested;
  gboolean finished;
  gboolean closed;
  gboolean cleanup_queued;
} PrivateClose;

static void
post_close_source(PrivateClose *close, GSourceFunc callback)
{
  GSource *source = g_idle_source_new();
  g_source_set_priority(source, G_PRIORITY_DEFAULT);
  g_source_set_callback(source, callback, close, NULL);
  g_source_attach(source, close->context);
  g_source_unref(source);
}

static gboolean
private_close_free(gpointer data)
{
  PrivateClose *close = data;
  if (close->closed_handler)
    g_signal_handler_disconnect(close->bus, close->closed_handler);
  g_object_unref(close->bus);
  g_main_context_unref(close->context);
  g_free(close);
  return G_SOURCE_REMOVE;
}

static void
private_close_maybe_finished(PrivateClose *close)
{
  if (close->finished && close->closed && !close->cleanup_queued) {
    close->cleanup_queued = TRUE;
    post_close_source(close, private_close_free);
  }
}

static void private_request_close(PrivateClose *close);

static void
private_closed(GDBusConnection *bus, gboolean remote, GError *error, gpointer data)
{
  (void)bus; (void)remote; (void)error;
  PrivateClose *close = data;
  close->closed = TRUE;
  private_request_close(close);
  private_close_maybe_finished(close);
}

static void
private_close_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  PrivateClose *close = data;
  g_autoptr(GError) error = NULL;
  if (!g_dbus_connection_close_finish(G_DBUS_CONNECTION(source), result, &error) &&
      !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CLOSED))
    g_warning("Cannot close private signer connection: %s", error->message);
  close->finished = TRUE;
  private_close_maybe_finished(close);
}

static void
private_request_close(PrivateClose *close)
{
  if (close->requested) return;
  close->requested = TRUE;
  g_dbus_connection_close(close->bus, NULL, private_close_finished, close);
}

static gboolean
private_close_start(gpointer data)
{
  PrivateClose *close = data;
  if (g_dbus_connection_is_closed(close->bus))
    return private_close_free(close);
  close->closed_handler = g_signal_connect(close->bus, "closed", G_CALLBACK(private_closed), close);
  GIOStream *stream = g_dbus_connection_get_stream(close->bus);
#ifdef G_OS_UNIX
  if (G_IS_SOCKET_CONNECTION(stream)) {
    /* Revoke I/O without invalidating the retained connection's fd. EOF
     * removes GIO's read source before final close. Use the kernel call:
     * GSocket's mutable flags belong to GDBus's private worker thread. */
    GSocket *socket = g_socket_connection_get_socket(G_SOCKET_CONNECTION(stream));
    gint result;
    do {
      result = shutdown(g_socket_get_fd(socket), SHUT_RDWR);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != ENOTCONN) {
      g_warning("Cannot shut down private signer connection: %s", g_strerror(errno));
      private_request_close(close);
    }
  } else
#else
  (void)stream;
#endif
  {
    private_request_close(close);
  }
  return G_SOURCE_REMOVE;
}

static void
close_private(GDBusConnection *bus, GMainContext *context)
{
  PrivateClose *close = g_new0(PrivateClose, 1);
  close->bus = g_object_ref(bus);
  close->context = g_main_context_ref(context);
  /* Caller cancellation can run on another thread. Start on the connection's
   * owning context and retain it through close_finish AND closed dispatch,
   * so last-unref cannot race its GIO read-source cancellation. */
  post_close_source(close, private_close_start);
}

static void
close_pending_bus(Pending *p)
{
  g_mutex_lock(&p->bus_lock);
  GDBusConnection *bus = NULL;
  if (p->bus && !p->close_started) {
    p->close_started = TRUE;
    bus = g_object_ref(p->bus);
  }
  g_mutex_unlock(&p->bus_lock);
  if (bus) {
    close_private(bus, g_task_get_context(p->task));
    g_object_unref(bus);
  }
}

static void
cancel_pending(Pending *p)
{
  /* Revoking this operation's private sender must not affect its peers. */
  g_mutex_lock(&p->bus_lock);
  p->revoked = TRUE;
  g_mutex_unlock(&p->bus_lock);
  close_pending_bus(p);
  g_cancellable_cancel(p->cancel);
}

void
gh_signer_nip55l_free(GhNip55lSigner *signer)
{
  if (!signer) return;
  for (guint i = 0; i < signer->pending->len; i++)
    cancel_pending(g_ptr_array_index(signer->pending, i));
  signer_unref(signer);
}

gboolean
gh_signer_nip55l_select(GhNip55lSigner *signer, const gchar *npub, GError **error)
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
    cancel_pending(g_ptr_array_index(signer->pending, i));
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

/* NIP-04: "<base64>?iv=<base64>", bounded like a NIP-44 payload. */
static gboolean
valid_nip04_ciphertext(const gchar *text)
{
  if (!text || strlen(text) > MAX_RESULT)
    return FALSE;
  const gchar *iv = strstr(text, "?iv=");
  if (!iv || iv == text || !iv[4])
    return FALSE;
  for (const gchar *c = text; *c; c++)
    if (c < iv || c >= iv + 4)
      if (!g_ascii_isalnum(*c) && *c != '+' && *c != '/' && *c != '=')
        return FALSE;
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
  cancel_pending(data);
}

static void
pending_free(Pending *p)
{
  /* However the declaration ended, the operations that waited on it are
   * resolved by that one outcome: launched, or failed the same way. */
  if (p->op == OP_DECLARE && p->signer->declaring) release_waiting(p->signer, p->outcome);
  g_clear_error(&p->outcome);
  if (p->caller_handler) g_cancellable_disconnect(p->caller, p->caller_handler);
  g_clear_object(&p->caller);
  g_clear_object(&p->cancel);
  g_clear_object(&p->bus);
  g_mutex_clear(&p->bus_lock);
  if (p->request) nostr_event_free(p->request);
  g_free(p->input);
  g_free(p->peer);
  g_clear_pointer(&p->context, g_variant_unref);
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
  if (g_strcmp0(remote, "org.nostr.Signer.Error.IdentityChanged") == 0)
    return g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_KEY_MISMATCH,
                               "Signer identity changed during approval");
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

static void send_call(Pending *p, gboolean wrapped);
static void
call_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Pending *p = data;
  GhNip55lSigner *signer = p->signer;
  g_autoptr(GError) bus_error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                                              &bus_error);
  if (p->wrapped && g_error_matches(bus_error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) &&
      !g_cancellable_is_cancelled(p->cancel) && p->generation == signer->generation &&
      !g_dbus_connection_is_closed(p->bus)) {
    send_call(p, FALSE);
    return;
  }
  if (reply && p->wrapped && g_variant_is_of_type(reply, G_VARIANT_TYPE("(v)"))) {
    g_autoptr(GVariant) boxed = g_variant_get_child_value(reply, 0);
    GVariant *tuple = g_variant_get_variant(boxed);
    g_variant_unref(reply); reply = tuple;
  }
  g_ptr_array_remove(signer->pending, p);
  if (g_cancellable_is_cancelled(p->cancel) || p->generation != signer->generation ||
      g_dbus_connection_is_closed(p->bus)) {
    fail_pending(p, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                                        "Signer operation cancelled"));
  } else if (bus_error) {
    fail_pending(p, map_bus_error(bus_error));
  } else if (p->op == OP_DECLARE) {
    if (reply && g_variant_is_of_type(reply, G_VARIANT_TYPE("(a{sv})")))
      g_task_return_boolean(p->task, TRUE);
    else
      fail_pending(p, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_RESULT,
                                          "Signer returned an invalid declaration result"));
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
  close_pending_bus(p);
  pending_free(p);
}

static const gchar *
operation_method(Operation op, gboolean named)
{
  switch (op) {
  case OP_SIGN: return "SignEvent";
  case OP_ENCRYPT: return named ? "NIP44EncryptForApp" : "NIP44Encrypt";
  case OP_NIP04_DECRYPT: return named ? "NIP04DecryptForApp" : "NIP04Decrypt";
  case OP_DECRYPT: return named ? "NIP44DecryptForApp" : "NIP44Decrypt";
  case OP_DECLARE: return "RequestPermissions";
  }
  g_assert_not_reached();
}
/* RequestPermissions(aa{sv} permissions, a{sv} context): the tokens use the
 * NIP-46/NIP-55 vocabulary; identity pins the selected npub and app_id
 * carries the same named claim as every ForApp call. */
static GVariant *
declaration_arguments(Pending *p)
{
  GVariantBuilder entries, context;
  g_variant_builder_init(&entries, G_VARIANT_TYPE("aa{sv}"));
  g_auto(GStrv) tokens = g_strsplit(p->input, ",", -1);
  for (guint i = 0; tokens[i]; i++) {
    GVariantBuilder entry; g_variant_builder_init(&entry, G_VARIANT_TYPE_VARDICT);
    if (g_str_has_prefix(tokens[i], "sign_event:")) {
      g_variant_builder_add(&entry, "{sv}", "type", g_variant_new_string("sign_event"));
      g_variant_builder_add(&entry, "{sv}", "kind",
                            g_variant_new_uint32((guint32)g_ascii_strtoull(tokens[i] + strlen("sign_event:"), NULL, 10)));
    } else {
      g_variant_builder_add(&entry, "{sv}", "type", g_variant_new_string(tokens[i]));
    }
    g_variant_builder_add_value(&entries, g_variant_builder_end(&entry));
  }
  g_variant_builder_init(&context, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&context, "{sv}", "identity", g_variant_new_string(p->signer->npub));
  g_variant_builder_add(&context, "{sv}", "app_id", g_variant_new_string(SIGNER_APP_ID));
  g_variant_builder_add(&context, "{sv}", "reason",
                        g_variant_new_string("Messages, groups, lists and relay sign-in for this account"));
  return g_variant_new("(aa{sv}a{sv})", &entries, &context);
}

static void
introspection_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Pending *p = data;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  const gchar *xml = NULL;
  if (reply) g_variant_get(reply, "(&s)", &xml);
  g_autoptr(GDBusNodeInfo) node = xml ? g_dbus_node_info_new_for_xml(xml, NULL) : NULL;
  GDBusInterfaceInfo *info = node ? g_dbus_node_info_lookup_interface(node, SIGNER_INTERFACE) : NULL;
  p->named = info && g_dbus_interface_info_lookup_method(info, operation_method(p->op, TRUE));
  GDBusMethodInfo *wrapper = info ? g_dbus_interface_info_lookup_method(info, "CallWithContext") : NULL;
  gboolean context = p->context && wrapper;
  if (context && p->op != OP_SIGN &&
      g_strcmp0(g_dbus_annotation_info_lookup(wrapper->annotations, "org.nostr.Signer.NamedContext"), "true"))
    p->named = FALSE; /* older context services accept only the legacy tuple */
  gboolean flow = info && g_dbus_interface_info_lookup_method(info, "ApproveFlowRequest");
  if (context && !flow) {
    /* G02 services understand context but not the later bounded-flow extension. */
    GVariantDict dict; g_variant_dict_init(&dict, p->context);
    g_variant_dict_remove(&dict, "flow_size");
    g_clear_pointer(&p->context, g_variant_unref);
    p->context = g_variant_ref_sink(g_variant_dict_end(&dict));
  }
  send_call(p, context);
}
static void
connection_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Pending *p = data;
  GhNip55lSigner *signer = p->signer;
  g_autoptr(GError) error = NULL;
  GDBusConnection *bus = g_dbus_connection_new_for_address_finish(result, &error);
  if (g_cancellable_is_cancelled(p->cancel) || p->generation != signer->generation) {
    if (bus) {
      close_private(bus, g_task_get_context(p->task));
      g_object_unref(bus);
    }
    g_ptr_array_remove(signer->pending, p);
    fail_pending(p, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                                        "Signer operation cancelled"));
    pending_free(p);
    return;
  }
  if (!bus || g_strcmp0(g_dbus_connection_get_guid(bus),
                        g_dbus_connection_get_guid(signer->bus)) != 0) {
    if (bus) {
      close_private(bus, g_task_get_context(p->task));
      g_object_unref(bus);
    }
    g_ptr_array_remove(signer->pending, p);
    fail_pending(p, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE,
                                        "Cannot connect to the selected session bus"));
    pending_free(p);
    return;
  }
  g_mutex_lock(&p->bus_lock);
  if (p->revoked) {
    g_mutex_unlock(&p->bus_lock);
    close_private(bus, g_task_get_context(p->task));
    g_object_unref(bus);
    g_ptr_array_remove(signer->pending, p);
    fail_pending(p, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                                        "Signer operation cancelled"));
    pending_free(p);
    return;
  }
  p->bus = bus;
  g_mutex_unlock(&p->bus_lock);
  /* The opt-in is per bus connection and lost if the service restarts, so it
   * precedes every gated call; the service handles one connection's calls in
   * order. No reply is requested, so a pre-0.5.0 service's UnknownMethod
   * error is never sent; its failures then all arrive as ApprovalDenied. */
  g_dbus_connection_call(p->bus, SIGNER_BUS, SIGNER_PATH, SIGNER_INTERFACE,
                         "EnableTypedApprovalErrors", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                         -1, NULL, NULL, NULL);
  /* A declaration needs no capability probe: an older service answers
   * UnknownMethod, which maps to UNAVAILABLE and leaves per-request consent. */
  if (p->op == OP_DECLARE) { send_call(p, FALSE); return; }
  /* Choose the named API before executing, retaining legacy compatibility
   * without replaying a call after an uncertain result. */
  g_dbus_connection_call(p->bus, SIGNER_BUS, SIGNER_PATH, "org.freedesktop.DBus.Introspectable",
      "Introspect", NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 5000,
      p->cancel, introspection_done, p);
}

static void
send_call(Pending *p, gboolean wrapped)
{
  if (g_cancellable_is_cancelled(p->cancel) || p->generation != p->signer->generation ||
      g_dbus_connection_is_closed(p->bus)) {
    g_ptr_array_remove(p->signer->pending, p);
    fail_pending(p, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                                        "Signer operation cancelled"));
    close_pending_bus(p);
    pending_free(p); return;
  }
  const gchar *method = operation_method(p->op, p->named);
  GVariant *tuple = p->op == OP_DECLARE ? declaration_arguments(p) :
      p->op == OP_SIGN ? g_variant_new("(sss)", p->input, p->signer->npub, SIGNER_APP_ID) :
      p->named ? g_variant_new("(ssss)", p->input, p->peer, p->signer->npub, SIGNER_APP_ID) :
      g_variant_new("(sss)", p->input, p->peer, p->signer->npub);
  p->wrapped = wrapped;
  g_dbus_connection_call(p->bus, SIGNER_BUS, SIGNER_PATH, SIGNER_INTERFACE,
      wrapped ? "CallWithContext" : method,
      wrapped ? g_variant_new("(sv@a{sv})", method, tuple, g_variant_ref(p->context)) : tuple,
      p->op == OP_DECLARE ? G_VARIANT_TYPE("(a{sv})") : wrapped ? G_VARIANT_TYPE("(v)") : G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE, SIGNER_CALL_TIMEOUT_MS, p->cancel, call_done, p);
}

static void
address_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
  (void)source; (void)task_data;
  g_autoptr(GError) error = NULL;
  gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, cancel, &error);
  if (address)
    g_task_return_pointer(task, address, g_free);
  else
    g_task_return_error(task, g_steal_pointer(&error));
}

static void
address_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Pending *p = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *address = g_task_propagate_pointer(G_TASK(result), &error);
  if (g_cancellable_is_cancelled(p->cancel) ||
      p->generation != p->signer->generation) {
    g_ptr_array_remove(p->signer->pending, p);
    fail_pending(p, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                                        "Signer operation cancelled"));
    pending_free(p);
    return;
  }
  if (!address) {
    g_ptr_array_remove(p->signer->pending, p);
    fail_pending(p, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE,
                                        "Cannot resolve the selected session bus"));
    pending_free(p);
    return;
  }
  g_dbus_connection_new_for_address(address,
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL, p->cancel, connection_done, p);
}

static void
launch(Pending *p)
{
  GTask *lookup = g_task_new(NULL, p->cancel, address_done, p);
  g_task_run_in_thread(lookup, address_worker);
  g_object_unref(lookup);
}

/* The declaration's one outcome resolves every operation that waited on it.
 * A service without the method (UnknownMethod -> UNAVAILABLE) or an
 * unreadable result keeps the old per-request route: the operations start.
 * A denied, timed-out, agentless, identity-changed or cancelled review
 * answers for them too: they fail the same way and never raise the per-kind
 * reviews the declaration was meant to replace. */
static void
release_waiting(GhNip55lSigner *signer, const GError *outcome)
{
  signer->declaring = FALSE;
  gboolean proceed = !outcome ||
    g_error_matches(outcome, GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE) ||
    g_error_matches(outcome, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_RESULT);
  while (signer->waiting->len) {
    Pending *p = g_ptr_array_index(signer->waiting, 0);
    g_ptr_array_remove_index(signer->waiting, 0);
    if (proceed) {
      launch(p); /* cancelled ones finish CANCELLED through the normal path */
      continue;
    }
    g_ptr_array_remove(signer->pending, p);
    fail_pending(p, g_error_new(GH_SIGNER_ERROR, outcome->code,
                                "Not sent: the signer's permission review ended first (%s)",
                                outcome->message));
    pending_free(p);
  }
}

static void
start_call(GhNip55lSigner *signer, Operation op, const gchar *input, const gchar *peer,
           const GhSignerContext *context, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  if (op == OP_SIGN)
    g_task_set_source_tag(task, gh_signer_nip55l_sign_async);
  else if (op == OP_ENCRYPT)
    g_task_set_source_tag(task, gh_signer_nip55l_nip44_encrypt_async);
  else if (op == OP_NIP04_DECRYPT)
    g_task_set_source_tag(task, gh_signer_nip55l_nip04_decrypt_async);
  else
    g_task_set_source_tag(task, gh_signer_nip55l_nip44_decrypt_async);
  if (!input || strlen(input) > MAX_RESULT || !g_utf8_validate(input, -1, NULL) ||
      (op != OP_SIGN && !hex64(peer))) {
    g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                            "Invalid signer input");
    g_object_unref(task);
    return;
  }
  if (context && ((!context->reason || !g_utf8_validate(context->reason, -1, NULL) ||
      strlen(context->reason) > 512 || g_utf8_strlen(context->reason, -1) > 160) ||
      !context->purpose || !g_utf8_validate(context->purpose, -1, NULL) || strlen(context->purpose) > 64 ||
      !context->flow_id || !g_utf8_validate(context->flow_id, -1, NULL) ||
      !*context->flow_id || strlen(context->flow_id) > 64 || !context->flow_size || context->flow_size > 1024)) {
    g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT, "Invalid signer context");
    g_object_unref(task); return;
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
  p->signer = signer_ref(signer);
  p->task = task;
  p->cancel = g_cancellable_new();
  g_mutex_init(&p->bus_lock);
  p->caller = cancellable ? g_object_ref(cancellable) : NULL;
  p->generation = signer->generation;
  p->op = op;
  p->request = request;
  p->input = g_strdup(input);
  p->peer = g_strdup(peer);
  if (context) {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "reason", g_variant_new_string(context->reason));
    g_variant_builder_add(&b, "{sv}", "purpose", g_variant_new_string(context->purpose));
    g_variant_builder_add(&b, "{sv}", "flow_id", g_variant_new_string(context->flow_id));
    g_variant_builder_add(&b, "{sv}", "flow_size", g_variant_new_uint32(context->flow_size));
    p->context = g_variant_ref_sink(g_variant_builder_end(&b));
  }
  g_ptr_array_add(signer->pending, p);
  if (p->caller)
    p->caller_handler = g_cancellable_connect(p->caller, G_CALLBACK(cancel_from_caller),
                                               p, NULL);
  if (!signer->declaring) { launch(p); return; }
  if (signer->waiting->len >= WAITING_MAX) {
    /* Bounded wait: further work fails visibly instead of growing memory. */
    g_ptr_array_remove(signer->pending, p);
    fail_pending(p, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE,
                                        "Too many operations are waiting for the signer's permission review"));
    pending_free(p);
    return;
  }
  g_ptr_array_add(signer->waiting, p);
}

/* Declared permission set (nip55l 0.8.0 RequestPermissions). */
static gboolean
valid_token(const gchar *token)
{
  static const gchar *const names[] = {"nip04_encrypt", "nip04_decrypt", "nip44_encrypt",
      "nip44_decrypt", "get_public_key", "get_relays", "decrypt_zap_event", NULL};
  if (!token || !*token || strchr(token, ',')) return FALSE;
  if (g_strv_contains(names, token)) return TRUE;
  if (!g_str_has_prefix(token, "sign_event:")) return FALSE;
  const gchar *digits = token + strlen("sign_event:");
  if (!*digits || strlen(digits) > 5) return FALSE;
  for (const gchar *c = digits; *c; c++) if (!g_ascii_isdigit(*c)) return FALSE;
  return g_ascii_strtoull(digits, NULL, 10) <= 65535;
}

void
gh_signer_nip55l_declare_async(GhNip55lSigner *signer, const gchar *const *tokens,
                                GCancellable *cancellable, GAsyncReadyCallback callback,
                                gpointer user_data)
{
  g_return_if_fail(signer != NULL);
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  g_task_set_source_tag(task, gh_signer_nip55l_declare_async);
  guint n = tokens ? g_strv_length((gchar **)tokens) : 0;
  gboolean valid = n > 0 && n <= 64;
  for (guint i = 0; valid && i < n; i++) valid = valid_token(tokens[i]);
  if (!valid) {
    g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_INPUT,
                            "Invalid declared permission set");
    g_object_unref(task);
    return;
  }
  Pending *p = g_new0(Pending, 1);
  p->signer = signer_ref(signer);
  p->task = task;
  p->cancel = g_cancellable_new();
  g_mutex_init(&p->bus_lock);
  p->caller = cancellable ? g_object_ref(cancellable) : NULL;
  p->generation = signer->generation;
  p->op = OP_DECLARE;
  p->input = g_strjoinv(",", (gchar **)tokens);
  g_ptr_array_add(signer->pending, p);
  if (p->caller)
    p->caller_handler = g_cancellable_connect(p->caller, G_CALLBACK(cancel_from_caller),
                                               p, NULL);
  signer->declaring = TRUE;
  launch(p);
}

gboolean
gh_signer_nip55l_declare_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), FALSE);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == gh_signer_nip55l_declare_async, FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
gh_signer_nip55l_sign_async(GhNip55lSigner *signer, const gchar *unsigned_event,
                     GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(signer != NULL);
  start_call(signer, OP_SIGN, unsigned_event, NULL, NULL, cancellable, callback, user_data);
}

void
gh_signer_nip55l_sign_with_context_async(GhNip55lSigner *signer, const gchar *event,
    const GhSignerContext *context, GCancellable *cancel, GAsyncReadyCallback cb, gpointer data)
{
  g_return_if_fail(signer != NULL);
  start_call(signer, OP_SIGN, event, NULL, context, cancel, cb, data);
}
void
gh_signer_nip55l_nip44_encrypt_with_context_async(GhNip55lSigner *signer, const gchar *text,
    const gchar *peer, const GhSignerContext *context, GCancellable *cancel,
    GAsyncReadyCallback cb, gpointer data)
{
  g_return_if_fail(signer != NULL);
  start_call(signer, OP_ENCRYPT, text, peer, context, cancel, cb, data);
}
gchar *
gh_signer_nip55l_sign_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == gh_signer_nip55l_sign_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void
gh_signer_nip55l_nip44_encrypt_async(GhNip55lSigner *signer, const gchar *plaintext,
                              const gchar *peer_pubkey_hex, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(signer != NULL);
  start_call(signer, OP_ENCRYPT, plaintext, peer_pubkey_hex, NULL, cancellable, callback, user_data);
}

void
gh_signer_nip55l_nip44_decrypt_async(GhNip55lSigner *signer, const gchar *ciphertext,
                              const gchar *peer_pubkey_hex, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(signer != NULL);
  start_call(signer, OP_DECRYPT, ciphertext, peer_pubkey_hex, NULL, cancellable, callback, user_data);
}

void
gh_signer_nip55l_nip04_decrypt_async(GhNip55lSigner *signer, const gchar *ciphertext,
                              const gchar *peer_pubkey_hex, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(signer != NULL);
  start_call(signer, OP_NIP04_DECRYPT, ciphertext, peer_pubkey_hex, NULL, cancellable, callback,
             user_data);
}

gchar *
gh_signer_nip55l_nip44_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  gpointer tag = g_task_get_source_tag(G_TASK(result));
  g_return_val_if_fail(tag == gh_signer_nip55l_nip44_encrypt_async ||
                       tag == gh_signer_nip55l_nip44_decrypt_async ||
                       tag == gh_signer_nip55l_nip04_decrypt_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
