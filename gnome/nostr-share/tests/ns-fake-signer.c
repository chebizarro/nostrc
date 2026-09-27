/* ns-fake-signer.c - In-process org.nostr.Signer for nostr-share tests
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-fake-signer.h"

#include <nostr/nip19/nip19.h>
#include <nostr/nip44/nip44.h>
#include "nostr-event.h"
#include "nostr-keys.h"

#include <stdlib.h>
#include <string.h>

static const gchar SIGNER_XML[] =
  "<node><interface name='org.nostr.Signer'>"
  "<method name='GetPublicKey'><arg type='s' direction='out'/></method>"
  "<method name='SignEvent'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
  "<method name='NIP44Encrypt'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
  "</interface></node>";

static void
unhex32(const gchar *hex, guint8 out[32])
{
  for (int i = 0; i < 32; i++)
    out[i] = (guint8)((g_ascii_xdigit_value(hex[2 * i]) << 4) |
                      g_ascii_xdigit_value(hex[2 * i + 1]));
}

struct _NsFakeSigner {
  gchar        *address;
  gchar        *sk, *pk, *npub;
  GThread      *thread;
  GMainContext *ctx;
  GMainLoop    *loop;
  GMutex        lock;
  GCond         cond;
  gboolean      ready;
  guint         sign_calls;
};

static void
signer_call(GDBusConnection *c, const gchar *sender, const gchar *path,
            const gchar *iface, const gchar *method, GVariant *params,
            GDBusMethodInvocation *inv, gpointer ud)
{
  (void)c; (void)sender; (void)path; (void)iface;
  NsFakeSigner *s = ud;
  if (g_str_equal(method, "GetPublicKey")) {
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", s->npub));
    return;
  }
  if (g_str_equal(method, "NIP44Encrypt")) {
    const gchar *plaintext = NULL, *peer = NULL, *identity = NULL;
    g_variant_get(params, "(&s&s&s)", &plaintext, &peer, &identity);
    guint8 sk[32], pk[32];
    unhex32(s->sk, sk);
    unhex32(peer, pk);
    char *out = NULL;
    if (strlen(peer) != 64 ||
        nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)plaintext, strlen(plaintext), &out) != 0) {
      g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.InvalidInput",
                                                 "bad peer");
      return;
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", out));
    free(out);
    return;
  }
  const gchar *json = NULL, *identity = NULL, *app_id = NULL;
  g_variant_get(params, "(&s&s&s)", &json, &identity, &app_id);
  NostrEvent *ev = nostr_event_new();
  if (nostr_event_deserialize_compact(ev, json, NULL) != 1) {
    nostr_event_free(ev);
    g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Signer.Error.InvalidInput",
                                               "not an event");
    return;
  }
  nostr_event_set_pubkey(ev, s->pk);
  if (nostr_event_get_created_at(ev) == 0)
    nostr_event_set_created_at(ev, g_get_real_time() / G_USEC_PER_SEC);
  nostr_event_sign(ev, s->sk);
  char *out = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  g_mutex_lock(&s->lock);
  s->sign_calls++;
  g_mutex_unlock(&s->lock);
  g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", out));
  free(out);
}

static void
on_acquired(GDBusConnection *c, const gchar *name, gpointer ud)
{
  (void)c; (void)name;
  NsFakeSigner *s = ud;
  g_mutex_lock(&s->lock);
  s->ready = TRUE;
  g_cond_broadcast(&s->cond);
  g_mutex_unlock(&s->lock);
}

static gpointer
signer_thread(gpointer p)
{
  NsFakeSigner *s = p;
  g_main_context_push_thread_default(s->ctx);
  GDBusConnection *c = g_dbus_connection_new_for_address_sync(
    s->address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL, NULL, NULL);
  g_assert_nonnull(c);
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(SIGNER_XML, NULL);
  static const GDBusInterfaceVTable vt = { signer_call, NULL, NULL, { 0 } };
  guint reg = g_dbus_connection_register_object(c, "/org/nostr/signer", node->interfaces[0],
                                                &vt, s, NULL, NULL);
  guint own = g_bus_own_name_on_connection(c, "org.nostr.Signer", G_BUS_NAME_OWNER_FLAGS_NONE,
                                           on_acquired, NULL, s, NULL);
  g_main_loop_run(s->loop);
  g_bus_unown_name(own);
  g_dbus_connection_unregister_object(c, reg);
  g_dbus_connection_flush_sync(c, NULL, NULL);
  g_dbus_connection_close_sync(c, NULL, NULL);
  g_object_unref(c);
  g_dbus_node_info_unref(node);
  while (g_main_context_iteration(s->ctx, FALSE))
    ;
  g_main_context_pop_thread_default(s->ctx);
  return NULL;
}

NsFakeSigner *
ns_fake_signer_start(void)
{
  NsFakeSigner *s = g_new0(NsFakeSigner, 1);
  s->address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert_nonnull(s->address);
  s->sk = nostr_key_generate_private();
  s->pk = nostr_key_get_public(s->sk);
  guint8 raw[32];
  for (int i = 0; i < 32; i++) {
    unsigned b = 0;
    sscanf(s->pk + 2 * i, "%2x", &b);
    raw[i] = (guint8)b;
  }
  char *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(raw, &npub), ==, 0);
  s->npub = g_strdup(npub);
  free(npub);
  g_mutex_init(&s->lock);
  g_cond_init(&s->cond);
  s->ctx = g_main_context_new();
  s->loop = g_main_loop_new(s->ctx, FALSE);
  s->thread = g_thread_new("fake-signer", signer_thread, s);
  g_mutex_lock(&s->lock);
  while (!s->ready)
    g_cond_wait(&s->cond, &s->lock);
  g_mutex_unlock(&s->lock);
  return s;
}

void
ns_fake_signer_stop(NsFakeSigner *s)
{
  if (s == NULL)
    return;
  g_main_loop_quit(s->loop);
  g_thread_join(s->thread);
  g_main_loop_unref(s->loop);
  g_main_context_unref(s->ctx);
  g_mutex_clear(&s->lock);
  g_cond_clear(&s->cond);
  free(s->sk);
  free(s->pk);
  g_free(s->npub);
  g_free(s->address);
  g_free(s);
}

const gchar *ns_fake_signer_sk(NsFakeSigner *s) { return s->sk; }
const gchar *ns_fake_signer_pk(NsFakeSigner *s) { return s->pk; }

guint
ns_fake_signer_sign_calls(NsFakeSigner *s)
{
  g_mutex_lock(&s->lock);
  guint n = s->sign_calls;
  g_mutex_unlock(&s->lock);
  return n;
}
