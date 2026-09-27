/* signer_nip5f.c - see nip55l_nip5f.h
 *
 * nips/nip5f runs one thread per connection; the gate (signer_service_g.c)
 * lives on the GLib main context. A request is handed to the main context
 * as an idle source and the connection thread waits for the reply; if the
 * client hangs up meanwhile, its parked requests are dropped the way a bus
 * caller's are when it leaves the bus.
 */
#include "nip55l_nip5f.h"

#include <gio/gio.h>
#include <string.h>

#include "nip55l_dbus_errors.h"

#ifdef NIP55L_HAVE_NIP5F
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nostr/nip19/nip19.h"
#include "nostr/nip5f/nip5f.h"
#include "signer_caller.h"
#include "signer_gate.h"

typedef struct {
  SignerCaller *who;
  gchar *key;   /* "nip5f:<n>": the gate's connection key */
  int fd;
} Conn;

typedef struct {
  gint refs;
  GMutex lock;
  GCond cond;
  gboolean done;
  gchar *error_name, *message, *result;
  SignerCaller *who;
  gchar *key;
  SignerGateOp op;
  gchar *a, *b, *selector;
} Job;

static void wipe_free(gchar *s) {
  if (!s) return;
  memset(s, 0, strlen(s));
  g_free(s);
}

static void job_unref(Job *j) {
  if (!g_atomic_int_dec_and_test(&j->refs)) return;
  g_mutex_clear(&j->lock);
  g_cond_clear(&j->cond);
  g_free(j->error_name);
  g_free(j->message);
  wipe_free(j->result);
  signer_caller_free(j->who);
  g_free(j->key);
  wipe_free(j->a);
  wipe_free(j->b);
  g_free(j->selector);
  g_free(j);
}

/* Main context. */
static void on_reply(gpointer ud, const gchar *error_name, const gchar *message, const gchar *result) {
  Job *j = ud;
  g_mutex_lock(&j->lock);
  j->error_name = g_strdup(error_name);
  j->message = g_strdup(message);
  j->result = g_strdup(result);
  j->done = TRUE;
  g_cond_signal(&j->cond);
  g_mutex_unlock(&j->lock);
  job_unref(j);
}

static gboolean submit_idle(gpointer data) {
  Job *j = data;
  signer_gate_submit(j->who, j->key, j->op, j->a, j->b, j->selector, on_reply, j);
  return G_SOURCE_REMOVE;
}

static gboolean closed_idle(gpointer data) {
  signer_gate_connection_closed(data);
  return G_SOURCE_REMOVE;
}

/* Run on the default main context, where the D-Bus service runs. (Not
 * g_main_context_invoke: that runs @fn on this thread if nobody owns the
 * context at that moment.) */
static void on_main(GSourceFunc fn, gpointer data, GDestroyNotify destroy) {
  GSource *src = g_idle_source_new();
  g_source_set_callback(src, fn, data, destroy);
  g_source_attach(src, NULL);
  g_source_unref(src);
}

