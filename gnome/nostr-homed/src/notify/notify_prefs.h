/*
 * notify_prefs — presentation keys of nostr-notify.conf [notify]
 * (nostrc-prqu.16), written by Nostr Settings' Notifications page:
 *
 *   notify_groups  bool, default true   NIP-29 group messages (kinds 9-12)
 *   notify_dms     bool, default true   NIP-17 gift wraps (kind 1059) to you
 *   group_preview  bool, default true   false = fixed body, like DMs
 *   sound          bool, default false  play the desktop's message sound
 *
 * DM bodies stay opaque regardless (§3.3 D3 F14). A malformed value keeps
 * the default (and is logged), so a typo never silences notifications.
 */
#ifndef NOSTR_NOTIFY_PREFS_H
#define NOSTR_NOTIFY_PREFS_H

#include <glib.h>
#include <stdbool.h>

typedef struct {
  bool notify_groups;
  bool notify_dms;
  bool group_preview;
  bool sound;
} NostrNotifyPrefs;

void nostr_notify_prefs_defaults(NostrNotifyPrefs *p);
/* Reads [notify] from @kf (NULL = defaults only). */
void nostr_notify_prefs_load(GKeyFile *kf, NostrNotifyPrefs *p);

#endif /* NOSTR_NOTIFY_PREFS_H */
