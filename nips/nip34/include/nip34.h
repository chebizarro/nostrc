#ifndef NOSTR_NIP34_H
#define NOSTR_NIP34_H

#include "nostr-event.h"
#include "tag.h"

// Define the Patch struct
typedef struct {
    nostr_event_t event;
    nostr_entity_pointer_t repository;
    nostr_tag_t *tags;
    size_t tags_len;
} nostr_patch_t;

// Define the Repository struct
typedef struct {
    nostr_event_t event;
    char *id;
    char *name;
    char *description;
    char **web;
    size_t web_len;
    char **clone;
    size_t clone_len;
    char **relays;
    size_t relays_len;
    char *earliest_unique_commit_id;
    char **maintainers;
    size_t maintainers_len;
} nostr_repository_t;

// Function to parse a Patch from an event
nostr_patch_t* nostr_parse_patch(const nostr_event_t *event);

// Function to parse a Repository from an event
nostr_repository_t* nostr_parse_repository(const nostr_event_t *event);

// Function to free a Patch
void nostr_free_patch(nostr_patch_t *patch);

// Function to free a Repository
void nostr_free_repository(nostr_repository_t *repository);

#endif // NOSTR_NIP34_H
