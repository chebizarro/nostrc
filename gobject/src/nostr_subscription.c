#include "nostr_subscription.h"
#include "nostr_relay.h"
#include "nostr_filter.h"
#include <glib.h>
#include "nostr-subscription.h"  /* core API */
#include "nostr-filter.h"               /* core Filters helper (legacy names) */

/* Private structure definition (G_DECLARE_FINAL_TYPE requires this in .c file) */
struct _GNostrSubscription {
    GObject parent_instance;

    /*< private >*/
    NostrSubscription *subscription;      /* core subscription pointer */
    NostrSubscriptionState state;         /* current lifecycle state */
    NostrSubscriptionConfig config;       /* subscription configuration */
    gchar *error_message;                 /* error message if state is ERROR */
    guint event_count;                    /* number of events received */
};

/* GNostrSubscription GObject implementation */
G_DEFINE_TYPE(GNostrSubscription, gnostr_subscription, G_TYPE_OBJECT)

static void gnostr_subscription_finalize(GObject *object) {
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(object);
    if (self->subscription) {
        nostr_subscription_free(self->subscription);
        self->subscription = NULL;
    }
    g_free(self->error_message);
    self->error_message = NULL;
    G_OBJECT_CLASS(gnostr_subscription_parent_class)->finalize(object);
}

static void gnostr_subscription_class_init(GNostrSubscriptionClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_subscription_finalize;
}

static void gnostr_subscription_init(GNostrSubscription *self) {
    self->subscription = NULL;
    self->state = NOSTR_SUBSCRIPTION_STATE_PENDING;
    self->config = NOSTR_SUBSCRIPTION_CONFIG_DEFAULT;
    self->error_message = NULL;
    self->event_count = 0;
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
    self->state = NOSTR_SUBSCRIPTION_STATE_CLOSED;
}

GNostrSubscription *gnostr_subscription_new_with_config(GNostrRelay *relay,
                                                         NostrFilter *filter,
                                                         NostrSubscriptionConfig *config) {
    g_return_val_if_fail(relay != NULL, NULL);
    GNostrSubscription *self = g_object_new(GNOSTR_TYPE_SUBSCRIPTION, NULL);

    if (config) {
        self->config = *config;
    } else {
        self->config = NOSTR_SUBSCRIPTION_CONFIG_DEFAULT;
    }
    self->state = NOSTR_SUBSCRIPTION_STATE_PENDING;

    /* Build a Filters list containing one filter from the GLib wrapper */
    Filters *fs = create_filters();
    if (filter) {
        filters_add(fs, &filter->filter);
    }
    self->subscription = nostr_subscription_new(relay->relay, (NostrFilters *)fs);
    return self;
}

NostrSubscriptionState gnostr_subscription_get_state(GNostrSubscription *self) {
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), NOSTR_SUBSCRIPTION_STATE_ERROR);
    return self->state;
}

const NostrSubscriptionConfig *gnostr_subscription_get_config(GNostrSubscription *self) {
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), NULL);
    return &self->config;
}

const gchar *gnostr_subscription_get_error_message(GNostrSubscription *self) {
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), NULL);
    return self->error_message;
}

guint gnostr_subscription_get_event_count(GNostrSubscription *self) {
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), 0);
    return self->event_count;
}