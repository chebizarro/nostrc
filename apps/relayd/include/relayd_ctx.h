#ifndef RELAYD_CTX_H
#define RELAYD_CTX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "relayd_config.h"
#include "nostr-storage.h"

typedef struct {
  NostrStorage *storage;
  RelaydConfig cfg;
} RelaydCtx;

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_CTX_H */
