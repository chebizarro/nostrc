#ifndef NOSTR_H
#define NOSTR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "go.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define NON_NULL(...) __attribute__((nonnull(__VA_ARGS__)))
#define MALLOC __attribute__((malloc))
#define CLEANUP(func) __attribute__((cleanup(func)))
#define NON_NULL_MALLOC(...) __attribute__((nonnull(__VA_ARGS__), malloc))

typedef int64_t Timestamp;

Timestamp Now();
time_t TimestampToTime(Timestamp t);

// Key generation and validation
char *generate_private_key();
char *get_public_key(const char *sk);
bool is_valid_public_key_hex(const char *pk);
bool is_valid_public_key(const char *pk);

typedef struct _Tag {
    char **elements;
    size_t count;
} Tag;

typedef struct _Tags {
    Tag **data;
    size_t count;
} Tags;

Tag *create_tag(const char *key, ...) MALLOC;
void free_tag(Tag *tag);
bool tag_starts_with(Tag *tag, Tag *prefix);
char *tag_key(Tag *tag);
char *tag_value(Tag *tag);
char *tag_relay(Tag *tag);
char *tag_marshal_to_json(Tag *tag);

Tags *create_tags(size_t count, ...) MALLOC;
void free_tags(Tags *tags);
char *tags_get_d(Tags *tags);
Tag *tags_get_first(Tags *tags, Tag *prefix);
Tag *tags_get_last(Tags *tags, Tag *prefix);
Tags *tags_get_all(Tags *tags, Tag *prefix) MALLOC;
Tags *tags_filter_out(Tags *tags, Tag *prefix) MALLOC;
Tags *tags_append_unique(Tags *tags, Tag *tag) MALLOC;
bool tags_contains_any(Tags *tags, const char *tag_name, char **values, size_t values_count);
char *tags_marshal_to_json(Tags *tags);

// Define the NostrEvent structure
typedef struct _NostrEvent {
    char *id;
    char *pubkey;
    int64_t created_at;
    int kind;
    Tags *tags;
    char *content;
    char *sig;
    void *extra; // Extra fields
} NostrEvent;

// Function prototypes for NostrEvent management
NostrEvent *create_event() MALLOC;
void free_event(NostrEvent *event);
char *event_serialize(NostrEvent *event);
char *event_get_id(NostrEvent *event) MALLOC;
bool event_check_signature(NostrEvent *event);
int event_sign(NostrEvent *event, const char *private_key);

typedef struct _Filter {
    char **ids;
    size_t ids_count;
    int *kinds;
    size_t kinds_count;
    char **authors;
    size_t authors_count;
    Tags *tags;
    Timestamp *since;
    Timestamp *until;
    int limit;
    char *search;
    bool limit_zero;
} Filter;

typedef struct _Filters {
    Filter *filters;
    size_t count;
} Filters;

Filter *create_filter() MALLOC;
void free_filter(Filter *filter);
Filters *create_filters(size_t count) MALLOC;
void free_filters(Filters *filters);
bool filter_matches(Filter *filter, NostrEvent *event);
bool filter_match_ignoring_timestamp(Filter *filter, NostrEvent *event);
bool filters_match(Filters *filters, NostrEvent *event);

typedef struct _ConnectionPrivate ConnectionPrivate;

typedef struct _Connection {
    ConnectionPrivate *priv;
} Connection;

typedef struct _RelayPrivate RelayPrivate;

typedef struct Relay {
    RelayPrivate *priv;
    char *url;
    //Subscription *subscriptions;
} Relay;

Relay *create_relay(const char *url) MALLOC;
void free_relay(Relay *relay);
int relay_connect(Relay *relay);
void relay_disconnect(Relay *relay);
int relay_subscribe(Relay *relay, Filters *filters);
void relay_unsubscribe(Relay *relay, int subscription_id);
void relay_publish(Relay *relay, NostrEvent *event);
void relay_auth(Relay *relay, void (*sign)(NostrEvent *));
bool relay_is_connected(Relay *relay);

// Structure to hold JSON function pointers
typedef struct _NostrJsonInterface {
    void (*init)(void);
    void (*cleanup)(void);
    char *(*serialize)(const NostrEvent *event);
    NostrEvent *(*deserialize)(const char *json_str);
} NostrJsonInterface;

extern NostrJsonInterface *json_interface;
void nostr_set_json_interface(NostrJsonInterface *interface);

typedef struct _Channel Channel;
typedef struct _SubscriptionPrivate SubscriptionPrivate;

typedef struct Subscription {
    SubscriptionPrivate *priv;
	char *id;
    Relay *relay;
    Filters *filters;
    GoChannel *events;
    GoChannel *closed_reason;
} Subscription;

Subscription *create_subscription(Relay *relay, Filters *filters, const char *label) MALLOC;
void free_subscription(Subscription *sub);
char *subscription_get_id(Subscription *sub);
void subscription_unsub(Subscription *sub);
void subscription_close(Subscription *sub);
void subscription_sub(Subscription *sub, Filters *filters);
void subscription_fire(Subscription *sub);

#ifdef __cplusplus
}
#endif

#endif // NOSTR_H