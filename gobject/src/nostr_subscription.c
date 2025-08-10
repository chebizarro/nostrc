#include "nostr_subscription.h"
#include "nostr_relay.h"
#include "nostr_filter.h"
#include <glib.h>
#include "nostr-subscription.h"  /* core API */
#include "nostr-filter.h"               /* core Filters helper (legacy names) */

/* GNostrSubscription GObject implementation */
G_DEFINE_TYPE(GNostrSubscription, gnostr_subscription, G_TYPE_OBJECT)

static void gnostr_subscription_finalize(GObject *object) {
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(object);
    if (self->subscription) {
        nostr_subscription_free(self->subscription);
        self->subscription = NULL;
    }
    G_OBJECT_CLASS(gnostr_subscription_parent_class)->finalize(object);
}

static void gnostr_subscription_class_init(GNostrSubscriptionClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_subscription_finalize;
}

static void gnostr_subscription_init(GNostrSubscription *self) {
    self->subscription = NULL;
}

GNostrSubscription *gnostr_subscription_new(GNostrRelay *relay, NostrFilter *filter) {
    g_return_val_if_fail(relay != NULL, NULL);
    GNostrSubscription *self = g_object_new(GNOSTR_TYPE_SUBSCRIPTION, NULL);
    /* Build a Filters list containing one filter from the GLib wrapper */
    Filters *fs = create_filters();
    if (filter) {
        filters_add(fs, &filter->filter);
    }
    self->subscription = nostr_subscription_new(relay->relay, (NostrFilters *)fs);
    return self;
}

void gnostr_subscription_unsubscribe(GNostrSubscription *self) {
    g_return_if_fail(self != NULL && self->subscription != NULL);
    nostr_subscription_unsubscribe(self->subscription);
}