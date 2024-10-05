#ifndef CONNECTION_H
#define CONNECTION_H

#include "nostr.h"
#include <zlib.h>

struct _ConnectionPrivate {
  struct lws *wsi;
  int enable_compression;
  z_stream zstream;
};

#endif // CONNECTION_H