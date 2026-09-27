/*
 * notify_sound.c — see notify_sound.h.
 */
#include "notify_sound.h"

#include <dlfcn.h>
#include <glib.h>
#include <stdint.h>

typedef struct ca_context ca_context;
typedef int (*ca_context_create_fn)(ca_context **c);
typedef int (*ca_context_play_fn)(ca_context *c, uint32_t id, ...);

static ca_context *g_ca;
static ca_context_play_fn g_play;
static int g_state; /* 0 untried, 1 ready, -1 unavailable */

bool nostr_notify_play_sound(void) {
  if (g_state == 0) {
    g_state = -1;
    void *h = dlopen("libcanberra.so.0", RTLD_NOW | RTLD_LOCAL);
    ca_context_create_fn create = h ? (ca_context_create_fn)dlsym(h, "ca_context_create") : NULL;
    g_play = h ? (ca_context_play_fn)dlsym(h, "ca_context_play") : NULL;
    if (create && g_play && create(&g_ca) == 0 && g_ca) {
      g_state = 1;
    } else {
      g_message("nostr-notify: sound = true, but libcanberra is not available; "
                "notifications stay silent");
    }
  }
  if (g_state < 0) return false;
  /* event.id per the freedesktop sound theme spec */
  return g_play(g_ca, 0, "event.id", "message-new-instant",
                "event.description", "Nostr message", NULL) == 0;
}