/* The client closed its end (EOF), as opposed to having sent more. */
static gboolean peer_gone(int fd) {
  char c;
  ssize_t n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
  return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

/* JSON string literal for @s. */
static gchar *json_string(const gchar *s) {
  GString *o = g_string_sized_new(strlen(s) + 2);
  g_string_append_c(o, '"');
  for (const guchar *p = (const guchar *)s; *p; p++) {
    switch (*p) {
      case '"':  g_string_append(o, "\\\""); break;
      case '\\': g_string_append(o, "\\\\"); break;
      case '\n': g_string_append(o, "\\n"); break;
      case '\r': g_string_append(o, "\\r"); break;
      case '\t': g_string_append(o, "\\t"); break;
      default:
        if (*p < 0x20) g_string_append_printf(o, "\\u%04x", *p);
        else g_string_append_c(o, (gchar)*p);
    }
  }
  g_string_append_c(o, '"');
  return g_string_free(o, FALSE);
}

static int error_code(const gchar *name) {
  if (g_strcmp0(name, ORG_NOSTR_SIGNER_ERR_INVALID_INPUT) == 0) return NIP5F_ERR_INVALID_PARAMS;
  if (g_strcmp0(name, ORG_NOSTR_SIGNER_ERR_NO_KEY) == 0) return NIP5F_ERR_NO_KEY;
  if (g_strcmp0(name, ORG_NOSTR_SIGNER_ERR_APPROVAL) == 0 ||
      g_strcmp0(name, ORG_NOSTR_SIGNER_ERR_PERMISSION) == 0) return NIP5F_ERR_DECLINED;
  return NIP5F_ERR_INTERNAL;
}

static void *hook_open(void *ud, const Nip5fPeer *peer) {
  (void)ud;
  static gint counter;
  if (peer->have_creds && peer->uid != (unsigned int)getuid()) return NULL;
  Conn *c = g_new0(Conn, 1);
  c->key = g_strdup_printf("nip5f:%d", g_atomic_int_add(&counter, 1) + 1);
  c->fd = peer->conn_fd;
  c->who = signer_caller_for_peer(c->key, peer->have_creds, peer->uid,
                                  peer->pid > 0 ? (guint32)peer->pid : 0, peer->pidfd);
  g_message("nostr-signer: NIP-5F connection %s from %s", c->key,
            c->who->principal ? c->who->principal : "(unidentified)");
  return c;
}

static void hook_close(void *ud, void *conn) {
  (void)ud;
  Conn *c = conn;
  on_main(closed_idle, g_strdup(c->key), g_free);
  signer_caller_free(c->who);
  g_free(c->key);
  g_free(c);
}

static int hook_request(void *ud, void *conn, const char *method, const char *a, const char *b,
                        char **out_result_json, char **out_message) {
  (void)ud;
  Conn *c = conn;
  gboolean list = FALSE;
  SignerGateOp op;
  const char *ga = NULL, *gb = NULL, *sel = NULL;
  if (g_strcmp0(method, "get_public_key") == 0) {
    op = SIGNER_GATE_GET_PUBLIC_KEY;
  } else if (g_strcmp0(method, "list_public_keys") == 0) {
    /* Only the active identity: naming the others would disclose them
     * without a grant for each. */
    op = SIGNER_GATE_GET_PUBLIC_KEY;
    list = TRUE;
  } else if (g_strcmp0(method, "sign_event") == 0) {
    op = SIGNER_GATE_SIGN_EVENT; ga = a; sel = b;
  } else if (g_strcmp0(method, "nip44_encrypt") == 0) {
    op = SIGNER_GATE_NIP44_ENCRYPT; ga = b; gb = a;
  } else if (g_strcmp0(method, "nip44_decrypt") == 0) {
    op = SIGNER_GATE_NIP44_DECRYPT; ga = b; gb = a;
  } else {
    return NIP5F_ERR_UNSUPPORTED;
  }

  Job *j = g_new0(Job, 1);
  j->refs = 2; /* this thread + the reply */
  g_mutex_init(&j->lock);
  g_cond_init(&j->cond);
  j->who = signer_caller_copy(c->who);
  j->key = g_strdup(c->key);
  j->op = op;
  j->a = g_strdup(ga);
  j->b = g_strdup(gb);
  j->selector = g_strdup(sel);
  on_main(submit_idle, j, NULL);

  gboolean cancelled = FALSE;
  g_mutex_lock(&j->lock);
  while (!j->done) {
    if (g_cond_wait_until(&j->cond, &j->lock, g_get_monotonic_time() + G_TIME_SPAN_SECOND / 2))
      continue;
    if (!cancelled && peer_gone(c->fd)) {
      cancelled = TRUE; /* the gate answers "caller disconnected" */
      on_main(closed_idle, g_strdup(c->key), g_free);
    }
  }
  g_mutex_unlock(&j->lock);

  int rc = 0;
  if (j->error_name) {
    rc = error_code(j->error_name);
    *out_message = strdup(j->message ? j->message : j->error_name);
  } else if (!j->result) {
    rc = NIP5F_ERR_INTERNAL;
  } else if (op == SIGNER_GATE_GET_PUBLIC_KEY) {
    /* NIP-5F speaks hex pubkeys; the service returns the npub. */
    uint8_t pk[32];
    if (nostr_nip19_decode_npub(j->result, pk) != 0) {
      rc = NIP5F_ERR_INTERNAL;
    } else {
      gchar hex[65];
      for (int i = 0; i < 32; i++) g_snprintf(hex + 2 * i, 3, "%02x", pk[i]);
      *out_result_json = list ? g_strdup_printf("[\"%s\"]", hex) : g_strdup_printf("\"%s\"", hex);
    }
  } else if (op == SIGNER_GATE_SIGN_EVENT) {
    *out_result_json = g_strdup(j->result);
  } else {
    *out_result_json = json_string(j->result);
  }
  /* nips/nip5f frees with free(): hand over malloc'd copies. */
  if (*out_result_json) {
    gchar *g = *out_result_json;
    *out_result_json = strdup(g);
    wipe_free(g);
    if (!*out_result_json) rc = NIP5F_ERR_INTERNAL;
  }
  job_unref(j);
  return rc;
}

static const Nip5fServerHooks hooks = { hook_open, hook_request, hook_close };
static void *server;

gboolean nip55l_nip5f_start(const gchar *socket_path, GError **error) {
  g_return_val_if_fail(socket_path && *socket_path, FALSE);
  if (server) nip55l_nip5f_stop();
  if (nostr_nip5f_server_start_with_hooks(socket_path, &hooks, NULL, &server) != 0) {
    server = NULL;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot listen on %s", socket_path);
    return FALSE;
  }
  g_message("nostr-signer: NIP-5F socket at %s (same approvals as D-Bus)", socket_path);
  return TRUE;
}

void nip55l_nip5f_stop(void) {
  if (!server) return;
  (void)nostr_nip5f_server_stop(server);
  server = NULL;
}

gboolean nip55l_nip5f_serve_unidentified(int fd) {
  Nip5fPeer peer = { 0, 0, 0, -1, fd };
  return nostr_nip5f_serve_connection(fd, &peer, &hooks, NULL) == 0;
}

#else /* !NIP55L_HAVE_NIP5F */

gboolean nip55l_nip5f_start(const gchar *socket_path, GError **error) {
  (void)socket_path;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "built without NIP-5F (ENABLE_NIP5F=OFF)");
  return FALSE;
}

void nip55l_nip5f_stop(void) {}

gboolean nip55l_nip5f_serve_unidentified(int fd) {
  (void)fd;
  return FALSE;
}

#endif
