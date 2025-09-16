#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void usage(const char *argv0){
  fprintf(stderr, "Usage: %s <provision|teardown> --user <user> --host 127.0.0.1 --port 7680\n", argv0);
}

static int write_file(const char *path, const char *content){
  GError *err=NULL; GFile *f = g_file_new_for_path(path);
  GFileOutputStream *os = g_file_replace(f, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &err);
  if (!os){ if (err){ fprintf(stderr, "write %s: %s\n", path, err->message); g_error_free(err);} g_object_unref(f); return -1; }
  gsize w=0; if (!g_output_stream_write_all(G_OUTPUT_STREAM(os), content, strlen(content), &w, NULL, &err)){
    if (err){ fprintf(stderr, "write %s: %s\n", path, err->message); g_error_free(err);} g_object_unref(os); g_object_unref(f); return -1;
  }
  g_object_unref(os); g_object_unref(f); return 0;
}

static int ensure_dirs(const char *path){
  GError *err=NULL; if (!g_mkdir_with_parents(path, 0700)==0){ return 0; }
  return 0;
}

static char *read_text_file(const char *path){
  GError *err=NULL; char *data=NULL; gsize len=0;
  if (!g_file_get_contents(path, &data, &len, &err)){
    if (err){ fprintf(stderr, "read %s: %s\n", path, err->message); g_error_free(err);} return NULL;
  }
  return data;
}

static char *subst_vars(char *tmpl, const char *user, const char *host, const char *port){
  if (!tmpl) return NULL;
  const struct { const char *k; const char *v; } vars[] = {
    {"${USER}", user}, {"${HOST}", host}, {"${PORT}", port}
  };
  GString *out = g_string_new("");
  char *p = tmpl;
  while (*p){
    gboolean matched=FALSE;
    for (size_t i=0;i<sizeof(vars)/sizeof(vars[0]);i++){
      size_t klen = strlen(vars[i].k);
      if (g_str_has_prefix(p, vars[i].k)){
        g_string_append(out, vars[i].v ? vars[i].v : "");
        p += klen; matched=TRUE; break;
      }
    }
    if (!matched){ g_string_append_c(out, *p); p++; }
  }
  g_free(tmpl);
  return g_string_free(out, FALSE);
}

static int render_eds_sources(const char *user, const char *host, const char *port){
  const char *xdg = g_get_user_config_dir();
  char dir[512]; snprintf(dir, sizeof dir, "%s/evolution/sources", xdg);
  ensure_dirs(dir);
  char cal_dst[600]; snprintf(cal_dst, sizeof cal_dst, "%s/nostr-caldav-%s.source", dir, user);
  char card_dst[600]; snprintf(card_dst, sizeof card_dst, "%s/nostr-carddav-%s.source", dir, user);
  /* Locate installed templates (allow override by NOSTR_GOA_OVERLAY_DATADIR) */
  const char *dd = g_getenv("NOSTR_GOA_OVERLAY_DATADIR");
  char datadir[512]; if (dd && *dd) { strncpy(datadir, dd, sizeof datadir - 1); datadir[sizeof datadir - 1] = '\0'; }
  else { snprintf(datadir, sizeof datadir, "%s/nostr-goa-overlay/eds_sources", g_get_user_data_dir()); }
  char cal_src[700]; snprintf(cal_src, sizeof cal_src, "%s/calendar.source.tmpl", datadir);
  char card_src[700]; snprintf(card_src, sizeof card_src, "%s/contacts.source.tmpl", datadir);
  char *cal_t = read_text_file(cal_src); if (!cal_t) return -1;
  char *card_t = read_text_file(card_src); if (!card_t){ g_free(cal_t); return -1; }
  char *cal_s = subst_vars(cal_t, user, host, port);
  char *card_s = subst_vars(card_t, user, host, port);
  int rc = 0; if (write_file(cal_dst, cal_s)!=0) rc=-1; if (write_file(card_dst, card_s)!=0) rc=-1;
  g_free(cal_s); g_free(card_s);
  return rc;
}

static int start_user_unit(const char *unit){
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ if (err){ g_error_free(err);} return -1; }
  GVariant *reply = g_dbus_connection_call_sync(bus,
    "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
    "org.freedesktop.systemd1.Manager", "StartUnit",
    g_variant_new("(ss)", unit, "replace"), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (err){ g_error_free(err); if (reply) g_variant_unref(reply); g_object_unref(bus); return -1; }
  if (reply) g_variant_unref(reply); g_object_unref(bus); return 0;
}

