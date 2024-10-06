#ifndef CONNECTION_PRIVATE_H
#define CONNECTION_PRIVATE_H

#include <libwebsockets.h>

struct _ConnectionPrivate {
  struct lws *wsi;
  int enable_compression;
  struct lws_context *context;
};

#endif // CONNECTION_PRIVATE_H