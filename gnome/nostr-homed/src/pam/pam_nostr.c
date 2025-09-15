#include "pam_nostr.h"
#include <string.h>
#include <gio/gio.h>
#include "nostr_dbus.h"
#include "nostr_cache.h"
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

static int get_username(pam_handle_t *pamh, const char **user_out){
  const char *user = NULL; int rc = pam_get_user(pamh, &user, NULL);
  if (rc != PAM_SUCCESS || !user) return PAM_USER_UNKNOWN;
  *user_out = user; return PAM_SUCCESS;
}

int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)flags; (void)argc; (void)argv;
  const char *user = NULL; int rc = get_username(pamh, &user);
  if (rc != PAM_SUCCESS) return rc;
  const char *busname = nh_signer_bus_name();
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ pam_syslog(pamh, LOG_ERR, "pam_nostr: DBus unavailable: %s", err?err->message:"error"); if (err) g_error_free(err); return PAM_AUTHINFO_UNAVAIL; }
  /* Call Authenticate(user)->(b ok). Signer should handle prompting per policy. */
  GVariant *ret = g_dbus_connection_call_sync(bus, busname,
                    "/org/nostr/Signer", "org.nostr.Signer", "Authenticate",
                    g_variant_new("(s)", user), G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ pam_syslog(pamh, LOG_ERR, "pam_nostr: Authenticate failed: %s", err?err->message:"error"); if (err) g_error_free(err); g_object_unref(bus); return PAM_AUTH_ERR; }
  gboolean ok = FALSE; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret); g_object_unref(bus);
  if (!ok){ pam_syslog(pamh, LOG_WARNING, "pam_nostr: authentication denied for %s", user); return PAM_AUTH_ERR; }
  pam_info(pamh, "pam_nostr: authenticated %s via signer", user);
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
  pam_info(pamh, "pam_nostr: opening session for %s", user);
  /* Export basic env */
  char home[256]; snprintf(home, sizeof home, "/home/%s", user);
  char shell[64]; snprintf(shell, sizeof shell, "/bin/bash");
  char xdg_runtime[256]; snprintf(xdg_runtime, sizeof xdg_runtime, "/run/user/%u", getuid());
  char xdg_data[256]; snprintf(xdg_data, sizeof xdg_data, "%s/.local/share", home);
  char xdg_config[256]; snprintf(xdg_config, sizeof xdg_config, "%s/.config", home);
  pam_putenv(pamh, (char*) (const char*)({ static char buf[320]; snprintf(buf, sizeof buf, "HOME=%s", home); buf; }));
  pam_putenv(pamh, (char*) (const char*)({ static char buf[96]; snprintf(buf, sizeof buf, "SHELL=%s", shell); buf; }));
  pam_putenv(pamh, (char*) (const char*)({ static char buf[320]; snprintf(buf, sizeof buf, "XDG_RUNTIME_DIR=%s", xdg_runtime); buf; }));
  pam_putenv(pamh, (char*) (const char*)({ static char buf[320]; snprintf(buf, sizeof buf, "XDG_DATA_HOME=%s", xdg_data); buf; }));
  pam_putenv(pamh, (char*) (const char*)({ static char buf[320]; snprintf(buf, sizeof buf, "XDG_CONFIG_HOME=%s", xdg_config); buf; }));
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ pam_syslog(pamh, LOG_ERR, "DBus unavailable: %s", err?err->message:"error"); if (err) g_error_free(err); return PAM_SESSION_ERR; }
  GVariant *ret = g_dbus_connection_call_sync(bus,
      "org.nostr.Homed1", "/org/nostr/Homed1", "org.nostr.Homed1", "OpenSession",
      g_variant_new("(s)", user), G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ pam_syslog(pamh, LOG_ERR, "OpenSession failed: %s", err?err->message:"error"); if (err) g_error_free(err); g_object_unref(bus); return PAM_SESSION_ERR; }
  gboolean ok = FALSE; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret); g_object_unref(bus);
  if (!ok){ pam_syslog(pamh, LOG_ERR, "OpenSession returned failure"); return PAM_SESSION_ERR; }
  return PAM_SUCCESS;
}

int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)flags; (void)argc; (void)argv;
  const char *user = NULL; int rc = get_username(pamh, &user);
  if (rc != PAM_SUCCESS) return rc;
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ if (err) g_error_free(err); return PAM_SUCCESS; }
  GVariant *ret = g_dbus_connection_call_sync(bus,
      "org.nostr.Homed1", "/org/nostr/Homed1", "org.nostr.Homed1", "CloseSession",
      g_variant_new("(s)", user), G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (!ret){ if (err) g_error_free(err); g_object_unref(bus); return PAM_SUCCESS; }
  g_variant_unref(ret); g_object_unref(bus); return PAM_SUCCESS;
}
