#include "grotto_clients.h"
#include "signer_caller.h"
#include "../../../libnostr/include/nostrc-profile-guard.h"
#include <glib/gstdio.h>
#include <string.h>

struct _GrottoClients { GKeyFile *records; gchar *path; guint64 revision; };
static gboolean invalid(GError **error, const gchar *message) {
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE;
}
static gboolean text_ok(const gchar *s, gsize max, gboolean empty) {
  if (!s || (!empty && !*s) || strlen(s) > max || !g_utf8_validate(s, -1, NULL)) return FALSE;
  for (const gchar *p = s; *p; p = g_utf8_next_char(p))
    if (g_unichar_iscntrl(g_utf8_get_char(p))) return FALSE;
  return TRUE;
}
gboolean grotto_clients_principal_valid(const gchar *s) {
  if (!text_ok(s, 4096, FALSE)) return FALSE;
  if (g_str_has_prefix(s, "nip46:")) {
    g_autoptr(SignerCaller) c = signer_caller_for_nip46(s + 6);
    return c && !strcmp(s, c->principal);
  }
  if (g_str_has_prefix(s, "http:" ) || g_str_has_prefix(s, "https:"))
    return signer_caller_is_web_origin(s);
  return !strpbrk(s, "=[]|\r\n") && !g_ascii_isspace(*s) && !g_ascii_isspace(s[strlen(s) - 1]);
}
static gboolean transport_ok(const gchar *s) {
  return !g_strcmp0(s, "DBUS") || !g_strcmp0(s, "NIP5F") || !g_strcmp0(s, "NIP46") || !g_strcmp0(s, "MANAGEMENT");
}
static gchar *record_id(const gchar *principal) {
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, principal, -1);
}
static GKeyFile *copy_records(GrottoClients *c) {
  gsize size; g_autofree gchar *data = g_key_file_to_data(c->records, &size, NULL);
  GKeyFile *next = g_key_file_new();
  g_key_file_load_from_data(next, data, size, G_KEY_FILE_NONE, NULL); return next;
}
static gboolean commit(GrottoClients *c, GKeyFile *next, GError **error) {
  g_autofree gchar *dir = g_path_get_dirname(c->path);
  if (nostrc_profile_guard_refuses_path(c->path) || nostrc_profile_guard_refuses_path(dir)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "client store refuses real profile under test"); return FALSE;
  }
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "cannot create client directory"); return FALSE;
  }
  gsize size; g_autofree gchar *data = g_key_file_to_data(next, &size, error);
  if (!data || !g_file_set_contents_full(c->path, data, size,
      G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error)) return FALSE;
  g_key_file_unref(c->records); c->records = g_key_file_ref(next); c->revision++; return TRUE;
}
GrottoClients *grotto_clients_new(const gchar *config_home, GError **error) {
  g_autoptr(GrottoClients) c = g_new0(GrottoClients, 1);
  c->records = g_key_file_new(); c->revision = 1;
  c->path = g_build_filename(config_home ? config_home : g_get_user_config_dir(), "grotto", "clients.ini", NULL);
  if (nostrc_profile_guard_refuses_path(c->path)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "client store refuses real profile under test"); return NULL;
  }
  g_autoptr(GError) local = NULL;
  if (!g_key_file_load_from_file(c->records, c->path, G_KEY_FILE_NONE, &local)) {
    if (!g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) { g_propagate_error(error, g_steal_pointer(&local)); return NULL; }
    g_key_file_set_integer(c->records, "clients", "version", 1);
    return g_steal_pointer(&c);
  }
  if (g_key_file_get_integer(c->records, "clients", "version", NULL) != 1) {
    invalid(error, "unsupported clients.ini version"); return NULL;
  }
  g_auto(GStrv) groups = g_key_file_get_groups(c->records, NULL);
  for (guint i = 0; groups[i]; i++) {
    const gchar *id = groups[i]; if (!strcmp(id, "clients")) continue;
    g_autofree gchar *p = g_key_file_get_string(c->records, id, "principal", NULL);
    g_autofree gchar *t = g_key_file_get_string(c->records, id, "transport", NULL);
    g_autofree gchar *identity = g_key_file_get_string(c->records, id, "identity", NULL);
    g_autofree gchar *label = g_key_file_get_string(c->records, id, "label", NULL);
    g_autofree gchar *claimed = g_key_file_get_string(c->records, id, "claimed-name", NULL);
    g_autofree gchar *digest = p ? record_id(p) : NULL;
    gint tier = g_key_file_get_integer(c->records, id, "tier", &local);
    if (local || !grotto_clients_principal_valid(p) || g_strcmp0(id, digest) || !transport_ok(t) ||
        !text_ok(identity, 4096, TRUE) || !text_ok(label, 160, TRUE) || !text_ok(claimed, 512, TRUE) || tier < 0 || tier > 2) {
      invalid(error, "invalid client record"); return NULL;
    }
    const gchar *numbers[] = {"first-consented-at", "last-used-at"};
    for (guint j = 0; j < G_N_ELEMENTS(numbers); j++) {
      g_key_file_get_uint64(c->records, id, numbers[j], &local);
      if (local) { invalid(error, "invalid client timestamp"); return NULL; }
    }
    gboolean paired = g_key_file_get_boolean(c->records, id, "paired", &local);
    guint64 first = g_key_file_get_uint64(c->records, id, "first-consented-at", NULL);
    if (local || (tier != SIGNER_TRUST_LOW && !first) ||
        (paired && (!first || strcmp(t, "NIP46") || !g_str_has_prefix(p, "nip46:")))) {
      invalid(error, "invalid client admission"); return NULL;
    }
    g_auto(GStrv) relays = g_key_file_get_string_list(c->records, id, "relays", NULL, &local);
    if (local) { invalid(error, "invalid client relays"); return NULL; }
    for (guint j = 0; relays && relays[j]; j++)
      if (!text_ok(relays[j], 2048, FALSE)) { invalid(error, "invalid client relay"); return NULL; }
  }
  return g_steal_pointer(&c);
}
void grotto_clients_free(GrottoClients *c) {
  if (!c) return;
  g_clear_pointer(&c->records, g_key_file_unref); g_free(c->path); g_free(c);
}
guint64 grotto_clients_revision(GrottoClients *c) { return c->revision; }
gboolean grotto_clients_known(GrottoClients *c, const gchar *principal) {
  if (!principal || !grotto_clients_principal_valid(principal)) return FALSE;
  g_autofree gchar *id = record_id(principal);
  return g_key_file_has_group(c->records, id);
}
SignerTrustTier grotto_clients_trust(const gchar *principal, gboolean *consented, gpointer data) {
  GrottoClients *c = data; *consented = FALSE;
  if (!principal || !grotto_clients_principal_valid(principal)) return SIGNER_TRUST_LOW;
  g_autofree gchar *id = record_id(principal);
  if (!g_key_file_has_group(c->records, id)) return SIGNER_TRUST_LOW;
  *consented = g_key_file_get_uint64(c->records, id, "first-consented-at", NULL) != 0;
  return g_key_file_get_integer(c->records, id, "tier", NULL);
}
gboolean grotto_clients_record(GrottoClients *c, const gchar *principal, const gchar *transport,
    const gchar *claimed, const gchar *identity, gboolean consented, gboolean used, GError **error) {
  if (!grotto_clients_principal_valid(principal) || (transport && !transport_ok(transport)) ||
      (claimed && !text_ok(claimed, 512, TRUE)) || (identity && !text_ok(identity, 4096, TRUE)))
    return invalid(error, "invalid client metadata");
  g_autofree gchar *id = record_id(principal); g_autoptr(GKeyFile) next = copy_records(c);
  if (!g_key_file_has_group(next, id)) {
    g_key_file_set_string(next, id, "principal", principal);
    g_key_file_set_string(next, id, "label", ""); g_key_file_set_integer(next, id, "tier", SIGNER_TRUST_LOW);
    g_key_file_set_string(next, id, "transport", g_str_has_prefix(principal, "nip46:") ? "NIP46" : "DBUS");
    g_key_file_set_string(next, id, "claimed-name", ""); g_key_file_set_string(next, id, "identity", "");
    g_key_file_set_boolean(next, id, "paired", FALSE);
    g_key_file_set_string_list(next, id, "relays", NULL, 0);
    g_key_file_set_uint64(next, id, "first-consented-at", 0); g_key_file_set_uint64(next, id, "last-used-at", 0);
  }
  if (transport) g_key_file_set_string(next, id, "transport", transport);
  if (claimed) g_key_file_set_string(next, id, "claimed-name", claimed);
  if (identity) g_key_file_set_string(next, id, "identity", identity);
  guint64 now = g_get_real_time() / G_USEC_PER_SEC;
  if (consented && !g_key_file_get_uint64(next, id, "first-consented-at", NULL)) g_key_file_set_uint64(next, id, "first-consented-at", now);
  if (used) g_key_file_set_uint64(next, id, "last-used-at", now);
  return commit(c, next, error);
}
static gboolean existing(GrottoClients *c, const gchar *principal, gchar **id, GError **error) {
  if (!grotto_clients_principal_valid(principal)) return invalid(error, "invalid principal");
  *id = record_id(principal);
  if (g_key_file_has_group(c->records, *id)) return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "unknown client"); return FALSE;
}
gboolean grotto_clients_set_label(GrottoClients *c, const gchar *p, const gchar *label, GError **error) {
  g_autofree gchar *id = NULL;
  if (!text_ok(label, 160, TRUE)) return invalid(error, "invalid client label");
  if (!existing(c, p, &id, error)) return FALSE;
  g_autoptr(GKeyFile) next = copy_records(c); g_key_file_set_string(next, id, "label", label); return commit(c, next, error);
}
gboolean grotto_clients_set_tier(GrottoClients *c, const gchar *p, SignerTrustTier tier, GError **error) {
  g_autofree gchar *id = NULL;
  if (tier < SIGNER_TRUST_LOW || tier > SIGNER_TRUST_FULL) return invalid(error, "invalid trust tier");
  if (!existing(c, p, &id, error)) return FALSE;
  if (!g_key_file_get_uint64(c->records, id, "first-consented-at", NULL) && tier != SIGNER_TRUST_LOW)
    return invalid(error, "trust requires explicit client consent");
  g_autoptr(GKeyFile) next = copy_records(c); g_key_file_set_integer(next, id, "tier", tier); return commit(c, next, error);
}
gchar *grotto_clients_paired_identity(GrottoClients *c, const gchar *principal) {
  if (!c || !grotto_clients_principal_valid(principal) || !g_str_has_prefix(principal, "nip46:")) return NULL;
  g_autofree gchar *id = record_id(principal);
  if (!g_key_file_get_boolean(c->records, id, "paired", NULL) ||
      !g_key_file_get_uint64(c->records, id, "first-consented-at", NULL)) return NULL;
  gchar *identity = g_key_file_get_string(c->records, id, "identity", NULL);
  if (identity && *identity) return identity;
  g_free(identity); return NULL;
}
gboolean grotto_clients_set_pairing(GrottoClients *c, const gchar *p, gboolean paired,
    const gchar *const *relays, GError **error) {
  g_autofree gchar *id = NULL;
  if (!existing(c, p, &id, error)) return FALSE;
  if (!g_str_has_prefix(p, "nip46:") || !g_key_file_get_uint64(c->records, id, "first-consented-at", NULL))
    return invalid(error, "pairing requires explicit remote consent");
  gsize n = relays ? g_strv_length((GStrv)relays) : 0;
  if (n > 64) return invalid(error, "too many client relays");
  for (gsize i = 0; i < n; i++) if (!text_ok(relays[i], 2048, FALSE)) return invalid(error, "invalid client relay");
  g_autoptr(GKeyFile) next = copy_records(c);
  g_key_file_set_boolean(next, id, "paired", paired); g_key_file_set_string_list(next, id, "relays", relays, n);
  return commit(c, next, error);
}
gboolean grotto_clients_remove(GrottoClients *c, const gchar *p, GError **error) {
  if (!grotto_clients_principal_valid(p)) return invalid(error, "invalid principal");
  g_autofree gchar *id = record_id(p); if (!g_key_file_has_group(c->records, id)) return TRUE;
  g_autoptr(GKeyFile) next = copy_records(c); g_key_file_remove_group(next, id, NULL); return commit(c, next, error);
}
GVariant *grotto_clients_list(GrottoClients *c) {
  GVariantBuilder list; g_variant_builder_init(&list, G_VARIANT_TYPE("aa{sv}"));
  g_auto(GStrv) groups = g_key_file_get_groups(c->records, NULL);
  for (guint i = 0; groups[i]; i++) {
    const gchar *id = groups[i]; if (!strcmp(id, "clients")) continue;
    GVariantBuilder row; g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
    const gchar *strings[] = {"principal", "transport", "label", "claimed-name", "identity"};
    for (guint j = 0; j < G_N_ELEMENTS(strings); j++) {
      g_autofree gchar *v = g_key_file_get_string(c->records, id, strings[j], NULL);
      g_variant_builder_add(&row, "{sv}", strings[j], g_variant_new_string(v));
    }
    gint tier = g_key_file_get_integer(c->records, id, "tier", NULL);
    const gchar *tiers[] = {"low", "medium", "full"};
    g_variant_builder_add(&row, "{sv}", "trust-tier", g_variant_new_string(tiers[tier]));
    g_variant_builder_add(&row, "{sv}", "paired", g_variant_new_boolean(g_key_file_get_boolean(c->records, id, "paired", NULL)));
    const gchar *numbers[] = {"first-consented-at", "last-used-at"};
    for (guint j = 0; j < G_N_ELEMENTS(numbers); j++)
      g_variant_builder_add(&row, "{sv}", numbers[j], g_variant_new_uint64(g_key_file_get_uint64(c->records, id, numbers[j], NULL)));
    g_auto(GStrv) relays = g_key_file_get_string_list(c->records, id, "relays", NULL, NULL);
    g_variant_builder_add(&row, "{sv}", "relays", g_variant_new_strv((const gchar *const *)relays, -1));
    g_variant_builder_add_value(&list, g_variant_builder_end(&row));
  }
  return g_variant_ref_sink(g_variant_builder_end(&list));
}
