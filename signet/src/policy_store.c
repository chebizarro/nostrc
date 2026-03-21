/* SPDX-License-Identifier: MIT
 *
 * policy_store.c - Policy storage backends (Phase 4: file-backed).
 */

#include "signet/policy_store.h"
#include "signet/audit_logger.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* NIP-19 bech32 decode */
#include <nostr/nip19/nip19.h>

typedef enum {
  SIGNET_POLICY_STORE_BACKEND_NONE = 0,
  SIGNET_POLICY_STORE_BACKEND_FILE,
  /* SIGNET_POLICY_STORE_BACKEND_SQLCIPHER -- future */
} SignetPolicyStoreBackend;

typedef struct {
  gboolean default_allow;

  GPtrArray *allow_clients; /* gchar* canonical ("*" or 64 hex lower-case) */
  GPtrArray *deny_clients;  /* gchar* canonical ("*" or 64 hex lower-case) */

  GPtrArray *allow_methods; /* gchar* (may include "*") */
  GPtrArray *deny_methods;  /* gchar* (may include "*") */

  GArray *allow_kinds;      /* gint64 */
  GArray *deny_kinds;       /* gint64 */
  gboolean allow_kinds_any; /* wildcard */
  gboolean deny_kinds_any;  /* wildcard */

  guint32 ttl_seconds; /* if >0: reload TTL for this identity policy */
  int64_t loaded_at;   /* unix seconds; when parsed */
} SignetIdentityPolicy;

struct SignetPolicyStore {
  SignetPolicyStoreBackend backend;

  char *file_path;

  /* Parsed policies: identity string -> SignetIdentityPolicy* */
  GHashTable *identities;

  int64_t file_loaded_at;

  /* SIGHUP-driven reload request */
  gboolean reload_requested;

  /* Last load error (for operator logs); may be NULL. */
  char *last_error;


  GMutex mu;
};

/* ------------------------- SIGHUP support (reload) ------------------------- */

/* We install ONE handler globally and reference-count it across stores.
 * The handler:
 * - requests audit logger reopen (async-signal-safe)
 * - flips a sig_atomic_t that stores will observe and reload on next access
 *
 * This design intentionally allows a single SIGHUP to drive both log rotation
 * and policy reload without a REST control plane.
 */

static volatile sig_atomic_t g_signet_policy_sighup_requested = 0;

static struct sigaction g_signet_prev_sighup;
static gboolean g_signet_prev_sighup_valid = FALSE;

static gint g_signet_sighup_refcount = 0;
static GMutex g_signet_sighup_mu;

static void signet_policy_sighup_handler(int signo) {
  (void)signo;
  g_signet_policy_sighup_requested = 1;
  /* Ensure audit logger reopen still works even if this handler is installed. */
  signet_audit_logger_request_reopen();

  /* Chain previous handler (best-effort). */
  if (g_signet_prev_sighup_valid) {
    if (g_signet_prev_sighup.sa_handler != SIG_DFL &&
        g_signet_prev_sighup.sa_handler != SIG_IGN &&
        g_signet_prev_sighup.sa_handler != NULL &&
        g_signet_prev_sighup.sa_handler != signet_policy_sighup_handler) {
      g_signet_prev_sighup.sa_handler(signo);
    }
  }
}

static void signet_policy_sighup_install_ref(void) {
  g_mutex_lock(&g_signet_sighup_mu);

  if (g_signet_sighup_refcount == 0) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signet_policy_sighup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    struct sigaction prev;
    memset(&prev, 0, sizeof(prev));

    if (sigaction(SIGHUP, &sa, &prev) == 0) {
      g_signet_prev_sighup = prev;
      g_signet_prev_sighup_valid = TRUE;
    } else {
      g_signet_prev_sighup_valid = FALSE;
    }
  }

  g_signet_sighup_refcount++;
  g_mutex_unlock(&g_signet_sighup_mu);
}

static void signet_policy_sighup_uninstall_unref(void) {
  g_mutex_lock(&g_signet_sighup_mu);

  if (g_signet_sighup_refcount > 0) g_signet_sighup_refcount--;

  if (g_signet_sighup_refcount == 0) {
    if (g_signet_prev_sighup_valid) {
      (void)sigaction(SIGHUP, &g_signet_prev_sighup, NULL);
      g_signet_prev_sighup_valid = FALSE;
    }
  }

  g_mutex_unlock(&g_signet_sighup_mu);
}

/* ------------------------------ util helpers ------------------------------ */

static char *signet_strdup0(const char *s) {
  if (!s || s[0] == '\0') return NULL;
  return strdup(s);
}

static gboolean signet_streq0(const char *a, const char *b) {
  if (!a || !b) return FALSE;
  return strcmp(a, b) == 0;
}

