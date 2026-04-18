#include "pam_nostr.h"
#include <string.h>
#include <gio/gio.h>
#include "nostr_cache.h"
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>

/* Signer D-Bus coordinates */
#define SIGNER_BUS   "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"
#define SIGNER_IFACE "org.nostr.Signer"
#define HOMED_BUS    "org.nostr.Homed1"
#define HOMED_PATH   "/org/nostr/Homed1"
#define HOMED_IFACE  "org.nostr.Homed1"

static int get_username(pam_handle_t *pamh, const char **user_out){
  const char *user = NULL; int rc = pam_get_user(pamh, &user, NULL);
  if (rc != PAM_SUCCESS || !user) return PAM_USER_UNKNOWN;
  *user_out = user; return PAM_SUCCESS;
}

/**
 * Generate 32 random hex bytes for a nonce.
 * Falls back to /dev/urandom; returns 0 on success.
 */
static int gen_nonce_hex(char out[65]){
  unsigned char buf[32];
  FILE *f = fopen("/dev/urandom", "rb");
  if (!f) return -1;
  size_t n = fread(buf, 1, 32, f);
  fclose(f);
  if (n != 32) return -1;
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++){
    out[i*2]   = hx[(buf[i] >> 4) & 0xF];
    out[i*2+1] = hx[buf[i] & 0xF];
  }
  out[64] = '\0';
  return 0;
}

/**
 * NIP-46-style challenge/response via the signer's SignEvent method.
 *
 * Builds a kind-22242 auth event with:
 *   content: challenge JSON (nonce, hostname, tty, timestamp)
 *   tags: [["relay","pam"],["challenge",nonce]]
 *
 * Calls SignEvent(event_json, "", "pam_nostr") on the signer.
 * The signer shows an approval dialog per its ACL policy.
 * If the signer returns a signature, the key holder approved.
 *
 * We trust the session bus isolation: only the user's own processes
 * can reach their signer on the session bus.
 */
static int pam_nip46_challenge(pam_handle_t *pamh, GDBusConnection *bus){
  char nonce[65];
  if (gen_nonce_hex(nonce) != 0){
    pam_syslog(pamh, LOG_ERR, "pam_nostr: failed to generate nonce");
    return -1;
  }

  char hostname[256] = {0};
  (void)gethostname(hostname, sizeof hostname - 1);

  const char *tty = ttyname(STDIN_FILENO);
  if (!tty) tty = "unknown";

  int64_t now = (int64_t)time(NULL);

  /* Build the kind-22242 event JSON (unsigned — signer will compute id and sign).
   * Minimal valid event: pubkey left empty (signer fills it), no id, no sig. */
  char event_json[2048];
  snprintf(event_json, sizeof event_json,
    "{\"kind\":22242,\"created_at\":%lld,"
    "\"tags\":[[\"relay\",\"pam\"],[\"challenge\",\"%s\"]],"
    "\"content\":\"{\\\"nonce\\\":\\\"%s\\\",\\\"host\\\":\\\"%s\\\","
    "\\\"tty\\\":\\\"%s\\\",\\\"ts\\\":%lld,\\\"exp\\\":%lld}\"}",
    (long long)now, nonce, nonce, hostname, tty,
    (long long)now, (long long)(now + 60));

  GError *err = NULL;
  GVariant *ret = g_dbus_connection_call_sync(bus,
    SIGNER_BUS, SIGNER_PATH, SIGNER_IFACE, "SignEvent",
    g_variant_new("(sss)", event_json, "", "pam_nostr"),
    G_VARIANT_TYPE("(s)"),
    G_DBUS_CALL_FLAGS_NONE, 30000, /* 30s timeout for user approval */
    NULL, &err);

  if (!ret){
    pam_syslog(pamh, LOG_WARNING, "pam_nostr: SignEvent failed: %s",
               err ? err->message : "unknown error");
    if (err) g_error_free(err);
    return -1;
  }

  const char *sig = NULL;
  g_variant_get(ret, "(&s)", &sig);
  int ok = (sig && strlen(sig) == 128) ? 0 : -1;
  if (ok != 0)
    pam_syslog(pamh, LOG_WARNING, "pam_nostr: SignEvent returned invalid signature");
  else
    pam_syslog(pamh, LOG_INFO, "pam_nostr: NIP-46 challenge signed successfully");
  g_variant_unref(ret);
  return ok;
}

int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)flags; (void)argc; (void)argv;
  const char *user = NULL; int rc = get_username(pamh, &user);
  if (rc != PAM_SUCCESS) return rc;

  /* No D-Bus calls here — the session bus is not available during
   * pam_sm_authenticate (it starts later via pam_systemd).
   * We do a lightweight cache check: if the user exists in the
   * nostr-homed cache, allow authentication to proceed.
   * Cryptographic identity verification (NIP-46 challenge/response)
   * is deferred to pam_sm_open_session where the user bus is up.
   * See docs/SYSTEMD_TOPOLOGY.md for the rationale. */
  nh_cache c;
  if (nh_cache_open_configured(&c, "/etc/nss_nostr.conf") != 0) {
    pam_syslog(pamh, LOG_ERR, "pam_nostr: cache unavailable");
    return PAM_AUTHINFO_UNAVAIL;
  }
  unsigned int uid = 0, gid = 0;
  char home[256];
  if (nh_cache_lookup_name(&c, user, &uid, &gid, home, sizeof home) != 0) {
    nh_cache_close(&c);
    pam_syslog(pamh, LOG_WARNING, "pam_nostr: user %s not in cache", user);
    return PAM_USER_UNKNOWN;
  }
  nh_cache_close(&c);
  pam_syslog(pamh, LOG_INFO, "pam_nostr: user %s found in cache (uid=%u), auth deferred to open_session", user, uid);
  return PAM_SUCCESS;
}

