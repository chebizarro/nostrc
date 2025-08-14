#include <string.h>
#include <stdlib.h>
#include "libnostr_store.h"
#include "store_int.h"

/* Forward decl for nostrdb backend ops getter (only when enabled) */
#ifdef LIBNOSTR_WITH_NOSTRDB
const ln_store_ops *ln_ndb_get_ops(void);
#endif

static const ln_store_ops *lookup_backend_ops(const char *backend)
{
  if (!backend) return NULL;
#ifdef LIBNOSTR_WITH_NOSTRDB
  if (strcmp(backend, "nostrdb") == 0) return ln_ndb_get_ops();
#endif
  return NULL;
}

int ln_store_open(const char *backend, const char *path, const char *opts_json, ln_store **out)
{
  if (!out) return LN_ERR_OOM;
  *out = NULL;
  const ln_store_ops *ops = lookup_backend_ops(backend);
  if (!ops) return LN_ERR_BACKEND_NOT_FOUND;
  ln_store *h = NULL;
  int rc = ops->open(&h, path, opts_json);
  if (rc != LN_OK) return rc;
  if (!h) return LN_ERR_DB_OPEN;
  h->ops = ops;
  *out = h;
  return LN_OK;
}

void ln_store_close(ln_store *s)
{
  if (!s) return;
  if (s->ops && s->ops->close) s->ops->close(s);
}