static char *signet_strip_quotes_dup(const char *s) {
  if (!s) return NULL;

  g_autofree char *tmp = g_strdup(s);
  if (!tmp) return NULL;

  g_strstrip(tmp);
  size_t n = strlen(tmp);

  if (n >= 2 && ((tmp[0] == '\"' && tmp[n - 1] == '\"') || (tmp[0] == '\'' && tmp[n - 1] == '\''))) {
    tmp[n - 1] = '\0';
    tmp++;
    /* tmp now points inside allocated memory; copy into new string */
    return g_strdup(tmp);
  }

  return g_strdup(tmp);
}

static gboolean signet_is_hex_64(const char *s) {
  if (!s) return FALSE;
  if (strlen(s) != 64) return FALSE;
  for (size_t i = 0; i < 64; i++) {
    char c = s[i];
    gboolean ok = ((c >= '0' && c <= '9') ||
                   (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F'));
    if (!ok) return FALSE;
  }
  return TRUE;
}

static char *signet_hex_encode_lower(const guint8 *in, gsize in_len) {
  static const char *hex = "0123456789abcdef";
  if (!in || in_len == 0) return NULL;

  gsize out_len = in_len * 2;
  char *out = g_malloc(out_len + 1);
  if (!out) return NULL;

  for (gsize i = 0; i < in_len; i++) {
    out[i * 2] = hex[(in[i] >> 4) & 0xF];
    out[i * 2 + 1] = hex[in[i] & 0xF];
  }
  out[out_len] = '\0';
  return out;
}

/* Normalize pubkey strings to 64 hex lower-case when possible.
 * - "*" remains "*"
 * - "npub1..." decodes to 32 bytes and is hex-encoded
 * - 64-char hex is lower-cased
 * - otherwise lower-case the input (best-effort)
 */
static char *signet_pubkey_canon_dup(const char *s) {
  if (!s || s[0] == '\0') return NULL;
  if (strcmp(s, "*") == 0) return g_strdup("*");

  if (g_ascii_strncasecmp(s, "npub1", 5) == 0) {
    uint8_t pk[32];
    if (nostr_nip19_decode_npub(s, pk) != 0) return NULL;
    char *hex = signet_hex_encode_lower(pk, 32);
    memset(pk, 0, sizeof(pk));
    return hex;
  }

  if (signet_is_hex_64(s)) return g_ascii_strdown(s, -1);

  return g_ascii_strdown(s, -1);
}

static void signet_identity_policy_free(SignetIdentityPolicy *p) {
  if (!p) return;

  if (p->allow_clients) g_ptr_array_free(p->allow_clients, TRUE);
  if (p->deny_clients) g_ptr_array_free(p->deny_clients, TRUE);

  if (p->allow_methods) g_ptr_array_free(p->allow_methods, TRUE);
  if (p->deny_methods) g_ptr_array_free(p->deny_methods, TRUE);

  if (p->allow_kinds) g_array_free(p->allow_kinds, TRUE);
  if (p->deny_kinds) g_array_free(p->deny_kinds, TRUE);

  g_free(p);
}

static gboolean signet_list_has_any_strings(GPtrArray *a) {
  return a && a->len > 0;
}

static gboolean signet_list_match_string(GPtrArray *a, const char *value) {
  if (!a || !value) return FALSE;
  for (guint i = 0; i < a->len; i++) {
    const char *it = (const char *)g_ptr_array_index(a, i);
    if (!it) continue;
    if (strcmp(it, "*") == 0) return TRUE;
    if (strcmp(it, value) == 0) return TRUE;
  }
  return FALSE;
}

static gboolean signet_kinds_has_any(const SignetIdentityPolicy *p, gboolean allow) {
  if (!p) return FALSE;
  if (allow) return p->allow_kinds_any || (p->allow_kinds && p->allow_kinds->len > 0);
  return p->deny_kinds_any || (p->deny_kinds && p->deny_kinds->len > 0);
}

static gboolean signet_kinds_match(const SignetIdentityPolicy *p, gboolean allow, gint64 kind) {
  if (!p) return FALSE;

  if (allow && p->allow_kinds_any) return TRUE;
  if (!allow && p->deny_kinds_any) return TRUE;

  GArray *arr = allow ? p->allow_kinds : p->deny_kinds;
  if (!arr) return FALSE;

  for (guint i = 0; i < arr->len; i++) {
    gint64 v = g_array_index(arr, gint64, i);
    if (v == kind) return TRUE;
  }
  return FALSE;
}

/* Parse a JSON array string (preferred) or a loose CSV/semicolon-separated list. */
static GPtrArray *signet_parse_string_list(const char *raw) {
  if (!raw) return NULL;

  g_autofree char *s = signet_strip_quotes_dup(raw);
  if (!s) return NULL;
  g_strstrip(s);
  if (s[0] == '\0') return NULL;

  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  if (!out) return NULL;

  if (s[0] == '[') {
    JsonParser *p = json_parser_new();
    if (!p) {
      g_ptr_array_free(out, TRUE);
      return NULL;
    }

    if (!json_parser_load_from_data(p, s, -1, NULL)) {
      g_object_unref(p);
      g_ptr_array_free(out, TRUE);
      return NULL;
    }

    JsonNode *root = json_parser_get_root(p);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
      g_object_unref(p);
      g_ptr_array_free(out, TRUE);
      return NULL;
    }

    JsonArray *a = json_node_get_array(root);
    guint n = json_array_get_length(a);
    for (guint i = 0; i < n; i++) {
      JsonNode *en = json_array_get_element(a, i);
      if (!en) continue;

      if (JSON_NODE_HOLDS_VALUE(en) && json_node_get_value_type(en) == G_TYPE_STRING) {
        const char *v = json_node_get_string(en);
        if (!v) continue;
        g_autofree char *sv = signet_strip_quotes_dup(v);
        if (!sv) continue;
        g_strstrip(sv);
        if (sv[0] == '\0') continue;
        g_ptr_array_add(out, g_strdup(sv));
      }
    }

    g_object_unref(p);
    return out;
  }

  /* Loose parsing: split on comma/semicolon */
  gchar **parts = g_strsplit_set(s, ",;", -1);
  if (!parts) return out;

  for (gint i = 0; parts[i]; i++) {
    g_strstrip(parts[i]);
    if (parts[i][0] == '\0') continue;
    g_ptr_array_add(out, g_strdup(parts[i]));
  }

  g_strfreev(parts);
  return out;
}

static gboolean signet_parse_int_list(const char *raw, GArray **out_arr, gboolean *out_any) {
  if (out_arr) *out_arr = NULL;
  if (out_any) *out_any = FALSE;
  if (!raw) return TRUE;

  g_autofree char *s0 = signet_strip_quotes_dup(raw);
  if (!s0) return FALSE;
  g_strstrip(s0);
  if (s0[0] == '\0') return TRUE;

  GArray *arr = g_array_new(FALSE, FALSE, sizeof(gint64));
  if (!arr) return FALSE;

  gboolean any = FALSE;

  if (s0[0] == '[') {
    JsonParser *p = json_parser_new();
    if (!p) {
      g_array_free(arr, TRUE);
      return FALSE;
    }

    if (!json_parser_load_from_data(p, s0, -1, NULL)) {
      g_object_unref(p);
      g_array_free(arr, TRUE);
      return FALSE;
    }

    JsonNode *root = json_parser_get_root(p);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
      g_object_unref(p);
      g_array_free(arr, TRUE);
      return FALSE;
    }

    JsonArray *a = json_node_get_array(root);
    guint n = json_array_get_length(a);
    for (guint i = 0; i < n; i++) {
      JsonNode *en = json_array_get_element(a, i);
      if (!en) continue;

      if (JSON_NODE_HOLDS_VALUE(en)) {
        GType t = json_node_get_value_type(en);
        if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE || t == G_TYPE_INT || t == G_TYPE_LONG) {
          gint64 v = json_node_get_int(en);
          g_array_append_val(arr, v);
        } else if (t == G_TYPE_STRING) {
          const char *sv = json_node_get_string(en);
          if (sv && strcmp(sv, "*") == 0) any = TRUE;
        }
      }
    }

    g_object_unref(p);

    if (out_any) *out_any = any;
    if (out_arr) *out_arr = arr;
    return TRUE;
  }

  /* Loose parsing: split on comma/semicolon */
  gchar **parts = g_strsplit_set(s0, ",;", -1);
  if (!parts) {
    if (out_any) *out_any = any;
    if (out_arr) *out_arr = arr;
    return TRUE;
  }

  for (gint i = 0; parts[i]; i++) {
    g_strstrip(parts[i]);
    if (parts[i][0] == '\0') continue;
    if (strcmp(parts[i], "*") == 0) {
      any = TRUE;
      continue;
    }

    char *end = NULL;
    errno = 0;
    long long v = strtoll(parts[i], &end, 10);
    if (errno != 0 || !end || *end != '\0') continue;

    gint64 vv = (gint64)v;
    g_array_append_val(arr, vv);
  }

  g_strfreev(parts);

  if (out_any) *out_any = any;
  if (out_arr) *out_arr = arr;
  return TRUE;
}

