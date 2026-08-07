#ifndef NOSTR_NIP_COMMUNIKEYS_H
#define NOSTR_NIP_COMMUNIKEYS_H

#include "nostr-event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical NIP-CAS-0007 registry symbols. */
#ifndef CAS_COMMUNITY_DEFINITION
#define CAS_COMMUNITY_DEFINITION ((uint16_t)10222)
#endif
#ifndef CAS_TARGETED_PUBLICATION
#define CAS_TARGETED_PUBLICATION ((uint16_t)30222)
#endif

#define NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST 30000
#define NOSTR_COMMUNIKEYS_KIND_BADGE_DEFINITION 30009
#define NOSTR_COMMUNIKEYS_MAX_TARGETS 12

typedef enum {
    NOSTR_COMMUNIKEYS_OK = 0,
    NOSTR_COMMUNIKEYS_ERR_NULL,
    NOSTR_COMMUNIKEYS_ERR_OOM,
    NOSTR_COMMUNIKEYS_ERR_WRONG_KIND,
    NOSTR_COMMUNIKEYS_ERR_BAD_PUBKEY,
    NOSTR_COMMUNIKEYS_ERR_BAD_CONTENT,
    NOSTR_COMMUNIKEYS_ERR_BAD_TAG,
    NOSTR_COMMUNIKEYS_ERR_CARDINALITY,
    NOSTR_COMMUNIKEYS_ERR_MISSING_RELAY,
    NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER,
    NOSTR_COMMUNIKEYS_ERR_SECTION_NAME,
    NOSTR_COMMUNIKEYS_ERR_SECTION_KIND,
    NOSTR_COMMUNIKEYS_ERR_SECTION_ACL,
    NOSTR_COMMUNIKEYS_ERR_DUPLICATE_ASSIGNMENT,
    NOSTR_COMMUNIKEYS_ERR_BAD_COORDINATE,
    NOSTR_COMMUNIKEYS_ERR_BAD_REFERENCE,
    NOSTR_COMMUNIKEYS_ERR_BAD_TARGETS,
    NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH,
    NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH,
    NOSTR_COMMUNIKEYS_ERR_BAD_URI
} nostr_communikeys_status_t;

const char *nostr_communikeys_status_string(nostr_communikeys_status_t status);
bool nostr_communikeys_is_lower_hex_32(const char *value);

typedef struct {
    int kind;
    char *pubkey;
    char *identifier;
    char *relay;
} nostr_communikeys_coordinate_t;

bool nostr_communikeys_coordinate_parse(const char *value,
                                        const char *relay,
                                        nostr_communikeys_coordinate_t *out);
char *nostr_communikeys_coordinate_format(const nostr_communikeys_coordinate_t *coordinate);
void nostr_communikeys_coordinate_clear(nostr_communikeys_coordinate_t *coordinate);

typedef struct {
    int kind;
    char *subtype; /* NULL and the empty string both mean the exact empty subtype. */
} nostr_communikeys_assignment_t;

typedef struct {
    char *name;
    nostr_communikeys_assignment_t *assignments;
    size_t assignments_len;
    nostr_communikeys_coordinate_t profile_list;
    char **badges;
    size_t badges_len;
} nostr_communikeys_section_t;

typedef struct {
    char *url;
    char *protocol;
} nostr_communikeys_mint_t;

typedef struct {
    char *pubkey;
    char **relays;
    size_t relays_len;
    char **blossom_servers;
    size_t blossom_servers_len;
    char **grasp_servers;
    size_t grasp_servers_len;
    nostr_communikeys_mint_t *mints;
    size_t mints_len;
    char *tos;
    char *tos_relay;
    char *location;
    char *geohash;
    char *description;
    nostr_communikeys_section_t *sections;
    size_t sections_len;

    /* Defensive parsing always preserves at most one resolution result.
     * Invalid definitions remain inspectable but MUST NOT be used for access. */
    bool valid;
    nostr_communikeys_status_t validation_status;
} nostr_communikeys_definition_t;

