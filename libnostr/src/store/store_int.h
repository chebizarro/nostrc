#ifndef LIBNOSTR_STORE_STORE_INT_H
#define LIBNOSTR_STORE_STORE_INT_H

#include "libnostr_store.h"

/* Internal representation shared by store core and backends. Not installed. */
struct ln_store {
  const ln_store_ops *ops;
  void *impl; /* backend-specific */
};
#endif /* LIBNOSTR_STORE_STORE_INT_H */