static gboolean signet_policy_store_load_file_locked(SignetPolicyStore *ps, int64_t now) {
  if (!ps || ps->backend != SIGNET_POLICY_STORE_BACKEND_FILE) return FALSE;

  g_clear_pointer(&ps->last_error, g_free);

  /* Missing file => empty policies (default-deny at engine level). */
  if (!ps->file_path || ps->file_path[0] == '\0') {
    if (ps->identities) g_hash_table_remove_all(ps->identities);
    ps->file_loaded_at = now;
    ps->reload_requested = FALSE;
    return TRUE;
  }

  GKeyFile *kf = g_key_file_new();
  if (!kf) {
    ps->last_error = g_strdup("OOM creating GKeyFile");
    return FALSE;
  }

  GError *err = NULL;
  if (!g_key_file_load_from_file(kf, ps->file_path, G_KEY_FILE_NONE, &err)) {
    if (ps->identities) g_hash_table_remove_all(ps->identities);
    ps->file_loaded_at = now;
    ps->reload_requested = FALSE;

    if (err) {
      ps->last_error = g_strdup(err->message ? err->message : "failed to load policy file");
      g_error_free(err);
    } else {
      ps->last_error = g_strdup("failed to load policy file");
    }

    g_key_file_free(kf);
    return TRUE;
  }

  /* Build a new identities table and swap atomically under the mutex. */
  GHashTable *new_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)signet_identity_policy_free);
  if (!new_ids) {
    ps->last_error = g_strdup("OOM creating policy table");
    g_key_file_free(kf);
    return FALSE;
  }

  gsize ngroups = 0;
  gchar **groups = g_key_file_get_groups(kf, &ngroups);

  for (gsize gi = 0; gi < ngroups; gi++) {
    const char *grp = groups[gi];
    if (!grp) continue;

    if (!g_str_has_prefix(grp, "identity.")) continue;

    const char *identity = grp + strlen("identity.");
    if (!identity || identity[0] == '\0') continue;

    SignetIdentityPolicy *p = g_new0(SignetIdentityPolicy, 1);
    if (!p) continue;

    p->default_allow = FALSE; /* safe default */
    p->loaded_at = now;

    /* default = "allow"|"deny" */
    g_autofree gchar *defv_raw = g_key_file_get_string(kf, grp, "default", NULL);
    if (defv_raw) {
      g_autofree char *defv = signet_strip_quotes_dup(defv_raw);
      if (defv) {
        g_strstrip(defv);
        if (g_ascii_strcasecmp(defv, "allow") == 0) p->default_allow = TRUE;
        if (g_ascii_strcasecmp(defv, "deny") == 0) p->default_allow = FALSE;
      }
    }

    /* ttl_seconds */
    g_autofree gchar *ttl_raw = g_key_file_get_string(kf, grp, "ttl_seconds", NULL);
    if (ttl_raw) {
      g_autofree char *ttls = signet_strip_quotes_dup(ttl_raw);
      if (ttls) {
        g_strstrip(ttls);
        if (ttls[0] != '\0') {
          char *end = NULL;
          errno = 0;
          long long v = strtoll(ttls, &end, 10);
          if (errno == 0 && end && *end == '\0' && v >= 0 && v <= G_MAXUINT32) p->ttl_seconds = (guint32)v;
        }
      }
    }

    /* allow/deny clients */
    g_autofree gchar *allow_clients_raw = g_key_file_get_string(kf, grp, "allow_clients", NULL);
    GPtrArray *allow_clients = signet_parse_string_list(allow_clients_raw);
    if (allow_clients) {
      p->allow_clients = g_ptr_array_new_with_free_func(g_free);
      for (guint i = 0; i < allow_clients->len; i++) {
        const char *v = (const char *)g_ptr_array_index(allow_clients, i);
        if (!v) continue;
        if (strcmp(v, "*") == 0) {
          g_ptr_array_add(p->allow_clients, g_strdup("*"));
          continue;
        }
        g_autofree char *canon = signet_pubkey_canon_dup(v);
        if (canon) g_ptr_array_add(p->allow_clients, g_strdup(canon));
      }
      g_ptr_array_free(allow_clients, TRUE);
      if (p->allow_clients && p->allow_clients->len == 0) g_clear_pointer(&p->allow_clients, g_ptr_array_unref);
    }

    g_autofree gchar *deny_clients_raw = g_key_file_get_string(kf, grp, "deny_clients", NULL);
    GPtrArray *deny_clients = signet_parse_string_list(deny_clients_raw);
    if (deny_clients) {
      p->deny_clients = g_ptr_array_new_with_free_func(g_free);
      for (guint i = 0; i < deny_clients->len; i++) {
        const char *v = (const char *)g_ptr_array_index(deny_clients, i);
        if (!v) continue;
        if (strcmp(v, "*") == 0) {
          g_ptr_array_add(p->deny_clients, g_strdup("*"));
          continue;
        }
        g_autofree char *canon = signet_pubkey_canon_dup(v);
        if (canon) g_ptr_array_add(p->deny_clients, g_strdup(canon));
      }
      g_ptr_array_free(deny_clients, TRUE);
      if (p->deny_clients && p->deny_clients->len == 0) g_clear_pointer(&p->deny_clients, g_ptr_array_unref);
    }

    /* allow/deny methods */
    g_autofree gchar *allow_methods_raw = g_key_file_get_string(kf, grp, "allow_methods", NULL);
    p->allow_methods = signet_parse_string_list(allow_methods_raw);
    if (p->allow_methods && p->allow_methods->len == 0) g_clear_pointer(&p->allow_methods, g_ptr_array_unref);

    g_autofree gchar *deny_methods_raw = g_key_file_get_string(kf, grp, "deny_methods", NULL);
    p->deny_methods = signet_parse_string_list(deny_methods_raw);
    if (p->deny_methods && p->deny_methods->len == 0) g_clear_pointer(&p->deny_methods, g_ptr_array_unref);

    /* allow/deny kinds */
    g_autofree gchar *allow_kinds_raw = g_key_file_get_string(kf, grp, "allow_kinds", NULL);
    (void)signet_parse_int_list(allow_kinds_raw, &p->allow_kinds, &p->allow_kinds_any);

    g_autofree gchar *deny_kinds_raw = g_key_file_get_string(kf, grp, "deny_kinds", NULL);
    (void)signet_parse_int_list(deny_kinds_raw, &p->deny_kinds, &p->deny_kinds_any);

    g_hash_table_replace(new_ids, g_strdup(identity), p);
  }

  if (groups) g_strfreev(groups);
  g_key_file_free(kf);

  if (!ps->identities) {
    ps->identities = new_ids;
  } else {
    g_hash_table_destroy(ps->identities);
    ps->identities = new_ids;
  }

  ps->file_loaded_at = now;
  ps->reload_requested = FALSE;
  return TRUE;
}

