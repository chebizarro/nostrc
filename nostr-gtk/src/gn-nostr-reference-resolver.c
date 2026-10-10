/* GnNostrReferenceResolver (nostrc-8xfib.6): see the header for the
 * local-only / explicit-fetch contract. */
#include <nostr-gtk-1.0/gn-nostr-reference-resolver.h>
#include "gn-portable-i18n-private.h"

G_DEFINE_INTERFACE(GnNostrReferenceResolver, gn_nostr_reference_resolver, G_TYPE_OBJECT)

static void
gn_nostr_reference_resolver_default_init(GnNostrReferenceResolverInterface *iface)
{
  (void)iface;
}

gchar *
gn_nostr_reference_resolver_lookup_local(GnNostrReferenceResolver *self,
                                         const GnNostrReference *reference)
{
  g_return_val_if_fail(GN_IS_NOSTR_REFERENCE_RESOLVER(self), NULL);
  GnNostrReferenceResolverInterface *iface = GN_NOSTR_REFERENCE_RESOLVER_GET_IFACE(self);
  return reference && iface->lookup_local ? iface->lookup_local(self, reference) : NULL;
}

gchar *
gn_nostr_reference_resolver_display_name(GnNostrReferenceResolver *self,
                                         const gchar *pubkey_hex)
{
  g_return_val_if_fail(GN_IS_NOSTR_REFERENCE_RESOLVER(self), NULL);
  GnNostrReferenceResolverInterface *iface = GN_NOSTR_REFERENCE_RESOLVER_GET_IFACE(self);
  return pubkey_hex && iface->display_name ? iface->display_name(self, pubkey_hex) : NULL;
}

gboolean
gn_nostr_reference_resolver_can_fetch(GnNostrReferenceResolver *self)
{
  g_return_val_if_fail(GN_IS_NOSTR_REFERENCE_RESOLVER(self), FALSE);
  GnNostrReferenceResolverInterface *iface = GN_NOSTR_REFERENCE_RESOLVER_GET_IFACE(self);
  return iface->fetch_async && iface->fetch_finish && iface->can_fetch &&
         iface->can_fetch(self);
}

void
gn_nostr_reference_resolver_fetch_async(GnNostrReferenceResolver *self,
                                        const GnNostrReference *reference,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data)
{
  g_return_if_fail(GN_IS_NOSTR_REFERENCE_RESOLVER(self));
  g_return_if_fail(reference != NULL);
  GnNostrReferenceResolverInterface *iface = GN_NOSTR_REFERENCE_RESOLVER_GET_IFACE(self);
  if (!iface->fetch_async || !iface->fetch_finish) {
    g_task_report_new_error(self, callback, user_data,
                            gn_nostr_reference_resolver_fetch_async,
                            G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            _("This resolver cannot fetch notes"));
    return;
  }
  iface->fetch_async(self, reference, cancellable, callback, user_data);
}

gchar *
gn_nostr_reference_resolver_fetch_finish(GnNostrReferenceResolver *self,
                                         GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(GN_IS_NOSTR_REFERENCE_RESOLVER(self), NULL);
  if (g_async_result_is_tagged(result, gn_nostr_reference_resolver_fetch_async))
    return g_task_propagate_pointer(G_TASK(result), error);
  return GN_NOSTR_REFERENCE_RESOLVER_GET_IFACE(self)->fetch_finish(self, result, error);
}

void
gn_nostr_resolved_note_free(GnNostrResolvedNote *note)
{
  if (!note) return;
  gn_nostr_event_info_free(note->event);
  g_free(note->event_json);
  g_free(note->author_name);
  g_free(note);
}

GnNostrResolvedNote *
gn_nostr_resolved_note_new(const GnNostrReference *reference, const gchar *event_json,
                           GnNostrReferenceResolver *resolver)
{
  if (!reference || !event_json) return NULL;
  g_autoptr(GnNostrEventInfo) info = gn_nostr_event_parse(event_json, NULL);
  if (!info || !gn_nostr_reference_matches_event_info(reference, info)) return NULL;
  GnNostrResolvedNote *note = g_new0(GnNostrResolvedNote, 1);
  note->event = g_steal_pointer(&info);
  note->event_json = g_strdup(event_json);
  if (resolver)
    note->author_name = gn_nostr_reference_resolver_display_name(resolver, note->event->pubkey);
  return note;
}

GnNostrResolvedNote *
gn_nostr_reference_resolver_resolve_local(GnNostrReferenceResolver *self,
                                          const GnNostrReference *reference)
{
  g_return_val_if_fail(GN_IS_NOSTR_REFERENCE_RESOLVER(self), NULL);
  g_autofree gchar *json = gn_nostr_reference_resolver_lookup_local(self, reference);
  return gn_nostr_resolved_note_new(reference, json, self);
}
