#ifndef RELAYD_ASYNC_STORAGE_H
#define RELAYD_ASYNC_STORAGE_H

#include "nostr-storage.h"

/* Optional relay-only write interface. enqueue() must return after handing
 * the event to the backend; notify() is called only after a writer commit and
 * may run on a worker thread. The relay checks visible() on its LWS thread
 * before it emits OK true. No libwebsockets API except lws_cancel_service()
 * may be called by notify(). */
typedef struct RelaydAsyncStorageOps {
  int (*start)(NostrStorage *st, void (*notify)(void *), void *ctx);
  void (*stop)(NostrStorage *st);
  int (*enqueue)(NostrStorage *st, const NostrEvent *ev);
  int (*visible)(NostrStorage *st, const unsigned char id[32]);
  void (*drain)(NostrStorage *st);
} RelaydAsyncStorageOps;

#endif