static void signet_policy_store_maybe_reload_locked(SignetPolicyStore *ps, int64_t now) {
  if (!ps || ps->backend != SIGNET_POLICY_STORE_BACKEND_FILE) return;

  if (g_signet_policy_sighup_requested) {
    g_signet_policy_sighup_requested = 0;
    ps->reload_requested = TRUE;
  }

  if (ps->reload_requested) {
    (void)signet_policy_store_load_file_locked(ps, now);
    return;
  }

  /* TTL-driven reload: if any identity policy has ttl_seconds and the file is stale, reload. */
  if (!ps->identities || g_hash_table_size(ps->identities) == 0) return;
  if (ps->file_loaded_at <= 0) return;

  /* Find the smallest non-zero TTL among identities. */
  GHashTableIter it;
  gpointer k = NULL;
  gpointer v = NULL;
  guint32 min_ttl = 0;

  g_hash_table_iter_init(&it, ps->identities);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    SignetIdentityPolicy *p = (SignetIdentityPolicy *)v;
    if (!p) continue;
    if (p->ttl_seconds == 0) continue;
    if (min_ttl == 0 || p->ttl_seconds < min_ttl) min_ttl = p->ttl_seconds;
  }

  if (min_ttl == 0) return;

  if (now >= (ps->file_loaded_at + (int64_t)min_ttl)) {
    ps->reload_requested = TRUE;
    (void)signet_policy_store_load_file_locked(ps, now);
  }
}

