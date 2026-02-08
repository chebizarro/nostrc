#ifndef NOSTR_STORAGE_H
#define NOSTR_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Forward decls */
typedef struct _NostrEvent NostrEvent;
typedef struct NostrFilter NostrFilter;

typedef struct NostrStorage NostrStorage;

typedef struct {
  int  (*open)(NostrStorage*, const char *uri, const char *opts_json);
  void (*close)(NostrStorage*);

  /* Write path */
  int  (*put_event)(NostrStorage*, const NostrEvent *ev);
  int  (*ingest_ldjson)(NostrStorage*, const char *ldjson, size_t len);
  int  (*delete_event)(NostrStorage*, const char *id_hex);

  /* Read path (iterator-based) */
  void* (*query)(NostrStorage*, const NostrFilter *filters, size_t nfilters,
                 size_t limit, uint64_t since, uint64_t until, int *err);
  int   (*query_next)(NostrStorage*, void *it, NostrEvent *out, size_t *n);
  void  (*query_free)(NostrStorage*, void *it);

  /* NIP-45 */
  int (*count)(NostrStorage*, const NostrFilter *filters, size_t nfilters, uint64_t *out);

  /* NIP-50 (optional; return ENOTSUP if unimplemented) */
  int (*search)(NostrStorage*, const char *q, const NostrFilter *scope, size_t limit, void **it_out);

  /* NIP-77 (scaffold) */
  int  (*set_digest)(NostrStorage*, const NostrFilter *scope, void **state);
  int  (*set_reconcile)(NostrStorage*, void *state, const void *peer_msg, size_t len,
                        void **resp, size_t *resp_len);
  void (*set_free)(NostrStorage*, void *state);

  /* Relay-aware ingestion (optional; NULL means relay provenance not recorded) */
  int (*put_event_with_relay)(NostrStorage*, const NostrEvent *ev, const char *relay);
  int (*ingest_ldjson_with_relay)(NostrStorage*, const char *ldjson, size_t len, const char *relay);
} NostrStorageVTable;

struct NostrStorage { NostrStorageVTable *vt; void *impl; };

/* Minimal registry so apps can choose driver at runtime if they linked multiple */
typedef NostrStorage* (*NostrStorageFactory)(void);

int nostr_storage_register(const char *name, NostrStorageFactory make);
NostrStorage* nostr_storage_create(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_STORAGE_H */
