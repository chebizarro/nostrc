#ifndef FIPSD_NOSTR_TRAVERSAL_IO_H
#define FIPSD_NOSTR_TRAVERSAL_IO_H
#include "nostr/driver.h"
typedef struct fipsd_traversal_io fipsd_traversal_io;
struct fipsd_nostr_runtime;
/* Owned by the rendezvous subsystem. Register classifiers before node start;
 * destroy only after the host runtime and all UDP drains have joined. */
fipsd_traversal_io *fipsd_traversal_io_create(const fipsd_config *, fipsd_nostr_node_driver *, struct fipsd_nostr_runtime *);
int fipsd_traversal_io_start(fipsd_traversal_io *, fipsd_runtime *, const fips_identity *);
void fipsd_traversal_io_stop(fipsd_traversal_io *);
void fipsd_traversal_io_destroy(fipsd_traversal_io *);
bool fipsd_traversal_io_connect(fipsd_traversal_io *, const char *);
bool fipsd_traversal_io_signal(fipsd_traversal_io *, const fipsd_nostr_traversal *);
void fipsd_traversal_io_published(fipsd_traversal_io *, const char *, bool accepted, bool repair);
bool fipsd_traversal_io_advert(fipsd_traversal_io *, fipsd_nostr_advert *);
#endif
