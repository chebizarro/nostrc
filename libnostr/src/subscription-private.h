#ifndef SUBSCRIPTION_H
#define SUBSCRIPTION_H

#include "nostr.h"
#include "channel.h"
#include <stdatomic.h>

typedef struct _SubscriptionPrivate {
	char *label;
	int counter;

	GoChannel *countResult;
	GoContext *context;

	_Atomic bool live;
	_Atomic bool eosed;
	_Atomic bool closed;
	pthread_mutex_t sub_mutex;
	pthread_t thread;
} SubscriptionPrivate;

Subscription *create_subscription(Relay *relay, Filters *filters, const char *label);
void subscription_start(Subscription *sub);
void subscription_dispatch_event(Subscription *sub, NostrEvent *event);
void subscription_dispatch_eose(Subscription *sub);
void subscription_dispatch_closed(Subscription *sub, const char *reason);

#endif // SUBSCRIPTION_H