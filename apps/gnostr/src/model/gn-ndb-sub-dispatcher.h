#ifndef GN_NDB_SUB_DISPATCHER_H
#define GN_NDB_SUB_DISPATCHER_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef void (*GnNdbSubBatchFn)(
  uint64_t subid,
  const uint64_t *note_keys,
  guint n_keys,
  gpointer user_data
);

/* Initialize the dispatcher and register callback with storage_ndb.
 * MUST be called before storage_ndb_init() for notifications to work. */
void gn_ndb_dispatcher_init(void);

uint64_t gn_ndb_subscribe(
  const char *filter_json,
  GnNdbSubBatchFn cb,
  gpointer user_data,
  GDestroyNotify destroy
);

void gn_ndb_unsubscribe(uint64_t subid);

G_END_DECLS

#endif /* GN_NDB_SUB_DISPATCHER_H */