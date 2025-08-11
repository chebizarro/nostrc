#include <gio/gio.h>
#include "nostr/nip5f/nip5f.h"

/* Minimal GLib wrappers to satisfy build; full implementation pending */

typedef struct _NostrSockSignerServer {
  GObject parent_instance;
} NostrSockSignerServer;

G_DEFINE_TYPE(NostrSockSignerServer, nostr_sock_signer_server, G_TYPE_OBJECT)

static void nostr_sock_signer_server_class_init(NostrSockSignerServerClass *klass) {
  (void)klass;
}

static void nostr_sock_signer_server_init(NostrSockSignerServer *self) {
  (void)self;
}
