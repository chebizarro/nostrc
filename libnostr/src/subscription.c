#include "nostr.h"
#include "subscription-private.h"
#include "relay-private.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/ssl.h>

// Subscription-related functions
Subscription *create_subscription(Relay *relay, Filters *filters, const char *label) {
	Subscription *sub = (Subscription *)malloc(sizeof(Subscription));
	if (!sub)
	return NULL;

	sub->priv->label = strdup(label);
	sub->priv->counter = 0;
	sub->relay = relay;
	sub->filters = *filters;
	sub->priv->countResult = NULL;
	sub->events = NULL;
	sub->closed_reason = NULL;
	sub->priv->live = false;
	sub->priv->eosed = false;
	sub->priv->closed = false;
	pthread_mutex_init(&sub->priv->sub_mutex, NULL);

	return sub;
}

void free_subscription(Subscription *sub) {
	free(sub->priv->label);
	free(sub->closed_reason);
	go_channel_free(sub->events);
	pthread_mutex_destroy(&sub->priv->sub_mutex);
	free(sub);
}

char *subscription_get_id(Subscription *sub) {
	return sub->id;
} 

void *subscription_thread_func(void *arg) {
	pthread_mutex_lock(&sub->priv->sub_mutex);

	pthread_mutex_unlock(&sub->priv->sub_mutex);
	return NULL;
}

void subscription_start(void *arg) {
	Subscription *sub = (Subscription *)arg;
	while (!go_context_is_canceled(sub->priv->context)) { }
	subscription_unsub(sub);
	// go_channel_close(sub->events)
}

void subscription_dispatch_event(Subscription *sub, NostrEvent *event) {

	pthread_mutex_lock(&sub->priv->sub_mutex);
	//sub->events = (NostrEvent **)realloc(sub->events, (sub->event_count + 1) * sizeof(NostrEvent *));
	//sub->events[sub->priv->event_count++] = event;
	pthread_mutex_unlock(&sub->priv->sub_mutex);
}

void subscription_dispatch_eose(Subscription *sub) {
	atomic_store(&sub->priv->eosed, true);
	sub->priv->eosed = true;
}

void subscription_dispatch_closed(Subscription *sub, const char *reason) {
	atomic_store(&sub->priv->closed, true);
	free(sub->closed_reason);
	sub->closed_reason = strdup(reason);
}

void subscription_unsub(Subscription *sub) {
	atomic_store(&sub->priv->live, false);
	pthread_cancel(sub->priv->thread);
	subscription_close(sub);
}

void subscription_close(Subscription *sub) {
	if (relay_is_connected(sub->relay)) {
		Envelope *close_msg = NewEnvelope(sub->id);
		*char close_b = envelope_marshal_json(close_msg);
		relay_write(sub.relay, close_b);
	}
}

void subscription_sub(Subscription *sub, Filters *filters) {
	sub->filters = filters;
	subscription_fire(sub);
}

static void *sub_error(void *arg) {

	subscription_cancel();

}

void subscription_fire(Subscription *sub) {
	Envelope *req;
	if (sub->count_result) {
		req = NewEnvelope();	
	} else {
		req = NewEnvelope();
	}

	atomic_store(&sub->priv->live, true);

	GoChannel *err = (sub->relay, data);

	go(sub_error, err);


	char req_msg[512];
	snprintf(req_msg, sizeof(req_msg), "{\"type\":\"REQ\",\"id\":\"%s\",\"filters\":[...]}",
		 id); // Simplified; serialize filters properly
	sub->priv->live = true;
}