/* ------------------------------ public API -------------------------------- */

int signet_policy_store_get(SignetPolicyStore *ps,
                            const SignetPolicyKeyView *key,
                            int64_t now,
                            SignetPolicyValue *out_val) {
  if (!ps || !key || !out_val || !key->identity || !key->method || !key->client_pubkey_hex) return -1;

  out_val->decision = SIGNET_POLICY_RULE_DENY;
  out_val->expires_at = 0;
  out_val->reason_code = "policy.error";

  if (ps->backend != SIGNET_POLICY_STORE_BACKEND_FILE) {
    /* Phase 4 only implements file backend. */
    out_val->reason_code = "policy.backend_unimplemented";
    return -1;
  }

  g_mutex_lock(&ps->mu);
  signet_policy_store_maybe_reload_locked(ps, now);

  if (!ps->identities) {
    g_mutex_unlock(&ps->mu);
    return 1;
  }

  SignetIdentityPolicy *p = (SignetIdentityPolicy *)g_hash_table_lookup(ps->identities, key->identity);
  if (!p) {
    g_mutex_unlock(&ps->mu);
    out_val->reason_code = "policy.identity_missing";
    return 1;
  }

  g_autofree char *client_canon = signet_pubkey_canon_dup(key->client_pubkey_hex);
  if (!client_canon) client_canon = g_ascii_strdown(key->client_pubkey_hex, -1);

  const gboolean is_sign_event = (strcmp(key->method, "sign_event") == 0);
  const gboolean kind_applicable = is_sign_event && (key->event_kind >= 0);

  /* Deny takes precedence (any matching deny dimension denies). */
  if (signet_list_has_any_strings(p->deny_clients) && signet_list_match_string(p->deny_clients, client_canon)) {
    out_val->decision = SIGNET_POLICY_RULE_DENY;
    out_val->reason_code = "policy.deny.client";
  } else if (signet_list_has_any_strings(p->deny_methods) && signet_list_match_string(p->deny_methods, key->method)) {
    out_val->decision = SIGNET_POLICY_RULE_DENY;
    out_val->reason_code = "policy.deny.method";
  } else if (kind_applicable && signet_kinds_has_any(p, FALSE) && signet_kinds_match(p, FALSE, (gint64)key->event_kind)) {
    out_val->decision = SIGNET_POLICY_RULE_DENY;
    out_val->reason_code = "policy.deny.kind";
  } else {
    /* Explicit allow requires matching all configured allow dimensions. */
    const gboolean has_allow_rule =
        signet_list_has_any_strings(p->allow_clients) ||
        signet_list_has_any_strings(p->allow_methods) ||
        (kind_applicable && signet_kinds_has_any(p, TRUE));

    gboolean allow_match = TRUE;

    if (signet_list_has_any_strings(p->allow_clients)) {
      allow_match = allow_match && signet_list_match_string(p->allow_clients, client_canon);
    }
    if (signet_list_has_any_strings(p->allow_methods)) {
      allow_match = allow_match && signet_list_match_string(p->allow_methods, key->method);
    }
    if (kind_applicable && signet_kinds_has_any(p, TRUE)) {
      allow_match = allow_match && signet_kinds_match(p, TRUE, (gint64)key->event_kind);
    }

    if (has_allow_rule && allow_match) {
      out_val->decision = SIGNET_POLICY_RULE_ALLOW;
      out_val->reason_code = "policy.allow.match";
    } else {
      out_val->decision = p->default_allow ? SIGNET_POLICY_RULE_ALLOW : SIGNET_POLICY_RULE_DENY;
      out_val->reason_code = p->default_allow ? "policy.default_allow" : "policy.default_deny";
    }
  }

  /* TTL semantics: treat as policy evaluation freshness hint for callers. */
  if (p->ttl_seconds > 0 && ps->file_loaded_at > 0) out_val->expires_at = ps->file_loaded_at + (int64_t)p->ttl_seconds;

  g_mutex_unlock(&ps->mu);
  return 0;
}

