#ifndef NOSTR_GSUBSCRIPTION_H
#define NOSTR_GSUBSCRIPTION_H

#include <glib-object.h>
#include "subscription.h"

/* Define NostrSubscription GObject */
#define NOSTR_TYPE_SUBSCRIPTION (nostr_subscription_get_type())
G_DECLARE_FINAL_TYPE(NostrSubscription, nostr_subscription, NOSTR, SUBSCRIPTION, GObject)

struct _NostrSubscription {
    GObject parent_instance;
    Subscription *subscription;
};

NostrSubscription *nostr_subscription_new(NostrRelay *relay, NostrFilter *filter);
void nostr_subscription_unsubscribe(NostrSubscription *self);

#endif // NOSTR_GSUBSCRIPTION_H