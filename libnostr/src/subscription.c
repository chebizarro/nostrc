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

void *subscription_thread_func(void *arg) {
	Subscription *sub = (Subscription *)arg;
	pthread_mutex_lock(&sub->priv->sub_mutex);

	pthread_mutex_unlock(&sub->priv->sub_mutex);

	return NULL;
}

void subscription_start(Subscription *sub) {
	go(subscription_thread_func, sub->events);
	while (!sub->priv->context->canceled) {

	}
	subscription_unsub(sub);
	// go_channel_close(?)
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
	if (sub->relay->priv->ssl) {
	const char *id = sub->priv->label; // Simplified; use proper ID generation logic
	char close_msg[256];
	snprintf(close_msg, sizeof(close_msg), "{\"type\":\"CLOSE\",\"id\":\"%s\"}", id);
	SSL_write(sub->relay->priv->ssl, close_msg, strlen(close_msg));
	}
}

void subscription_sub(Subscription *sub, Filters *filters) {
	sub->filters = *filters;
	subscription_fire(sub);
}

void subscription_fire(Subscription *sub) {
	const char *id = sub->priv->label; // Simplified; use proper ID generation logic
	char req_msg[512];
	snprintf(req_msg, sizeof(req_msg), "{\"type\":\"REQ\",\"id\":\"%s\",\"filters\":[...]}",
		 id); // Simplified; serialize filters properly
	SSL_write(sub->relay->priv->ssl, req_msg, strlen(req_msg));
	sub->priv->live = true;
}