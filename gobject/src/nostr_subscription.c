#include "nostr_subscription.h"
#include "nostr_relay.h"
#include "nostr_filter.h"
#include <glib.h>

/* NostrSubscription GObject implementation */
G_DEFINE_TYPE(NostrSubscription, nostr_subscription, G_TYPE_OBJECT)

static void nostr_subscription_finalize(GObject *object) {
    NostrSubscription *self = NOSTR_SUBSCRIPTION(object);
    if (self->subscription) {
        subscription_free(self->subscription);
    }
    G_OBJECT_CLASS(nostr_subscription_parent_class)->finalize(object);
}

static void nostr_subscription_class_init(NostrSubscriptionClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = nostr_subscription_finalize;
}

static void nostr_subscription_init(NostrSubscription *self) {
    self->subscription = NULL;
}

NostrSubscription *nostr_subscription_new(NostrRelay *relay, NostrFilter *filter) {
    NostrSubscription *self = g_object_new(NOSTR_TYPE_SUBSCRIPTION, NULL);
    self->subscription = subscription_new(relay->relay, &filter->filter);
    return self;
}

void nostr_subscription_unsubscribe(NostrSubscription *self) {
    subscription_unsubscribe(self->subscription);
}