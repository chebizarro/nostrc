#ifndef LIBNOSTR_STORE_NOSTRDB_NDB_BACKEND_H
#define LIBNOSTR_STORE_NOSTRDB_NDB_BACKEND_H

#include "store_int.h"

/* Internal backend-specific state */
struct ln_ndb_impl {
  void *db;   /* placeholder for nostrdb handle */
};

/* Exported to registry when LIBNOSTR_WITH_NOSTRDB */
const struct ln_store_ops *ln_ndb_get_ops(void);
#endif /* LIBNOSTR_STORE_NOSTRDB_NDB_BACKEND_H */
