/* nostr-publish-signer.c - Signer abstraction for libnostr-publish
 *
 * SPDX-License-Identifier: MIT
 *
 * A tiny reference-counted wrapper over a vtable, plus the NIP-42 AUTH
 * event builder. The D-Bus implementation lives in
 * nostr-publish-signer-dbus.c. Moved from gnome/nostr-dav (nd-signer.c).
 */

#include "nostr-publish-signer.h"

#include <json-glib/json-glib.h>

#include <string.h>

/* NIP-42 client-authentication event kind. */
#define NP_NIP42_AUTH_KIND 22242

G_DEFINE_QUARK(nostr-publish-signer-error-quark, nostr_publish_signer_error)

struct _NostrPublishSigner {
  int             ref_count;
  NostrPublishSignerVTable  vtable;
  gpointer        user_data;
};

NostrPublishSigner *
nostr_publish_signer_new_from_vtable(const NostrPublishSignerVTable *vtable, gpointer user_data)
{
  g_return_val_if_fail(vtable != NULL, NULL);
  g_return_val_if_fail(vtable->sign_event_json != NULL, NULL);

  NostrPublishSigner *self = g_new0(NostrPublishSigner, 1);
  self->ref_count = 1;
  self->vtable    = *vtable;
  self->user_data = user_data;
  return self;
}

NostrPublishSigner *
nostr_publish_signer_ref(NostrPublishSigner *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  g_atomic_int_inc(&self->ref_count);
  return self;
}

void
nostr_publish_signer_unref(NostrPublishSigner *self)
{
  if (self == NULL)
    return;
  if (!g_atomic_int_dec_and_test(&self->ref_count))
    return;
  if (self->vtable.user_data_destroy != NULL && self->user_data != NULL)
    self->vtable.user_data_destroy(self->user_data);
  g_free(self);
}

gchar *
nostr_publish_signer_sign_event_json(NostrPublishSigner     *self,
                          const gchar  *unsigned_json,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_return_val_if_fail(self != NULL, NULL);
  g_return_val_if_fail(unsigned_json != NULL, NULL);

  return self->vtable.sign_event_json(self->user_data, unsigned_json,
                                      cancellable, error);
}

gboolean
nostr_publish_signer_error_is_permanent(const GError *error)
{
  return g_error_matches(error, NOSTR_PUBLISH_SIGNER_ERROR,
                         NOSTR_PUBLISH_SIGNER_ERROR_DENIED) ||
         g_error_matches(error, NOSTR_PUBLISH_SIGNER_ERROR,
                         NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED);
}

gchar *
nostr_publish_signer_sign_auth_event(NostrPublishSigner *self,
                                     const gchar        *relay_url,
                                     const gchar        *challenge,
                                     gint64              created_at,
                                     GError            **error)
{
  g_return_val_if_fail(self != NULL, NULL);
  g_return_val_if_fail(challenge != NULL, NULL);

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, NP_NIP42_AUTH_KIND);
  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, created_at);
  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, "");
  json_builder_set_member_name(b, "tags");
  json_builder_begin_array(b);
    json_builder_begin_array(b);
      json_builder_add_string_value(b, "relay");
      json_builder_add_string_value(b, relay_url ? relay_url : "");
    json_builder_end_array(b);
    json_builder_begin_array(b);
      json_builder_add_string_value(b, "challenge");
      json_builder_add_string_value(b, challenge);
    json_builder_end_array(b);
  json_builder_end_array(b);
  json_builder_end_object(b);

  g_autoptr(JsonNode) root = json_builder_get_root(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  g_autofree gchar *unsigned_json = json_generator_to_data(gen, NULL);

  return nostr_publish_signer_sign_event_json(self, unsigned_json, NULL, error);
}
