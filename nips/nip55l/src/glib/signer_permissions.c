#include "signer_permissions.h"
#include "signer_permission_policy.h"
#include <nostrc-profile-guard.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

#define REVIEW_LIFETIME (90 * 24 * 60 * 60)
struct _SignerPermissions {
  GKeyFile *records;
  gchar *config_home;
  GMutex lock;
  guint64 revision, next_expiry;
  SignerTrustLookup trust;
  gpointer trust_data;
  SignerPermissionsChanged changed;
  gpointer changed_data;
  GArray *required;
};
typedef struct {
  gchar *principal, *identity, *operation, *decision, *legacy;
  gint kind;
  guint64 expires, created;
} Row;
static void row_free(Row *r) {
  g_free(r->principal); g_free(r->identity); g_free(r->operation);
  g_free(r->decision); g_free(r->legacy); g_free(r);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Row, row_free)
static guint64 now_seconds(void) { return g_get_real_time() / G_USEC_PER_SEC; }
static gboolean fail(GError **error, GIOErrorEnum code, const gchar *message) {
  g_set_error_literal(error, G_IO_ERROR, code, message); return FALSE;
}
static gboolean known_operation(const gchar *op) {
  static const gchar *const operations[] = {"event", "get_public_key", "get_relays", "ping", "connect",
    "nip04_encrypt", "nip04_decrypt", "nip44_encrypt", "nip44_decrypt", "nip44_conversation_key", "zap_decrypt", NULL};
  return op && g_strv_contains(operations, op);
}
static gboolean valid_row(Row *r) {
  if (!r->principal || !*r->principal || !r->identity || !r->decision ||
      !g_utf8_validate(r->principal, -1, NULL) || !g_utf8_validate(r->identity, -1, NULL) ||
      strlen(r->principal) > 4096 || strlen(r->identity) > 4096 || !known_operation(r->operation) ||
      (strcmp(r->decision, "allow") && strcmp(r->decision, "deny") && strcmp(r->decision, "review"))) return FALSE;
  gboolean event = !strcmp(r->operation, "event");
  if (r->kind < -1 || r->kind > G_MAXUINT16 || (!event && r->kind != -1)) return FALSE;
  if (event && r->kind == -1 && !strcmp(r->decision, "allow")) return FALSE;
  if (!strcmp(r->decision, "allow") && !strcmp(r->identity, "*") && strcmp(r->operation, "get_relays")) return FALSE;
  if (!*r->identity && strcmp(r->operation, "get_relays") && strcmp(r->operation, "ping") && strcmp(r->operation, "connect")) return FALSE;
  if (!strcmp(r->decision, "review") && (!r->created || !r->legacy || !*r->legacy)) return FALSE;
  return TRUE;
}
static gchar *row_id(Row *r) {
  GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
  const gchar *parts[] = {r->principal, r->identity, r->operation};
  for (guint i = 0; i < G_N_ELEMENTS(parts); i++) {
    g_autofree gchar *len = g_strdup_printf("%" G_GSIZE_FORMAT ":", strlen(parts[i]));
    g_checksum_update(hash, (const guchar *)len, strlen(len));
    g_checksum_update(hash, (const guchar *)parts[i], strlen(parts[i]));
  }
  g_autofree gchar *kind = g_strdup_printf(":%d", r->kind);
  g_checksum_update(hash, (const guchar *)kind, strlen(kind));
  gchar *id = g_strconcat("record-", g_checksum_get_string(hash), NULL);
  g_checksum_free(hash); return id;
}
static guint64 read_timestamp(GKeyFile *kf, const gchar *id, const gchar *key, GError **error) {
  g_autofree gchar *text = g_key_file_get_string(kf, id, key, error);
  if (!text) return 0;
  gchar *end = NULL; errno = 0;
  guint64 value = g_ascii_strtoull(text, &end, 10);
  if (!g_ascii_isdigit(text[0]) || errno || !end || *end)
    fail(error, G_IO_ERROR_INVALID_DATA, "invalid permission timestamp");
  return value;
}
static Row *read_row(GKeyFile *kf, const gchar *id, GError **out_error) {
  GError *local = NULL;
  GError **error = &local;
  g_autofree gchar *expected = NULL;
  Row *r = g_new0(Row, 1);
  r->principal = g_key_file_get_string(kf, id, "principal", error);
  if (error && *error) goto bad;
  r->identity = g_key_file_get_string(kf, id, "identity", error);
  if (error && *error) goto bad;
  r->operation = g_key_file_get_string(kf, id, "operation", error);
  if (error && *error) goto bad;
  r->decision = g_key_file_get_string(kf, id, "decision", error);
  if (error && *error) goto bad;
  r->kind = g_key_file_get_integer(kf, id, "event-kind", error);
  if (error && *error) goto bad;
  r->expires = read_timestamp(kf, id, "expires-at", error);
  if (error && *error) goto bad;
  if (g_key_file_has_key(kf, id, "created-at", NULL)) {
    r->created = read_timestamp(kf, id, "created-at", error);
    if (*error) goto bad;
  }
  r->legacy = g_key_file_get_string(kf, id, "legacy-scope", NULL);
  if (!valid_row(r)) { fail(error, G_IO_ERROR_INVALID_DATA, "invalid permission record"); goto bad; }
  expected = row_id(r);
  if (strcmp(expected, id)) { fail(error, G_IO_ERROR_INVALID_DATA, "permission record id does not match scope"); goto bad; }
  return r;
bad:
  g_propagate_error(out_error, local);
  row_free(r); return NULL;
}
static void write_row(GKeyFile *kf, Row *r) {
  g_autofree gchar *id = row_id(r);
  g_key_file_remove_group(kf, id, NULL);
  g_key_file_set_string(kf, id, "principal", r->principal);
  g_key_file_set_string(kf, id, "identity", r->identity);
  g_key_file_set_string(kf, id, "operation", r->operation);
  g_key_file_set_integer(kf, id, "event-kind", r->kind);
  g_key_file_set_string(kf, id, "decision", r->decision);
  g_key_file_set_uint64(kf, id, "expires-at", r->expires);
  if (r->created) g_key_file_set_uint64(kf, id, "created-at", r->created);
  if (r->legacy) g_key_file_set_string(kf, id, "legacy-scope", r->legacy);
}
static GKeyFile *new_records(void) {
  GKeyFile *kf = g_key_file_new();
  g_key_file_set_integer(kf, "permissions", "version", 2); return kf;
}
static gchar *path_for(SignerPermissions *p, const gchar *name, GError **error) {
  g_autofree gchar *dir = g_build_filename(p->config_home, "gnostr", NULL);
  gchar *path = g_build_filename(dir, name, NULL);
  if (nostrc_profile_guard_refuses_path(dir) || nostrc_profile_guard_refuses_path(path)) {
    fail(error, G_IO_ERROR_PERMISSION_DENIED, "permission store refuses the real profile under test");
    g_free(path); return NULL;
  }
  return path;
}
static gboolean load_optional(GKeyFile *kf, const gchar *path, GError **error) {
  GError *local = NULL;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &local)) return TRUE;
  if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) { g_error_free(local); return TRUE; }
  g_propagate_error(error, local); return FALSE;
}
static gboolean save_file(GKeyFile *kf, const gchar *path, GError **error) {
  g_autofree gchar *dir = g_path_get_dirname(path);
  if (g_mkdir_with_parents(dir, 0700) || g_chmod(dir, 0700))
    return fail(error, G_IO_ERROR_PERMISSION_DENIED, "cannot secure permission directory");
  gsize size;
  g_autofree gchar *data = g_key_file_to_data(kf, &size, error);
  return data && g_file_set_contents_full(path, data, size,
      G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
}
static void changed(SignerPermissions *p) {
  g_mutex_lock(&p->lock); p->revision++; g_mutex_unlock(&p->lock);
  if (p->changed) p->changed(p->changed_data);
}
SignerPermissions *signer_permissions_new(const gchar *config_home) {
  SignerPermissions *p = g_new0(SignerPermissions, 1);
  p->config_home = g_strdup(config_home ? config_home : g_get_user_config_dir());
  g_mutex_init(&p->lock); p->records = new_records(); p->revision = 1;
  p->required = g_array_new(FALSE, FALSE, sizeof(guint32));
  g_array_append_vals(p->required, signer_default_required_confirmation_kinds,
                      G_N_ELEMENTS(signer_default_required_confirmation_kinds)); return p;
}
void signer_permissions_free(SignerPermissions *p) {
  if (!p) return;
  g_key_file_unref(p->records); g_array_unref(p->required); g_mutex_clear(&p->lock); g_free(p->config_home); g_free(p);
}
guint64 signer_permissions_revision(SignerPermissions *p) {
  g_mutex_lock(&p->lock);
  /* Absolute expiry can invalidate queued auto-authority without a mutation
   * or owner-context timer running. Workers read only this locked clock fence. */
  if (p->next_expiry && now_seconds() >= p->next_expiry) {
    p->next_expiry = 0; p->revision++;
  }
  guint64 revision = p->revision; g_mutex_unlock(&p->lock); return revision;
}
void signer_permissions_set_changed(SignerPermissions *p, SignerPermissionsChanged fn, gpointer data) {
  p->changed = fn; p->changed_data = data;
}
void signer_permissions_policy_changed(SignerPermissions *p) { changed(p); }
void signer_permissions_set_trust_lookup(SignerPermissions *p, SignerTrustLookup fn, gpointer data) {
  p->trust = fn; p->trust_data = data; changed(p);
}
gboolean signer_permissions_set_required_confirmation(SignerPermissions *p,
    const guint32 *kinds, gsize count, GError **error) {
  if (count && !kinds) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "missing required-confirmation kinds");
  for (gsize i = 0; i < count; i++) if (kinds[i] > G_MAXUINT16)
    return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "invalid required-confirmation kind");
  g_array_set_size(p->required, 0);
  if (count) g_array_append_vals(p->required, kinds, count);
  changed(p); return TRUE;
}
static void update_expiry_fence(SignerPermissions *p, GKeyFile *records) {
  guint64 next_expiry = 0, now = now_seconds();
  g_auto(GStrv) groups = g_key_file_get_groups(records, NULL);
  for (guint i = 0; groups[i]; i++) {
    if (!g_str_has_prefix(groups[i], "record-")) continue;
    g_autoptr(Row) r = read_row(records, groups[i], NULL);
    if (r && r->expires > now && (!next_expiry || r->expires < next_expiry)) next_expiry = r->expires;
  }
  signer_permissions_revision(p);
  g_mutex_lock(&p->lock); p->next_expiry = next_expiry; g_mutex_unlock(&p->lock);
}
static gboolean same_data(GKeyFile *a, GKeyFile *b) {
  g_autofree gchar *left = g_key_file_to_data(a, NULL, NULL);
  g_autofree gchar *right = g_key_file_to_data(b, NULL, NULL);
  return !g_strcmp0(left, right);
}
static gboolean parse_legacy(const gchar *val, gboolean *allow, guint64 *expires) {
  const gchar *tail;
  if (g_str_has_prefix(val, "allow")) { *allow = TRUE; tail = val + 5; }
  else if (g_str_has_prefix(val, "deny")) { *allow = FALSE; tail = val + 4; }
  else return FALSE;
  *expires = 0;
  if (!*tail) return TRUE;
  if (*tail++ != ':' || !g_ascii_isdigit(*tail)) return FALSE;
  gchar *end = NULL; errno = 0; *expires = g_ascii_strtoull(tail, &end, 10);
  return !errno && end && !*end;
}
/* Import fingerprints are tombstones too: a cleared v2 row cannot be
 * resurrected by an unchanged compatibility file on the next restart. */
