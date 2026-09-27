/* nwa-walletauth.c - see nwa-walletauth.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-walletauth.h"
#include "nwa-error.h"

#include <json.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SUB_AUTH "nwa-auth"
#define REQUEST_METHODS "pay_invoice get_balance make_invoice lookup_invoice list_transactions get_info"
#define NOTIFICATION_TYPES "payment_received payment_sent"

enum { SIGNAL_AUTHORIZED, SIGNAL_FAILED, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _NwaWalletAuth {
  GObject parent_instance;

  gchar  *secret;     /* wiped on finalize */
  gchar  *client_pk;
  gchar  *state;
  gchar **relays;
  gchar  *uri;

  NwaTransportFactory factory;
  gpointer            factory_data;
  GPtrArray          *transports;  /* owned */

  gboolean started, done;
  gint64   started_at;
  guint    settle_ms, timeout_s;
  guint    settle_id, timeout_id;

  gchar   *first_author;           /* first unconfirmed candidate */
  gchar   *first_relay;

  /* outcome, delivered from an idle: never tear transports down inside
   * their own frame callback */
  gboolean res_ok, res_confirmed;
  guint    res_why;
  gchar   *res_pk, *res_relay, *res_msg;
};

G_DEFINE_TYPE(NwaWalletAuth, nwa_wallet_auth, G_TYPE_OBJECT)

static void
wipe_free(gchar *s)
{
  if (!s) return;
  volatile gchar *p = s;
  while (*p) *p++ = 0;
  g_free(s);
}

static gboolean
relay_ok(const gchar *r)
{
  return r && (g_str_has_prefix(r, "wss://") || g_str_has_prefix(r, "ws://")) && strlen(r) <= 255 &&
         !strpbrk(r, " \t\r\n\"\\");
}

static gboolean
hex_eq_ct(const gchar *a, const gchar *b)
{
  gsize la = strlen(a), lb = strlen(b);
  guchar diff = la != lb;
  for (gsize i = 0; i < la && i < lb; i++) diff |= (guchar)(a[i] ^ b[i]);
  return diff == 0;
}

/* ---- lifecycle ---- */

static void
clear_timers(NwaWalletAuth *self)
{
  if (self->settle_id) { g_source_remove(self->settle_id); self->settle_id = 0; }
  if (self->timeout_id) { g_source_remove(self->timeout_id); self->timeout_id = 0; }
}

static void
disconnect_all(NwaWalletAuth *self)
{
  for (guint i = 0; i < self->transports->len; i++) {
    NostrPublishTransport *t = g_ptr_array_index(self->transports, i);
    nostr_publish_transport_set_listener(t, NULL, NULL);
    nostr_publish_transport_set_state_callback(t, NULL, NULL);
    nostr_publish_transport_disconnect(t);
  }
  g_ptr_array_set_size(self->transports, 0);
}

static gboolean
on_finish_idle(gpointer data)
{
  NwaWalletAuth *self = data;
  disconnect_all(self);
  if (self->res_ok)
    g_signal_emit(self, signals[SIGNAL_AUTHORIZED], 0, self->res_pk, self->res_relay, self->res_confirmed);
  else
    g_signal_emit(self, signals[SIGNAL_FAILED], 0, self->res_why, self->res_msg);
  return G_SOURCE_REMOVE;
}

static void
finish(NwaWalletAuth *self)
{
  self->done = TRUE;
  clear_timers(self);
  g_idle_add_full(G_PRIORITY_DEFAULT, on_finish_idle, g_object_ref(self), g_object_unref);
}

static void
finish_authorized(NwaWalletAuth *self, const gchar *wallet_pk, const gchar *relay, gboolean confirmed)
{
  if (self->done) return;
  self->res_ok = TRUE;
  self->res_pk = g_strdup(wallet_pk);
  self->res_relay = g_strdup(relay);
  self->res_confirmed = confirmed;
  finish(self);
}

static void
finish_failed(NwaWalletAuth *self, NwaWalletAuthFailure why, const gchar *message)
{
  if (self->done) return;
  self->res_why = why;
  self->res_msg = g_strdup(message);
  finish(self);
}

