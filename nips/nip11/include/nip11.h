#ifndef NOSTR_NIP11_H
#define NOSTR_NIP11_H

#include <stdbool.h>

typedef struct {
    int amount;
    char *unit;
} Fee;

typedef struct {
    int count;
    Fee *items;
} Fees;

typedef struct {
    int *kinds;
    int count;
    int amount;
    char *unit;
} PublicationFee;

typedef struct {
    Fees admission;
    Fees subscription;
    PublicationFee publication;
} RelayFeesDocument;

typedef struct {
    int max_message_length;
    int max_subscriptions;
    int max_filters;
    int max_limit;
    int max_subid_length;
    int max_event_tags;
    int max_content_length;
    int min_pow_difficulty;
    bool auth_required;
    bool payment_required;
    bool restricted_writes;
} RelayLimitationDocument;

typedef struct {
    char *url;
    char *name;
    char *description;
    char *pubkey;
    char *contact;
    int *supported_nips;
    int supported_nips_count;
    char *software;
    char *version;
    RelayLimitationDocument *limitation;
    char **relay_countries;
    int relay_countries_count;
    char **language_tags;
    int language_tags_count;
    char **tags;
    int tags_count;
    char *posting_policy;
    char *payments_url;
    RelayFeesDocument *fees;
    char *icon;
} RelayInformationDocument;

RelayInformationDocument* fetch_relay_info(const char *url);

void free_relay_info(RelayInformationDocument *info);

#endif // NOSTR_NIP11_H
