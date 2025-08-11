#pragma once
#include <glib.h>

typedef struct _AccountsStore AccountsStore;

typedef struct {
  gchar *id;     /* identity selector: key_id or npub */
  gchar *label;  /* optional display label */
} AccountEntry;

AccountsStore *accounts_store_new(void);
void accounts_store_free(AccountsStore *as);
void accounts_store_load(AccountsStore *as);
void accounts_store_save(AccountsStore *as);

/* returns FALSE if id already exists */
gboolean accounts_store_add(AccountsStore *as, const gchar *id, const gchar *label);

gboolean accounts_store_remove(AccountsStore *as, const gchar *id);

GPtrArray *accounts_store_list(AccountsStore *as); /* array of AccountEntry*; caller frees fields */

void accounts_store_set_active(AccountsStore *as, const gchar *id);
/* returns FALSE if no active set; if out_id provided, it's newly allocated */
gboolean accounts_store_get_active(AccountsStore *as, gchar **out_id);