static gboolean
on_settle(gpointer data)
{
  NwaWalletAuth *self = data;
  self->settle_id = 0;
  finish_authorized(self, self->first_author, self->first_relay, FALSE);
  return G_SOURCE_REMOVE;
}

static gboolean
on_timeout(gpointer data)
{
  NwaWalletAuth *self = data;
  self->timeout_id = 0;
  finish_failed(self, NWA_WALLET_AUTH_TIMEOUT, "the wallet did not answer the connection request in time");
  return G_SOURCE_REMOVE;
}

/* ---- inbound ---- */

static void add_relay(NwaWalletAuth *self, const gchar *url);

static void
subscribe(NwaWalletAuth *self, NostrPublishTransport *t)
{
  g_autofree gchar *req = g_strdup_printf(
    "[\"REQ\",\"" SUB_AUTH "\",{\"kinds\":[13194],\"#p\":[\"%s\"],\"since\":%" G_GINT64_FORMAT "}]",
    self->client_pk, self->started_at - 60);
  (void)nostr_publish_transport_send_frame(t, req, NULL);
}

/* Content must list at least one NWC method: garbage cannot open a settle
 * window or count as a conflicting answer. */
static gboolean
content_lists_methods(const gchar *content)
{
  if (!content) return FALSE;
  g_auto(GStrv) words = g_strsplit_set(content, " \t\n,", -1);
  static const gchar *const known[] = { "pay_invoice", "get_balance", "make_invoice", "lookup_invoice",
                                        "list_transactions", "get_info", NULL };
  for (guint i = 0; words[i]; i++)
    if (g_strv_contains(known, words[i])) return TRUE;
  return FALSE;
}

static void
handle_candidate(NwaWalletAuth *self, NostrEvent *ev, const gchar *arrived_on)
{
  if (nostr_event_get_kind(ev) != 13194) return;
  const gchar *author = nostr_event_get_pubkey(ev);
  if (!author || strlen(author) != 64 || g_ascii_strcasecmp(author, self->client_pk) == 0) return;
  gint64 created = nostr_event_get_created_at(ev);
  gint64 now = (gint64)time(NULL);
  if (created < self->started_at - 60 || created > now + 300) return;
  if (!content_lists_methods(nostr_event_get_content(ev))) return;

  gboolean ptag_ok = FALSE;
  const gchar *hint = NULL, *state = NULL;
  NostrTags *tags = nostr_event_get_tags(ev);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *t = nostr_tags_get(tags, i);
    const gchar *k = t ? nostr_tag_get_key(t) : NULL;
    if (!k || nostr_tag_size(t) < 2) continue;
    if (g_str_equal(k, "p") && g_ascii_strcasecmp(nostr_tag_get_value(t), self->client_pk) == 0) {
      ptag_ok = TRUE;
      if (nostr_tag_size(t) >= 3 && relay_ok(nostr_tag_get(t, 2))) hint = nostr_tag_get(t, 2);
    } else if (g_str_equal(k, "state")) {
      state = nostr_tag_get_value(t);
    }
  }
  if (!ptag_ok) return;
  g_autofree gchar *wallet = g_ascii_strdown(author, -1);
  const gchar *relay = hint ? hint : arrived_on;

  if (state) {
    if (!hex_eq_ct(state, self->state)) {
      g_debug("walletauth: answer from %.16s… carries another request's state; ignored", wallet);
      return;
    }
    finish_authorized(self, wallet, relay, TRUE);
    return;
  }
  if (!self->first_author) {
    self->first_author = g_strdup(wallet);
    self->first_relay = g_strdup(relay);
    self->settle_id = g_timeout_add(self->settle_ms, on_settle, self);
    /* Watch the wallet's own relay during the settle window too (in
     * addition to ours, never instead) — only for an answer we would
     * accept, so an ignored one cannot make the agent dial anywhere. */
    if (hint) add_relay(self, hint);
    return;
  }
  if (g_strcmp0(self->first_author, wallet) != 0)
    finish_failed(self, NWA_WALLET_AUTH_CONFLICT,
                  "two different wallets answered the connection request; nothing was connected");
}