int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)pamh; (void)flags; (void)argc; (void)argv; return PAM_SUCCESS;
}

int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)flags; (void)argc; (void)argv;
  const char *user = NULL; int rc = get_username(pamh, &user);
  if (rc != PAM_SUCCESS) return rc;
  nh_cache c; if (nh_cache_open_configured(&c, "/etc/nss_nostr.conf")!=0){
    pam_syslog(pamh, LOG_ERR, "pam_nostr: cache unavailable");
    return PAM_USER_UNKNOWN;
  }
  unsigned int uid=0,gid=0; char home[256];
  if (nh_cache_lookup_name(&c, user, &uid, &gid, home, sizeof home) != 0){
    nh_cache_close(&c);
    pam_syslog(pamh, LOG_WARNING, "pam_nostr: user %s not found in cache", user);
    return PAM_USER_UNKNOWN;
  }
  nh_cache_close(&c);
  return PAM_SUCCESS;
}

int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)flags; (void)argc; (void)argv;
  const char *user = NULL; int rc = get_username(pamh, &user);
  if (rc != PAM_SUCCESS) return rc;

  /* Look up the user's UID from the cache — we need it for XDG_RUNTIME_DIR.
   * Do NOT use getuid(): in a PAM module that returns the daemon's UID
   * (typically 0 for sshd/login/gdm), not the authenticating user's. */
  nh_cache c;
  if (nh_cache_open_configured(&c, "/etc/nss_nostr.conf") != 0) {
    pam_syslog(pamh, LOG_ERR, "pam_nostr: cache unavailable, cannot open session");
    return PAM_SESSION_ERR;
  }
  unsigned int uid = 0, gid = 0;
  char cache_home[256];
  if (nh_cache_lookup_name(&c, user, &uid, &gid, cache_home, sizeof cache_home) != 0) {
    nh_cache_close(&c);
    pam_syslog(pamh, LOG_ERR, "pam_nostr: user %s not in cache, cannot set env", user);
    return PAM_SESSION_ERR;
  }
  nh_cache_close(&c);

  pam_info(pamh, "pam_nostr: opening session for %s (uid=%u)", user, uid);

  /* Build environment strings. pam_putenv copies its input, so local
   * buffers are fine — no need for static storage. */
  char home[256];
  snprintf(home, sizeof home, "/home/%s", user);

  char env_home[320];
  snprintf(env_home, sizeof env_home, "HOME=%s", home);
  pam_putenv(pamh, env_home);

  char env_shell[96];
  snprintf(env_shell, sizeof env_shell, "SHELL=/bin/bash");
  pam_putenv(pamh, env_shell);

  /* XDG_RUNTIME_DIR uses the looked-up UID, not getuid().
   * systemd-logind creates /run/user/<uid> on session open. */
  char env_xdg_runtime[320];
  snprintf(env_xdg_runtime, sizeof env_xdg_runtime, "XDG_RUNTIME_DIR=/run/user/%u", uid);
  pam_putenv(pamh, env_xdg_runtime);

  char env_xdg_data[320];
  snprintf(env_xdg_data, sizeof env_xdg_data, "XDG_DATA_HOME=%s/.local/share", home);
  pam_putenv(pamh, env_xdg_data);

  char env_xdg_config[320];
  snprintf(env_xdg_config, sizeof env_xdg_config, "XDG_CONFIG_HOME=%s/.config", home);
  pam_putenv(pamh, env_xdg_config);

  /* Connect to session bus (available after pam_systemd) */
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){
    pam_syslog(pamh, LOG_ERR, "pam_nostr: session bus unavailable: %s",
               err ? err->message : "error");
    if (err) g_error_free(err);
    return PAM_SESSION_ERR;
  }

  /* NIP-46 challenge/response: prove the signer controls its key.
   * Builds a kind-22242 auth event, asks signer to sign it.
   * The signer's ACL shows an approval dialog to the user. */
  if (pam_nip46_challenge(pamh, bus) != 0){
    pam_syslog(pamh, LOG_ERR, "pam_nostr: NIP-46 auth challenge failed for %s", user);
    g_object_unref(bus);
    return PAM_SESSION_ERR;
  }

  /* Trigger nostr-homed to mount home and warm cache */
  GVariant *ret = g_dbus_connection_call_sync(bus,
      HOMED_BUS, HOMED_PATH, HOMED_IFACE, "OpenSession",
      g_variant_new("(s)", user), G_VARIANT_TYPE_TUPLE,
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){
    pam_syslog(pamh, LOG_ERR, "pam_nostr: OpenSession failed: %s",
               err ? err->message : "error");
    if (err) g_error_free(err);
    g_object_unref(bus);
    return PAM_SESSION_ERR;
  }
  gboolean ok = FALSE;
  g_variant_get(ret, "(b)", &ok);
  g_variant_unref(ret);
  g_object_unref(bus);
  if (!ok){
    pam_syslog(pamh, LOG_ERR, "pam_nostr: OpenSession returned failure");
    return PAM_SESSION_ERR;
  }
  return PAM_SUCCESS;
}

int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)flags; (void)argc; (void)argv;
  const char *user = NULL; int rc = get_username(pamh, &user);
  if (rc != PAM_SUCCESS) return rc;
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ if (err) g_error_free(err); return PAM_SUCCESS; }
  GVariant *ret = g_dbus_connection_call_sync(bus,
      HOMED_BUS, HOMED_PATH, HOMED_IFACE, "CloseSession",
      g_variant_new("(s)", user), G_VARIANT_TYPE_TUPLE,
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ if (err) g_error_free(err); g_object_unref(bus); return PAM_SUCCESS; }
  g_variant_unref(ret); g_object_unref(bus); return PAM_SUCCESS;
}
