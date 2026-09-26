/* nd-signer.c - Signer bridge for the nostr-dav publish worker
 *
 * SPDX-License-Identifier: MIT
 *
 * A tiny wrapper over a vtable. Reference-counted because the publisher
 * hands the signer around between the main context and its worker
 * thread. The DBus implementation lives in nd-signer-dbus.c.
 */

#include "nd-signer.h"

#include <string.h>

G_DEFINE_QUARK(nd-signer-error-quark, nd_signer_error)

struct _NdSigner {
  int             ref_count;
  NdSignerVTable  vtable;
  gpointer        user_data;
};

NdSigner *
nd_signer_new_from_vtable(const NdSignerVTable *vtable, gpointer user_data)
{
  g_return_val_if_fail(vtable != NULL, NULL);
  g_return_val_if_fail(vtable->sign_event_json != NULL, NULL);

  NdSigner *self = g_new0(NdSigner, 1);
  self->ref_count = 1;
  self->vtable    = *vtable;
  self->user_data = user_data;
  return self;
}

NdSigner *
nd_signer_ref(NdSigner *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  g_atomic_int_inc(&self->ref_count);
  return self;
}

void
nd_signer_unref(NdSigner *self)
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
nd_signer_sign_event_json(NdSigner     *self,
                          const gchar  *unsigned_json,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_return_val_if_fail(self != NULL, NULL);
  g_return_val_if_fail(unsigned_json != NULL, NULL);

  return self->vtable.sign_event_json(self->user_data, unsigned_json,
                                      cancellable, error);
}