static void
on_frame(NostrPublishTransport *t, const gchar *kind_hint, const gchar *envelope, gpointer data)
{
  NwaWalletAuth *self = data;
  if (self->done || g_strcmp0(kind_hint, "EVENT") != 0) return;
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, envelope, -1, NULL) || !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(p)))
    return;
  JsonArray *a = json_node_get_array(json_parser_get_root(p));
  if (json_array_get_length(a) < 3 || g_strcmp0(json_array_get_string_element(a, 1), SUB_AUTH) != 0) return;
  JsonNode *evn = json_array_get_element(a, 2);
  if (!JSON_NODE_HOLDS_OBJECT(evn)) return;
  g_autofree gchar *ej = json_to_string(evn, FALSE);
  NostrEvent *ev = nostr_event_new();
  if (ev && nostr_event_deserialize(ev, ej) == 0 && nostr_event_check_signature(ev))
    handle_candidate(self, ev, nostr_publish_transport_get_url(t));
  if (ev) nostr_event_free(ev);
}

static void
on_state(NostrPublishTransport *t, gboolean connected, const GError *error, gpointer data)
{
  (void)error;
  NwaWalletAuth *self = data;
  if (connected && !self->done) subscribe(self, t);
}

static void
add_relay(NwaWalletAuth *self, const gchar *url)
{
  for (guint i = 0; i < self->transports->len; i++)
    if (g_strcmp0(nostr_publish_transport_get_url(g_ptr_array_index(self->transports, i)), url) == 0)
      return;
  if (self->transports->len >= NWA_WALLET_AUTH_MAX_RELAYS + 1) return; /* ours + one hint */
  NostrPublishTransport *t = self->factory ? self->factory(url, self->factory_data)
                                           : nostr_publish_transport_new_websocket(url);
  if (!t) return;
  nostr_publish_transport_set_listener(t, on_frame, self);
  nostr_publish_transport_set_state_callback(t, on_state, self);
  g_ptr_array_add(self->transports, t);
  nostr_publish_transport_connect_async(t);
}

/* ---- public ---- */

NwaWalletAuth *
nwa_wallet_auth_new(const gchar *const *relays, const gchar *name,
                    NwaTransportFactory factory, gpointer factory_data, GError **error)
{
  g_autoptr(GPtrArray) rl = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; relays && relays[i]; i++) {
    if (!relay_ok(relays[i])) {
      g_set_error(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "not a ws:// or wss:// relay: %s", relays[i]);
      return NULL;
    }
    gboolean dup = FALSE;
    for (guint j = 0; j < rl->len; j++) dup |= g_str_equal(g_ptr_array_index(rl, j), relays[i]);
    if (!dup) g_ptr_array_add(rl, g_strdup(relays[i]));
  }
  if (rl->len == 0 || rl->len > NWA_WALLET_AUTH_MAX_RELAYS) {
    g_set_error(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "need 1 to %d relays", NWA_WALLET_AUTH_MAX_RELAYS);
    return NULL;
  }
  if (!name || !*name || g_utf8_strlen(name, -1) > 64 || !g_utf8_validate(name, -1, NULL)) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "name must be 1 to 64 characters");
    return NULL;
  }
  char *sk = nostr_key_generate_private();
  char *pk = sk ? nostr_key_get_public(sk) : NULL;
  if (!pk) {
    if (sk) { memset(sk, 0, strlen(sk)); free(sk); }
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_FAILED, "could not generate a key");
    return NULL;
  }
  NwaWalletAuth *self = g_object_new(NWA_TYPE_WALLET_AUTH, NULL);
  self->secret = g_strdup(sk);
  memset(sk, 0, strlen(sk));
  free(sk);
  self->client_pk = g_strdup(pk);
  free(pk);
  guint8 st[16];
  for (guint i = 0; i < 4; i++) {
    guint32 r = g_random_int();
    memcpy(st + 4 * i, &r, 4);
  }
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < 16; i++) g_string_append_printf(s, "%02x", st[i]);
  self->state = g_string_free(s, FALSE);
  g_ptr_array_add(rl, NULL);
  self->relays = (gchar **)g_ptr_array_steal(rl, NULL);
  self->factory = factory;
  self->factory_data = factory_data;

  GString *u = g_string_new(NULL);
  g_string_append_printf(u, "nostr+walletauth://%s?", self->client_pk);
  for (guint i = 0; self->relays[i]; i++) {
    g_autofree gchar *e = g_uri_escape_string(self->relays[i], NULL, FALSE);
    g_string_append_printf(u, "relay=%s&", e);
  }
  g_autofree gchar *en = g_uri_escape_string(name, NULL, FALSE);
  g_autofree gchar *em = g_uri_escape_string(REQUEST_METHODS, NULL, FALSE);
  g_autofree gchar *et = g_uri_escape_string(NOTIFICATION_TYPES, NULL, FALSE);
  g_string_append_printf(u, "name=%s&request_methods=%s&notification_types=%s&pubkey=%s&state=%s",
                         en, em, et, self->client_pk, self->state);
  self->uri = g_string_free(u, FALSE);
  return self;
}