/* ---- set_identity_json: parse JSON → SignetIdentityPolicy, persist ---- */

/* Helper: serialize an identity policy back to GKeyFile under [identity.<id>]. */
static void signet_policy_identity_to_keyfile(GKeyFile *kf,
                                              const char *identity,
                                              const SignetIdentityPolicy *p) {
  g_autofree char *grp = g_strdup_printf("identity.%s", identity);

  g_key_file_set_string(kf, grp, "default", p->default_allow ? "allow" : "deny");

  if (p->ttl_seconds > 0) {
    g_autofree char *ttl = g_strdup_printf("%u", p->ttl_seconds);
    g_key_file_set_string(kf, grp, "ttl_seconds", ttl);
  }

  /* Serialize string list as JSON array. */
  #define WRITE_STR_LIST(field, key_name)                                         \
    if (p->field && p->field->len > 0) {                                          \
      GString *buf = g_string_new("[");                                           \
      for (guint _i = 0; _i < p->field->len; _i++) {                             \
        if (_i > 0) g_string_append_c(buf, ',');                                  \
        g_string_append_printf(buf, "\"%s\"",                                     \
            (const char *)g_ptr_array_index(p->field, _i));                       \
      }                                                                            \
      g_string_append_c(buf, ']');                                                \
      g_key_file_set_string(kf, grp, key_name, buf->str);                         \
      g_string_free(buf, TRUE);                                                   \
    }

  WRITE_STR_LIST(allow_clients, "allow_clients")
  WRITE_STR_LIST(deny_clients,  "deny_clients")
  WRITE_STR_LIST(allow_methods, "allow_methods")
  WRITE_STR_LIST(deny_methods,  "deny_methods")
  #undef WRITE_STR_LIST

  /* Serialize kind lists. */
  #define WRITE_KIND_LIST(arr_field, any_field, key_name)                          \
    if (p->any_field) {                                                            \
      g_key_file_set_string(kf, grp, key_name, "[\"*\"]");                        \
    } else if (p->arr_field && p->arr_field->len > 0) {                           \
      GString *buf = g_string_new("[");                                           \
      for (guint _i = 0; _i < p->arr_field->len; _i++) {                         \
        if (_i > 0) g_string_append_c(buf, ',');                                  \
        g_string_append_printf(buf, "%" G_GINT64_FORMAT,                          \
            g_array_index(p->arr_field, gint64, _i));                             \
      }                                                                            \
      g_string_append_c(buf, ']');                                                \
      g_key_file_set_string(kf, grp, key_name, buf->str);                         \
      g_string_free(buf, TRUE);                                                   \
    }

  WRITE_KIND_LIST(allow_kinds, allow_kinds_any, "allow_kinds")
  WRITE_KIND_LIST(deny_kinds,  deny_kinds_any,  "deny_kinds")
  #undef WRITE_KIND_LIST
}

