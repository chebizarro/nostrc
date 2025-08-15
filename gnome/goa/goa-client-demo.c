#include <goa/goa.h>
#include <gio/gio.h>
#include <stdio.h>

int main(void){
  GError *err=NULL; GoaClient *client = goa_client_new_sync(NULL, &err);
  if (!client){ fprintf(stderr, "goa_client_new_sync failed: %s\n", err?err->message:"?" ); g_clear_error(&err); return 1; }
  GList *accounts = goa_client_get_accounts(client);
  for (GList *l=accounts; l; l=l->next){
    GoaObject *obj = GOA_OBJECT(l->data);
    GoaAccount *acc = goa_object_peek_account(obj);
    if (!acc) continue;
    const char *provider_type = goa_account_get_provider_type(acc);
    if (g_strcmp0(provider_type, "Gnostr")!=0) continue;
    const char *presentation = goa_account_get_presentation_identity(acc);
    g_print("Gnostr: %s\n", presentation?presentation:"(no name)");

    // Example: read custom properties if present in the keyfile or GSettings (not standardized here)
    // In a real integration, we’d look under our schema and account-specific path.
  }
  g_list_free_full(accounts, g_object_unref);
  g_object_unref(client);
  return 0;
}