static int stop_user_unit(const char *unit){
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ if (err){ g_error_free(err);} return -1; }
  GVariant *reply = g_dbus_connection_call_sync(bus,
    "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
    "org.freedesktop.systemd1.Manager", "StopUnit",
    g_variant_new("(ss)", unit, "replace"), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  if (err){ g_error_free(err); if (reply) g_variant_unref(reply); g_object_unref(bus); return -1; }
  if (reply) g_variant_unref(reply); g_object_unref(bus); return 0;
}

int main(int argc, char **argv){
  if (argc < 2){ usage(argv[0]); return 2; }
  const char *cmd = argv[1];
  const char *user=NULL, *host="127.0.0.1", *port="7680";
  for (int i=2;i<argc;i++){
    if (strcmp(argv[i], "--user")==0 && i+1<argc) user=argv[++i];
    else if (strcmp(argv[i], "--host")==0 && i+1<argc) host=argv[++i];
    else if (strcmp(argv[i], "--port")==0 && i+1<argc) port=argv[++i];
  }
  if (!user || !*user){ fprintf(stderr, "missing --user\n"); return 2; }
  if (strcmp(cmd, "provision")==0){
    if (render_eds_sources(user, host, port)!=0) return 1;
    start_user_unit("nostr-router.service");
    start_user_unit("nostr-dav.service");
    start_user_unit("nostrfs.service");
    start_user_unit("nostr-notify.service");
    /* Register nostr: handler */
    {
      gchar *argvv[] = { (gchar*)"xdg-mime", (gchar*)"default", (gchar*)"nostr.desktop", (gchar*)"x-scheme-handler/nostr", NULL };
      GError *err=NULL; gint status=0; (void)g_spawn_sync(NULL, argvv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, &err);
      if (err) g_error_free(err);
      /* Update desktop database */
      gchar *argvv2[] = { (gchar*)"update-desktop-database", NULL };
      err=NULL; status=0; (void)g_spawn_sync(NULL, argvv2, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, &err);
      if (err) g_error_free(err);
    }
    return 0;
  } else if (strcmp(cmd, "teardown")==0){
    stop_user_unit("nostr-notify.service");
    stop_user_unit("nostrfs.service");
    stop_user_unit("nostr-dav.service");
    stop_user_unit("nostr-router.service");
    /* Remove EDS minimal sources */
    const char *xdg = g_get_user_config_dir(); char path[600];
    snprintf(path, sizeof path, "%s/evolution/sources/nostr-caldav-%s.source", xdg, user); g_unlink(path);
    snprintf(path, sizeof path, "%s/evolution/sources/nostr-carddav-%s.source", xdg, user); g_unlink(path);
    /* Best-effort unregister handler by removing user-local desktop entry */
    {
      const char *dshare = g_get_user_data_dir(); char dpath[600];
      snprintf(dpath, sizeof dpath, "%s/applications/nostr.desktop", dshare);
      (void)g_unlink(dpath);
      gchar *argvv2[] = { (gchar*)"update-desktop-database", NULL };
      GError *err=NULL; gint status=0; (void)g_spawn_sync(NULL, argvv2, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, &err);
      if (err) g_error_free(err);
    }
    /* Remove x-scheme-handler/nostr associations from ~/.config/mimeapps.list */
    {
      gchar *mimeapps = g_build_filename(g_get_user_config_dir(), "mimeapps.list", NULL);
      gchar *data=NULL; gsize len=0; GError *rerr=NULL;
      if (g_file_get_contents(mimeapps, &data, &len, &rerr)){
        GString *out = g_string_new("");
        gboolean in_defaults=FALSE, in_added=FALSE;
        gchar **lines = g_strsplit(data, "\n", -1);
        for (gchar **p=lines; p && *p; p++){
          const char *line = *p;
          if (g_str_has_prefix(line, "[")){
            in_defaults = g_str_has_prefix(line, "[Default Applications]");
            in_added = g_str_has_prefix(line, "[Added Associations]");
            g_string_append(out, line); g_string_append_c(out, '\n');
            continue;
          }
          if ((in_defaults || in_added) && g_str_has_prefix(line, "x-scheme-handler/nostr=")){
            /* skip */
            continue;
          }
          g_string_append(out, line); g_string_append_c(out, '\n');
        }
        g_strfreev(lines);
        GError *werr=NULL; (void)g_file_set_contents(mimeapps, out->str, out->len, &werr); if (werr) g_error_free(werr);
        g_string_free(out, TRUE);
        g_free(data);
      }
      if (rerr) g_error_free(rerr);
      g_free(mimeapps);
    }
    return 0;
  }
  usage(argv[0]); return 2;
}
