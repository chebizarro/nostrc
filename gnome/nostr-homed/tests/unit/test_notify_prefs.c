/*
 * test_notify_prefs — nostr-notify.conf presentation keys (nostrc-prqu.16):
 * defaults, explicit values, malformed values keep the default, and the
 * file Nostr Settings writes (nss_notify_conf_save's key names) round-trips.
 */
#include "notify_prefs.h"
#include "notify_gnotification.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void load(const char *text, NostrNotifyPrefs *p) {
  GKeyFile *kf = g_key_file_new();
  CHECK(g_key_file_load_from_data(kf, text, -1, G_KEY_FILE_NONE, NULL));
  nostr_notify_prefs_load(kf, p);
  g_key_file_unref(kf);
}

int main(void) {
  NostrNotifyPrefs p;
  nostr_notify_prefs_load(NULL, &p);
  CHECK(p.notify_groups && p.notify_dms && p.group_preview && !p.sound);

  load("[notify]\nupstream_mode = direct\nhome_relays = wss://a\n", &p);
  CHECK(p.notify_groups && p.notify_dms && p.group_preview && !p.sound);

  load("[notify]\nnotify_groups = false\nnotify_dms = true\ngroup_preview = false\nsound = true\n", &p);
  CHECK(!p.notify_groups && p.notify_dms && !p.group_preview && p.sound);

  /* a typo keeps the default rather than silencing anything */
  load("[notify]\nnotify_dms = nope\nsound = maybe\n", &p);
  CHECK(p.notify_dms && !p.sound);

  /* keys outside [notify] are not ours */
  load("[other]\nnotify_dms = false\n", &p);
  CHECK(p.notify_dms);

  /* group_preview = false: fixed body, still a routable notification */
  NostrNotifyBuild b; memset(&b, 0, sizeof b);
  GNotification *n = nostr_notify_build_group(NULL, "grp", "cafebabecafebabecafebabecafebabecafebabecafebabecafebabecafebabe",
                                              9, NULL, "wss://g.example", &b);
  CHECK(n && b.withdraw_id);
  CHECK(strcmp(nostr_notify_group_fixed_body(), "New message in this group.") == 0);
  nostr_notify_build_dispose(&b);
  g_object_unref(n);

  fprintf(stderr, "test_notify_prefs: OK\n");
  return 0;
}