/* Parse a JSON string list member into a canonicalized GPtrArray. */
static GPtrArray *signet_parse_json_string_array(JsonObject *o,
                                                 const char *member,
                                                 gboolean canonicalize_pubkeys) {
  if (!json_object_has_member(o, member)) return NULL;

  JsonNode *n = json_object_get_member(o, member);
  if (!n || !JSON_NODE_HOLDS_ARRAY(n)) return NULL;

  JsonArray *a = json_node_get_array(n);
  guint len = json_array_get_length(a);
  if (len == 0) return NULL;

  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; i < len; i++) {
    JsonNode *en = json_array_get_element(a, i);
    if (!en || !JSON_NODE_HOLDS_VALUE(en)) continue;
    if (json_node_get_value_type(en) != G_TYPE_STRING) continue;

    const char *v = json_node_get_string(en);
    if (!v || v[0] == '\0') continue;

    if (canonicalize_pubkeys) {
      char *canon = signet_pubkey_canon_dup(v);
      if (canon) g_ptr_array_add(out, canon);
    } else {
      g_ptr_array_add(out, g_strdup(v));
    }
  }

  if (out->len == 0) { g_ptr_array_free(out, TRUE); return NULL; }
  return out;
}

/* Parse a JSON int/string array for kind lists. */
static gboolean signet_parse_json_kind_array(JsonObject *o,
                                             const char *member,
                                             GArray **out_arr,
                                             gboolean *out_any) {
  *out_arr = NULL;
  *out_any = FALSE;

  if (!json_object_has_member(o, member)) return TRUE;

  JsonNode *n = json_object_get_member(o, member);
  if (!n || !JSON_NODE_HOLDS_ARRAY(n)) return FALSE;

  JsonArray *a = json_node_get_array(n);
  guint len = json_array_get_length(a);
  if (len == 0) return TRUE;

  GArray *arr = g_array_new(FALSE, FALSE, sizeof(gint64));

  for (guint i = 0; i < len; i++) {
    JsonNode *en = json_array_get_element(a, i);
    if (!en) continue;

    if (JSON_NODE_HOLDS_VALUE(en)) {
      GType t = json_node_get_value_type(en);
      if (t == G_TYPE_STRING) {
        const char *sv = json_node_get_string(en);
        if (sv && strcmp(sv, "*") == 0) *out_any = TRUE;
      } else if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE || t == G_TYPE_INT) {
        gint64 v = json_node_get_int(en);
        g_array_append_val(arr, v);
      }
    }
  }

  if (arr->len == 0 && !*out_any) { g_array_free(arr, TRUE); return TRUE; }
  *out_arr = arr;
  return TRUE;
}

