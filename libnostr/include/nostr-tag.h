#ifndef __NOSTR_TAG_H__
#define __NOSTR_TAG_H__

/* Public header: standardized names for Tags/Tag APIs (no legacy aliases). */

#include <stddef.h>
#include <stdbool.h>
#include "go.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical type names */
typedef StringArray NostrTag;

typedef struct _NostrTags {
    StringArray **data;
    size_t count;
    size_t capacity; /* internal capacity for data array */
} NostrTags;

/* Constructors and ownership */
NostrTag  *nostr_tag_new(const char *key, ...);
void       nostr_tag_free(NostrTag *tag);

/* Tag queries */
bool       nostr_tag_starts_with(NostrTag *tag, NostrTag *prefix);
const char *nostr_tag_get_key(const NostrTag *tag);
const char *nostr_tag_get_value(const NostrTag *tag);
const char *nostr_tag_get_relay(const NostrTag *tag);
char       *nostr_tag_to_json(const NostrTag *tag);
size_t     nostr_tag_size(const NostrTag *tag);
const char *nostr_tag_get(const NostrTag *tag, size_t index);
void       nostr_tag_set(NostrTag *tag, size_t index, const char *value);
void       nostr_tag_append(NostrTag *tag, const char *value);
void       nostr_tag_add(NostrTag *tag, const char *value);
/* Capacity management */
void       nostr_tag_reserve(NostrTag *tag, size_t capacity);

/* Tags (array of Tag) */
NostrTags *nostr_tags_new(size_t count, ...);
void       nostr_tags_free(NostrTags *tags);
const char *nostr_tags_get_d(NostrTags *tags);
NostrTag  *nostr_tags_get_first(NostrTags *tags, NostrTag *prefix);
NostrTag  *nostr_tags_get_last(NostrTags *tags, NostrTag *prefix);
NostrTags *nostr_tags_get_all(NostrTags *tags, NostrTag *prefix);
NostrTags *nostr_tags_filter_out(NostrTags *tags, NostrTag *prefix);
NostrTags *nostr_tags_append_unique(NostrTags *tags, NostrTag *tag);
bool       nostr_tags_contains_any(NostrTags *tags, const char *tag_name, char **values, size_t values_count);
char       *nostr_tags_to_json(NostrTags *tags);
size_t     nostr_tags_size(const NostrTags *tags);
NostrTag  *nostr_tags_get(const NostrTags *tags, size_t index);
void       nostr_tags_set(NostrTags *tags, size_t index, NostrTag *tag);
void       nostr_tags_append(NostrTags *tags, NostrTag *tag);
/* Capacity management */
void       nostr_tags_reserve(NostrTags *tags, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_TAG_H__ */