static gboolean migrate(GKeyFile *records, GKeyFile *legacy, gboolean *dirty) {
  g_auto(GStrv) ops = g_key_file_get_groups(legacy, NULL);
  for (guint i = 0; ops[i]; i++) {
    g_auto(GStrv) keys = g_key_file_get_keys(legacy, ops[i], NULL, NULL);
    for (guint j = 0; keys && keys[j]; j++) {
      g_autofree gchar *val = g_key_file_get_string(legacy, ops[i], keys[j], NULL);
      gboolean allow = FALSE; guint64 expires = 0;
      const gchar *bar = strrchr(keys[j], '|');
      gboolean unsafe = !strcmp(ops[i], "event") && val && g_str_has_prefix(val, "allow");
      if (unsafe) { g_key_file_remove_key(legacy, ops[i], keys[j], NULL); *dirty = TRUE; }
      if (!bar || bar == keys[j] || !bar[1] || !val || !parse_legacy(val, &allow, &expires)) continue;
      g_autoptr(Row) r = g_new0(Row, 1);
      r->principal = g_strndup(keys[j], bar - keys[j]); r->identity = g_strdup(bar + 1);
      r->operation = g_strdup(ops[i]); r->kind = -1; r->expires = expires;
      r->decision = g_strdup(unsafe ? "review" : allow ? "allow" : "deny");
      if (unsafe) { r->created = now_seconds(); r->legacy = g_strdup("operation-wide event allow"); }
      if (!valid_row(r)) continue;
      g_autofree gchar *id = row_id(r);
      g_autofree gchar *fingerprint = g_compute_checksum_for_string(G_CHECKSUM_SHA256, val, -1);
      g_autofree gchar *previous = g_key_file_get_string(records, "imports", id, NULL);
      g_autofree gchar *existing = g_key_file_get_string(records, id, "decision", NULL);
      if (!g_strcmp0(previous, fingerprint) && (!unsafe || existing)) continue;
      /* Never replace an explicit broad denial with a legacy review marker. */
      if (g_strcmp0(existing, "deny") || !strcmp(r->decision, "deny")) write_row(records, r);
      g_key_file_set_string(records, "imports", id, fingerprint);
    }
  }
  return TRUE;
}
gboolean signer_permissions_reload(SignerPermissions *p, GError **error) {
  g_autofree gchar *path = path_for(p, "signer-permissions-v2.ini", error);
  if (!path) return FALSE;
  g_autofree gchar *old_path = path_for(p, "signer-grants.ini", error);
  if (!old_path) return FALSE;
  g_autoptr(GKeyFile) next = new_records();
  g_autoptr(GKeyFile) legacy = g_key_file_new();
  if (!load_optional(next, path, error) || !load_optional(legacy, old_path, error)) return FALSE;
  if (g_key_file_get_integer(next, "permissions", "version", NULL) != 2)
    return fail(error, G_IO_ERROR_NOT_SUPPORTED, "unsupported permissions file version");
  g_auto(GStrv) groups = g_key_file_get_groups(next, NULL);
  for (guint i = 0; groups[i]; i++) {
    if (!strcmp(groups[i], "permissions") || !strcmp(groups[i], "imports")) continue;
    g_autoptr(Row) r = read_row(next, groups[i], error);
    if (!r) return FALSE;
  }
  g_autofree gchar *before = g_key_file_to_data(next, NULL, NULL);
  gboolean dirty = FALSE; migrate(next, legacy, &dirty);
  g_clear_pointer(&groups, g_strfreev); groups = g_key_file_get_groups(next, NULL);
  for (guint i = 0; groups[i]; i++) {
    if (!g_str_has_prefix(groups[i], "record-")) continue;
    g_autoptr(Row) r = read_row(next, groups[i], error);
    if (!r) return FALSE;
    if (!strcmp(r->decision, "review") && now_seconds() >= r->created &&
        now_seconds() - r->created >= REVIEW_LIFETIME) g_key_file_remove_group(next, groups[i], NULL);
  }
  g_autofree gchar *after = g_key_file_to_data(next, NULL, NULL);
  /* Durable review marker first, then remove unsafe rollback authority.
   * Neither snapshot is enabled until BOTH writes have succeeded. */
  if (strcmp(before, after) && !save_file(next, path, error)) return FALSE;
  if (dirty && !save_file(legacy, old_path, error)) return FALSE;
  gboolean differs = !same_data(p->records, next);
  update_expiry_fence(p, next);
  g_key_file_unref(p->records); p->records = g_steal_pointer(&next);
  if (differs) changed(p);
  return TRUE;
}
static gboolean row_live(Row *r) { return !r->expires || now_seconds() < r->expires; }
static gboolean contains_kind(const guint32 *kinds, gsize count, gint kind) {
  for (gsize i = 0; i < count; i++) if (kind >= 0 && kinds[i] == (guint32)kind) return TRUE;
  return FALSE;
}
SignerPermissionResult signer_permissions_evaluate(SignerPermissions *p,
    const gchar *principal, const gchar *identity, const gchar *op, gint kind,
    gboolean first_party_default, GError **error) {
  if (!signer_permissions_reload(p, error)) return SIGNER_PERMISSION_ERROR;
  if (!known_operation(op) || kind < -1 || kind > G_MAXUINT16 ||
      (!strcmp(op, "event") ? kind < 0 : kind != -1)) return SIGNER_PERMISSION_ASK;
  gboolean allow = FALSE;
  g_auto(GStrv) groups = g_key_file_get_groups(p->records, NULL);
  for (guint i = 0; groups[i]; i++) {
    if (!g_str_has_prefix(groups[i], "record-")) continue;
    g_autoptr(Row) r = read_row(p->records, groups[i], error);
    if (!r) return SIGNER_PERMISSION_ERROR;
    if (g_strcmp0(principal, r->principal) || strcmp(op, r->operation) || !row_live(r) ||
        (g_strcmp0(identity ? identity : "", r->identity) && strcmp(r->identity, "*")) ||
        (r->kind != -1 && r->kind != kind)) continue;
    if (!strcmp(r->decision, "deny")) return SIGNER_PERMISSION_DENY;
    if (!strcmp(r->decision, "allow")) allow = TRUE;
  }
  if (!strcmp(op, "event") && contains_kind((guint32 *)p->required->data, p->required->len, kind))
    return SIGNER_PERMISSION_ASK;
  /* Export is never implied by a tier (and no raw-key client operation exists). */
  if (allow || first_party_default) return SIGNER_PERMISSION_ALLOW;
  gboolean consented = FALSE;
  SignerTrustTier tier = principal && p->trust ? p->trust(principal, &consented, p->trust_data) : SIGNER_TRUST_LOW;
  if (!principal || !strcmp(op, "nip44_conversation_key") || !strcmp(op, "connect")) return SIGNER_PERMISSION_ASK;
  if (tier == SIGNER_TRUST_FULL) return SIGNER_PERMISSION_ALLOW;
  if (tier == SIGNER_TRUST_MEDIUM && (!strcmp(op, "get_public_key") || !strcmp(op, "get_relays") ||
      !strcmp(op, "ping") || (!strcmp(op, "event") && contains_kind(signer_public_publishing_kinds,
        G_N_ELEMENTS(signer_public_publishing_kinds), kind)))) return SIGNER_PERMISSION_ALLOW;
  if (tier == SIGNER_TRUST_LOW && consented && !strcmp(op, "get_public_key")) return SIGNER_PERMISSION_ALLOW;
  return SIGNER_PERMISSION_ASK;
}
static void variant_row(GVariantBuilder *rows, Row *r) {
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "principal", g_variant_new_string(r->principal));
  g_variant_builder_add(&b, "{sv}", "identity", g_variant_new_string(r->identity));
  g_variant_builder_add(&b, "{sv}", "operation", g_variant_new_string(r->operation));
  g_variant_builder_add(&b, "{sv}", "event-kind", g_variant_new_int32(r->kind));
  g_variant_builder_add(&b, "{sv}", "decision", g_variant_new_string(r->decision));
  g_variant_builder_add(&b, "{sv}", "expires-at", g_variant_new_uint64(r->expires));
  g_variant_builder_add(&b, "{sv}", "state", g_variant_new_string(!strcmp(r->decision, "review") ? "needs-review" : "explicit"));
  if (r->legacy) g_variant_builder_add(&b, "{sv}", "legacy-scope", g_variant_new_string(r->legacy));
  g_variant_builder_add_value(rows, g_variant_builder_end(&b));
}
GVariant *signer_permissions_list(SignerPermissions *p, const gchar *principal, GError **error) {
  if (!signer_permissions_reload(p, error)) return NULL;
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}"));
  g_auto(GStrv) groups = g_key_file_get_groups(p->records, NULL);
  for (guint i = 0; groups[i]; i++) {
    if (!g_str_has_prefix(groups[i], "record-")) continue;
    g_autoptr(Row) r = read_row(p->records, groups[i], NULL);
    if ((!principal || !strcmp(principal, r->principal)) && row_live(r)) variant_row(&b, r);
  }
  return g_variant_ref_sink(g_variant_builder_end(&b));
}
gboolean signer_permissions_needs_review(SignerPermissions *p, const gchar *principal, GError **error) {
  g_autoptr(GVariant) rows = signer_permissions_list(p, principal, error);
  if (!rows) return FALSE;
  for (gsize i = 0; i < g_variant_n_children(rows); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(rows, i); const gchar *state = NULL;
    if (g_variant_lookup(row, "state", "&s", &state) && !strcmp(state, "needs-review")) return TRUE;
  }
  return FALSE;
}
static GKeyFile *copy_records(SignerPermissions *p) {
  g_autofree gchar *data = g_key_file_to_data(p->records, NULL, NULL);
  GKeyFile *copy = g_key_file_new(); g_key_file_load_from_data(copy, data, -1, G_KEY_FILE_NONE, NULL); return copy;
}
static gboolean matches(Row *r, const gchar *principal, const gchar *identity, const gchar *op) {
  return !g_strcmp0(principal, r->principal) && (!identity || !strcmp(identity, r->identity)) &&
         (!op || !strcmp(op, r->operation));
}
static void remove_rows(GKeyFile *kf, const gchar *principal, const gchar *identity, const gchar *op, gboolean *removed) {
  g_auto(GStrv) groups = g_key_file_get_groups(kf, NULL);
  for (guint i = 0; groups[i]; i++) {
    if (!g_str_has_prefix(groups[i], "record-")) continue;
    g_autoptr(Row) r = read_row(kf, groups[i], NULL);
    if (r && matches(r, principal, identity, op)) { g_key_file_remove_group(kf, groups[i], NULL); *removed = TRUE; }
  }
}
static gboolean clear_legacy(SignerPermissions *p, const gchar *principal, const gchar *identity, const gchar *op, GError **error) {
  g_autofree gchar *path = path_for(p, "signer-grants.ini", error);
  if (!path) return FALSE;
  g_autoptr(GKeyFile) legacy = g_key_file_new();
  if (!load_optional(legacy, path, error)) return FALSE;
  gboolean dirty = FALSE;
  g_auto(GStrv) groups = g_key_file_get_groups(legacy, NULL);
  for (guint i = 0; groups[i]; i++) {
    if (op && strcmp(op, groups[i])) continue;
    g_auto(GStrv) keys = g_key_file_get_keys(legacy, groups[i], NULL, NULL);
    for (guint j = 0; keys && keys[j]; j++) {
      const gchar *bar = strrchr(keys[j], '|');
      if (!bar) continue;
      g_autofree gchar *who = g_strndup(keys[j], bar - keys[j]);
      if (!strcmp(who, principal) && (!identity || !strcmp(bar + 1, identity))) {
        g_key_file_remove_key(legacy, groups[i], keys[j], NULL); dirty = TRUE;
      }
    }
  }
  return !dirty || save_file(legacy, path, error);
}
static gboolean commit(SignerPermissions *p, GKeyFile *next, GError **error) {
  g_autofree gchar *path = path_for(p, "signer-permissions-v2.ini", error);
  if (!path || !save_file(next, path, error)) return FALSE;
  g_key_file_unref(p->records); p->records = g_key_file_ref(next);
  update_expiry_fence(p, next);
  changed(p); return TRUE;
}
gboolean signer_permissions_clear(SignerPermissions *p, const gchar *principal,
    const gchar *identity, const gchar *op, gboolean *removed, GError **error) {
  *removed = FALSE;
  if (!principal || !*principal) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "missing principal");
  if (!signer_permissions_reload(p, error)) return FALSE;
  g_autoptr(GKeyFile) next = copy_records(p);
  gboolean found = FALSE; remove_rows(next, principal, identity, op, &found);
  if (!found) return TRUE;
  if (!clear_legacy(p, principal, identity, op, error) || !commit(p, next, error)) return FALSE;
  *removed = TRUE; return TRUE;
}
gboolean signer_permissions_remember(SignerPermissions *p, const gchar *principal,
    const gchar *identity, const gchar *op, gint kind, gboolean allow, guint64 ttl, GError **error) {
  guint64 now = now_seconds();
  if (ttl > G_MAXUINT64 - now) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "permission expiry overflow");
  Row row = {.principal=(gchar *)principal, .identity=(gchar *)(identity ? identity : ""),
    .operation=(gchar *)op, .kind=kind, .decision=allow ? "allow" : "deny", .expires=ttl ? now + ttl : 0};
  if (!valid_row(&row)) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "invalid remembered permission scope");
  if (!signer_permissions_reload(p, error)) return FALSE;
  g_autoptr(GKeyFile) next = copy_records(p); write_row(next, &row);
  /* Retire any old operation-wide row for this exact scope. */
  if (kind == -1 && !clear_legacy(p, principal, row.identity, op, error)) return FALSE;
  return commit(p, next, error);
}
gboolean signer_permissions_replace(SignerPermissions *p, const gchar *principal,
    const gchar *identity, GVariant *rows, guint64 expected_revision, GError **error) {
  if (!principal || !*principal || !identity || !rows || !g_variant_is_of_type(rows, G_VARIANT_TYPE("aa{sv}")))
    return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "invalid permission replacement");
  if (!signer_permissions_reload(p, error)) return FALSE;
  if (expected_revision != signer_permissions_revision(p)) return fail(error, G_IO_ERROR_BUSY, "permission revision Conflict");
  g_autoptr(GKeyFile) next = copy_records(p);
  gboolean removed = FALSE; remove_rows(next, principal, identity, NULL, &removed);
  g_autoptr(GHashTable) ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (gsize i = 0; i < g_variant_n_children(rows); i++) {
    g_autoptr(GVariant) item = g_variant_get_child_value(rows, i);
    const gchar *who = NULL, *id = NULL, *op = NULL, *decision = NULL;
    gint kind = -2; guint64 expires = 0;
    if (!g_variant_lookup(item, "principal", "&s", &who) || !g_variant_lookup(item, "identity", "&s", &id) ||
        !g_variant_lookup(item, "operation", "&s", &op) || !g_variant_lookup(item, "decision", "&s", &decision) ||
        !g_variant_lookup(item, "event-kind", "i", &kind) || !g_variant_lookup(item, "expires-at", "t", &expires) ||
        strcmp(who, principal) || strcmp(id, identity)) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "replacement scope mismatch");
    Row row = {.principal=(gchar *)who, .identity=(gchar *)id, .operation=(gchar *)op,
      .decision=(gchar *)decision, .kind=kind, .expires=expires};
    /* Review entries may be retained, but only using the existing migration metadata. */
    g_autofree gchar *key = row_id(&row);
    g_autoptr(Row) previous = !strcmp(decision, "review") ? read_row(p->records, key, NULL) : NULL;
    if (previous && !strcmp(previous->decision, "review")) { row.created = previous->created; row.legacy = previous->legacy; }
    if (!valid_row(&row) || !g_hash_table_add(ids, g_strdup(key)))
      return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "invalid or duplicate replacement record");
    write_row(next, &row);
  }
  if (!clear_legacy(p, principal, identity, NULL, error)) return FALSE;
  return commit(p, next, error);
}
GVariant *signer_permissions_list_legacy(SignerPermissions *p, GError **error) {
  if (!signer_permissions_reload(p, error)) return NULL;
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a(sssbt)"));
  g_auto(GStrv) groups = g_key_file_get_groups(p->records, NULL);
  for (guint i = 0; groups[i]; i++) {
    if (!g_str_has_prefix(groups[i], "record-")) continue;
    g_autoptr(Row) r = read_row(p->records, groups[i], NULL);
    if (r->kind == -1 && strcmp(r->decision, "review"))
      g_variant_builder_add(&b, "(sssbt)", r->operation, r->principal, r->identity, !strcmp(r->decision, "allow"), r->expires);
  }
  return g_variant_ref_sink(g_variant_builder_end(&b));
}
