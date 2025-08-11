#include <gio/gio.h>
#include "nostr/nip5f/nip5f.h"

/* Minimal GLib client wrapper; full implementation pending */

typedef struct _NostrSockSignerClient {
  GObject parent_instance;
} NostrSockSignerClient;

G_DEFINE_TYPE(NostrSockSignerClient, nostr_sock_signer_client, G_TYPE_OBJECT)

static void nostr_sock_signer_client_class_init(NostrSockSignerClientClass *klass) {
  (void)klass;
}

static void nostr_sock_signer_client_init(NostrSockSignerClient *self) {
  (void)self;
}
