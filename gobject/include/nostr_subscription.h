#ifndef NOSTR_GSUBSCRIPTION_H
#define NOSTR_GSUBSCRIPTION_H

#include <glib-object.h>
#include "nostr-subscription.h"   /* core NostrSubscription APIs */
#include "nostr_relay.h"          /* GNostrRelay wrapper */
#include "nostr_filter.h"         /* GLib filter wrapper (temporary name) */

/* Define GNostrSubscription GObject (G-prefixed to avoid clashing with core) */
#define GNOSTR_TYPE_SUBSCRIPTION (gnostr_subscription_get_type())
G_DECLARE_FINAL_TYPE(GNostrSubscription, gnostr_subscription, GNOSTR, SUBSCRIPTION, GObject)

struct _GNostrSubscription {
    GObject parent_instance;
    NostrSubscription *subscription; /* core subscription pointer */
};

/* GObject convenience API */
GNostrSubscription *gnostr_subscription_new(GNostrRelay *relay, NostrFilter *filter);
void gnostr_subscription_unsubscribe(GNostrSubscription *self);

#endif // NOSTR_GSUBSCRIPTION_H