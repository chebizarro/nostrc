#include "../include/nostr/nip77/negentropy-glib.h"
#if defined(NOSTR_HAVE_GLIB)

struct _NostrNegSessionG { GObject parent_instance; };
struct _NostrNegSessionGClass { GObjectClass parent_class; };

G_DEFINE_TYPE(NostrNegSessionG, nostr_neg_session_g, G_TYPE_OBJECT)

static void nostr_neg_session_g_class_init(NostrNegSessionGClass *klass) {
  (void)klass;
}
static void nostr_neg_session_g_init(NostrNegSessionG *self) {
  (void)self;
}

NostrNegSessionG *nostr_neg_session_g_new(void) {
  return g_object_new(NOSTR_TYPE_NEG_SESSION_G, NULL);
}

#endif /* NOSTR_HAVE_GLIB */
