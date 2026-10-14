#pragma once
#include <gio/gio.h>
#include <secure_buf.h>
#include <string.h>

/* Only our owned payloads are mutable. Never overwrite a borrowed GDBus
 * message body: its immutable storage can be shared with the transport. */
typedef struct { gpointer data; gsize size; } GrottoSecretPayload;
static inline void grotto_secret_payload_free(gpointer data) {
  GrottoSecretPayload *p = data;
  secure_wipe(p->data, p->size); g_free(p->data); g_free(p);
}
static inline GVariant *grotto_secret_tuple(const gchar *const *strings, gsize count) {
  GVariant **children = g_new(GVariant *, count);
  for (gsize i = 0; i < count; i++) {
    GrottoSecretPayload *p = g_new(GrottoSecretPayload, 1);
    p->size = strlen(strings[i]) + 1; p->data = g_memdup2(strings[i], p->size);
    children[i] = g_variant_new_from_data(G_VARIANT_TYPE_STRING, p->data, p->size, TRUE, grotto_secret_payload_free, p);
  }
  GVariant *tuple = g_variant_ref_sink(g_variant_new_tuple(children, count));
  g_free(children);
  GrottoSecretPayload *p = g_new(GrottoSecretPayload, 1);
  p->size = g_variant_get_size(tuple); p->data = g_malloc(p->size);
  g_variant_store(tuple, p->data);
  GVariant *owned = g_variant_new_from_data(g_variant_get_type(tuple), p->data, p->size, TRUE, grotto_secret_payload_free, p);
  g_variant_unref(tuple);
  return owned;
}
