/*
 * notify_sound — the desktop's "message-new-instant" sound for the
 * `sound` preference (GNotification has no sound API).
 *
 * libcanberra is loaded at runtime (dlopen "libcanberra.so.0"), so the
 * daemon has no build or package dependency on it; without it the key is
 * a no-op and the daemon says so once in the journal.
 */
#ifndef NOSTR_NOTIFY_SOUND_H
#define NOSTR_NOTIFY_SOUND_H

#include <stdbool.h>

/* Fire-and-forget; main thread. Returns false when no sound could be played. */
bool nostr_notify_play_sound(void);

#endif /* NOSTR_NOTIFY_SOUND_H */