void nostr_communikeys_definition_clear(nostr_communikeys_definition_t *definition);

/* Parses a kind-10222 event defensively. Returns false only for a wrong kind,
 * invalid arguments, or allocation failure. Inspect out->valid before use. */
bool nostr_communikeys_definition_parse(const NostrEvent *event,
                                        nostr_communikeys_definition_t *out);
nostr_communikeys_status_t
nostr_communikeys_definition_validate_event(const NostrEvent *event);

/* Strict writer: returns NULL unless the model satisfies all definition rules. */
NostrEvent *
nostr_communikeys_definition_to_event(const nostr_communikeys_definition_t *definition,
                                      int64_t created_at);

const nostr_communikeys_section_t *
nostr_communikeys_definition_find_section(const nostr_communikeys_definition_t *definition,
                                          int kind,
                                          const char *subtype);

typedef struct {
    char *pubkey;
    char *relay;
} nostr_communikeys_target_t;

typedef enum {
    NOSTR_COMMUNIKEYS_REFERENCE_EVENT,
    NOSTR_COMMUNIKEYS_REFERENCE_ADDRESS
} nostr_communikeys_reference_type_t;

typedef struct {
    char *identifier; /* kind-30222 d value */
    nostr_communikeys_reference_type_t reference_type;
    char *reference;  /* event id or address coordinate */
    char *reference_relay;
    char *reference_author; /* optional e author hint; derived for a references */
    int original_kind;
    char *original_author; /* kind-30222 author */
    nostr_communikeys_target_t *targets;
    size_t targets_len;
} nostr_communikeys_targeted_publication_t;

void nostr_communikeys_targeted_publication_clear(
    nostr_communikeys_targeted_publication_t *publication);

nostr_communikeys_status_t
nostr_communikeys_targeted_publication_parse(
    const NostrEvent *event,
    nostr_communikeys_targeted_publication_t *out);

/* If original is non-NULL, also verifies author, kind, and e/a consistency. */
nostr_communikeys_status_t
nostr_communikeys_targeted_publication_validate(
    const NostrEvent *event,
    const NostrEvent *original);

NostrEvent *
nostr_communikeys_targeted_publication_to_event(
    const nostr_communikeys_targeted_publication_t *publication,
    int64_t created_at);

nostr_communikeys_status_t
nostr_communikeys_exclusive_validate(const NostrEvent *event, char community_pubkey[65]);
bool nostr_communikeys_exclusive_add_h(NostrEvent *event, const char *community_pubkey);

typedef struct {
    char pubkey[65];
    char **relays;
    size_t relays_len;
} nostr_communikeys_identifier_t;

void nostr_communikeys_identifier_clear(nostr_communikeys_identifier_t *identifier);
nostr_communikeys_status_t
nostr_communikeys_identifier_parse(const char *value,
                                   nostr_communikeys_identifier_t *out);
char *nostr_communikeys_identifier_format(
    const nostr_communikeys_identifier_t *identifier);

typedef struct {
    char *publisher;
    char *identifier;
    char **members;
    size_t members_len;
} nostr_communikeys_profile_list_t;

void nostr_communikeys_profile_list_clear(nostr_communikeys_profile_list_t *list);
nostr_communikeys_status_t
nostr_communikeys_profile_list_parse(const NostrEvent *event,
                                     const char *expected_publisher,
                                     const char *expected_identifier,
                                     nostr_communikeys_profile_list_t *out);
bool nostr_communikeys_profile_list_contains(
    const nostr_communikeys_profile_list_t *list,
    const char *pubkey);

/* Resolves one exact section and verifies its referenced kind-30000 list.
 * Duplicate/invalid definitions fail closed. */
bool nostr_communikeys_author_can_publish(
    const nostr_communikeys_definition_t *definition,
    int kind,
    const char *subtype,
    const NostrEvent *profile_list_event,
    const char *author_pubkey);

#ifdef __cplusplus
}
#endif

#endif