const gchar *nwa_wallet_auth_get_uri(NwaWalletAuth *self) { return self->uri; }
const gchar *nwa_wallet_auth_get_client_pubkey(NwaWalletAuth *self) { return self->client_pk; }
const gchar *nwa_wallet_auth_get_state(NwaWalletAuth *self) { return self->state; }

void
nwa_wallet_auth_set_timing(NwaWalletAuth *self, guint settle_ms, guint timeout_s)
{
  self->settle_ms = settle_ms;
  if (timeout_s) self->timeout_s = timeout_s;
}

void
nwa_wallet_auth_start(NwaWalletAuth *self)
{
  if (self->started) return;
  self->started = TRUE;
  self->started_at = (gint64)time(NULL);
  self->timeout_id = g_timeout_add_seconds(self->timeout_s, on_timeout, self);
  for (guint i = 0; self->relays[i]; i++) add_relay(self, self->relays[i]);
}

void
nwa_wallet_auth_stop(NwaWalletAuth *self)
{
  if (self->started && !self->done)
    finish_failed(self, NWA_WALLET_AUTH_STOPPED, "cancelled");
}

gchar *
nwa_wallet_auth_build_nwc_uri(NwaWalletAuth *self, const gchar *wallet_pk, const gchar *relay)
{
  g_autofree gchar *er = g_uri_escape_string(relay, NULL, FALSE);
  return g_strdup_printf("nostr+walletconnect://%s?relay=%s&secret=%s", wallet_pk, er, self->secret);
}

static void
nwa_wallet_auth_dispose(GObject *obj)
{
  NwaWalletAuth *self = NWA_WALLET_AUTH(obj);
  self->done = TRUE; /* no signals from dispose */
  clear_timers(self);
  disconnect_all(self);
  G_OBJECT_CLASS(nwa_wallet_auth_parent_class)->dispose(obj);
}

static void
nwa_wallet_auth_finalize(GObject *obj)
{
  NwaWalletAuth *self = NWA_WALLET_AUTH(obj);
  wipe_free(self->secret);
  g_free(self->client_pk);
  g_free(self->state);
  g_strfreev(self->relays);
  g_free(self->uri);
  g_free(self->first_author);
  g_free(self->first_relay);
  g_free(self->res_pk);
  g_free(self->res_relay);
  g_free(self->res_msg);
  g_ptr_array_unref(self->transports);
  G_OBJECT_CLASS(nwa_wallet_auth_parent_class)->finalize(obj);
}

static void
nwa_wallet_auth_class_init(NwaWalletAuthClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = nwa_wallet_auth_dispose;
  oc->finalize = nwa_wallet_auth_finalize;
  signals[SIGNAL_AUTHORIZED] =
    g_signal_new("authorized", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
  signals[SIGNAL_FAILED] =
    g_signal_new("failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_STRING);
}

static void
nwa_wallet_auth_init(NwaWalletAuth *self)
{
  self->transports = g_ptr_array_new_with_free_func((GDestroyNotify)nostr_publish_transport_unref);
  self->settle_ms = 3000;
  self->timeout_s = 600;
}
