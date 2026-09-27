/*
 * notify_prefs.c — see notify_prefs.h.
 */
#include "notify_prefs.h"

void nostr_notify_prefs_defaults(NostrNotifyPrefs *p) {
  p->notify_groups = true;
  p->notify_dms = true;
  p->group_preview = true;
  p->sound = false;
}

static void read_bool(GKeyFile *kf, const char *key, bool *v) {
  if (!g_key_file_has_key(kf, "notify", key, NULL)) return;
  GError *e = NULL;
  gboolean b = g_key_file_get_boolean(kf, "notify", key, &e);
  if (e) {
    g_warning("nostr-notify: [notify] %s: %s; keeping %s", key, e->message, *v ? "true" : "false");
    g_error_free(e);
    return;
  }
  *v = b;
}

void nostr_notify_prefs_load(GKeyFile *kf, NostrNotifyPrefs *p) {
  nostr_notify_prefs_defaults(p);
  if (!kf) return;
  read_bool(kf, "notify_groups", &p->notify_groups);
  read_bool(kf, "notify_dms", &p->notify_dms);
  read_bool(kf, "group_preview", &p->group_preview);
  read_bool(kf, "sound", &p->sound);
}