int signet_policy_store_set_identity_json(SignetPolicyStore *ps,
                                          const char *identity,
                                          const char *policy_json,
                                          int64_t now,
                                          char **out_error) {
  if (!ps || !identity || !identity[0] || !policy_json || !policy_json[0]) {
    if (out_error) *out_error = g_strdup("invalid arguments");
    return -1;
  }

  /* Parse policy JSON. */
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, policy_json, -1, NULL)) {
    if (out_error) *out_error = g_strdup("invalid policy JSON");
    return -1;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    if (out_error) *out_error = g_strdup("policy must be a JSON object");
    return -1;
  }

  JsonObject *o = json_node_get_object(root);

  /* Build SignetIdentityPolicy from the JSON. */
  SignetIdentityPolicy *p = g_new0(SignetIdentityPolicy, 1);
  p->loaded_at = now;
  p->default_allow = FALSE; /* safe default */

  /* "default": "allow"|"deny" */
  if (json_object_has_member(o, "default")) {
    const char *defv = json_object_get_string_member(o, "default");
    if (defv && g_ascii_strcasecmp(defv, "allow") == 0) p->default_allow = TRUE;
  }

  /* "ttl_seconds": <int> */
  if (json_object_has_member(o, "ttl_seconds")) {
    gint64 ttl = json_object_get_int_member(o, "ttl_seconds");
    if (ttl > 0 && ttl <= G_MAXUINT32) p->ttl_seconds = (guint32)ttl;
  }

  /* Client lists (canonicalize pubkeys). */
  p->allow_clients = signet_parse_json_string_array(o, "allow_clients", TRUE);
  p->deny_clients  = signet_parse_json_string_array(o, "deny_clients",  TRUE);

  /* Method lists. */
  p->allow_methods = signet_parse_json_string_array(o, "allow_methods", FALSE);
  p->deny_methods  = signet_parse_json_string_array(o, "deny_methods",  FALSE);

  /* Kind lists. */
  if (!signet_parse_json_kind_array(o, "allow_kinds", &p->allow_kinds, &p->allow_kinds_any)) {
    signet_identity_policy_free(p);
    if (out_error) *out_error = g_strdup("invalid allow_kinds array");
    return -1;
  }
  if (!signet_parse_json_kind_array(o, "deny_kinds", &p->deny_kinds, &p->deny_kinds_any)) {
    signet_identity_policy_free(p);
    if (out_error) *out_error = g_strdup("invalid deny_kinds array");
    return -1;
  }

  /* Update in-memory table. */
  g_mutex_lock(&ps->mu);

  if (!ps->identities) {
    ps->identities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           (GDestroyNotify)signet_identity_policy_free);
  }

  g_hash_table_replace(ps->identities, g_strdup(identity), p);
  /* p is now owned by the hash table. */

  /* Persist to file if file-backed. */
  if (ps->backend == SIGNET_POLICY_STORE_BACKEND_FILE && ps->file_path) {
    /* Rebuild the entire GKeyFile from the in-memory table. */
    GKeyFile *kf = g_key_file_new();

    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, ps->identities);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      signet_policy_identity_to_keyfile(kf, (const char *)k, (const SignetIdentityPolicy *)v);
    }

    gsize len = 0;
    g_autofree gchar *data = g_key_file_to_data(kf, &len, NULL);
    g_key_file_free(kf);

    if (data) {
      GError *err = NULL;
      if (!g_file_set_contents(ps->file_path, data, (gssize)len, &err)) {
        g_mutex_unlock(&ps->mu);
        if (out_error) {
          *out_error = g_strdup_printf("policy applied in memory but failed to persist: %s",
                                       err ? err->message : "unknown error");
        }
        if (err) g_error_free(err);
        /* Return 0 — the in-memory update succeeded; persistence is best-effort. */
        return 0;
      }
    }
  }

  g_mutex_unlock(&ps->mu);
  return 0;
}

int signet_policy_store_put(SignetPolicyStore *ps,
                            const SignetPolicyKeyView *key,
                            const SignetPolicyValue *val,
                            int64_t now) {
  (void)ps;
  (void)key;
  (void)val;
  (void)now;
  /* Phase 4: single-rule put not yet implemented; use set_identity_json. */
  return -1;
}

int signet_policy_store_delete(SignetPolicyStore *ps,
                               const SignetPolicyKeyView *key,
                               int64_t now) {
  (void)ps;
  (void)key;
  (void)now;
  /* Phase 4: read-only file-backed store. */
  return -1;
}

void signet_policy_store_free(SignetPolicyStore *ps) {
  if (!ps) return;

  /* Only file backend installs SIGHUP hook in Phase 4. */
  if (ps->backend == SIGNET_POLICY_STORE_BACKEND_FILE) {
    signet_policy_sighup_uninstall_unref();
  }

  g_mutex_lock(&ps->mu);

  if (ps->identities) {
    g_hash_table_destroy(ps->identities);
    ps->identities = NULL;
  }

  g_clear_pointer(&ps->file_path, free);
  g_clear_pointer(&ps->last_error, g_free);


  g_mutex_unlock(&ps->mu);
  g_mutex_clear(&ps->mu);

  free(ps);
}

SignetPolicyStore *signet_policy_store_file_new(const char *path) {
  SignetPolicyStore *ps = (SignetPolicyStore *)calloc(1, sizeof(*ps));
  if (!ps) return NULL;

  ps->backend = SIGNET_POLICY_STORE_BACKEND_FILE;
  ps->file_path = signet_strdup0(path);

  g_mutex_init(&ps->mu);

  ps->identities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)signet_identity_policy_free);
  if (!ps->identities) {
    signet_policy_store_free(ps);
    return NULL;
  }

  /* Install SIGHUP reload hook (ref-counted). */
  signet_policy_sighup_install_ref();

  /* Initial load (best-effort). */
  const int64_t now = (int64_t)time(NULL);
  g_mutex_lock(&ps->mu);
  (void)signet_policy_store_load_file_locked(ps, now);
  g_mutex_unlock(&ps->mu);

  return ps;
}

/* SQLCipher-backed policy store will be implemented in a future task. */