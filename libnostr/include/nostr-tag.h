#ifndef __NOSTR_TAG_H__
#define __NOSTR_TAG_H__

#include "nostr-export.h"

/* Public header: standardized names for Tags/Tag APIs (no legacy aliases). */

#include <stddef.h>
#include <stdbool.h>
#include "string_array.h"

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
NOSTR_API NostrTag  *nostr_tag_new(const char *key, ...);
NOSTR_API void       nostr_tag_free(NostrTag *tag);

/* Tag queries */
NOSTR_API bool       nostr_tag_starts_with(NostrTag *tag, NostrTag *prefix);
NOSTR_API const char *nostr_tag_get_key(const NostrTag *tag);
NOSTR_API const char *nostr_tag_get_value(const NostrTag *tag);
NOSTR_API const char *nostr_tag_get_relay(const NostrTag *tag);
NOSTR_API char       *nostr_tag_to_json(const NostrTag *tag);
NOSTR_API size_t     nostr_tag_size(const NostrTag *tag);
NOSTR_API const char *nostr_tag_get(const NostrTag *tag, size_t index);
NOSTR_API void       nostr_tag_set(NostrTag *tag, size_t index, const char *value);
NOSTR_API void       nostr_tag_append(NostrTag *tag, const char *value);
NOSTR_API void       nostr_tag_add(NostrTag *tag, const char *value);
/* Capacity management */
NOSTR_API void       nostr_tag_reserve(NostrTag *tag, size_t capacity);

/* Tags (array of Tag) */
NOSTR_API NostrTags *nostr_tags_new(size_t count, ...);
NOSTR_API void       nostr_tags_free(NostrTags *tags);
NOSTR_API const char *nostr_tags_get_d(NostrTags *tags);
NOSTR_API NostrTag  *nostr_tags_get_first(NostrTags *tags, NostrTag *prefix);
NOSTR_API NostrTag  *nostr_tags_get_last(NostrTags *tags, NostrTag *prefix);
NOSTR_API NostrTags *nostr_tags_get_all(NostrTags *tags, NostrTag *prefix);
NOSTR_API NostrTags *nostr_tags_filter_out(NostrTags *tags, NostrTag *prefix);
NOSTR_API NostrTags *nostr_tags_append_unique(NostrTags *tags, NostrTag *tag);
NOSTR_API bool       nostr_tags_contains_any(NostrTags *tags, const char *tag_name, char **values, size_t values_count);
NOSTR_API char       *nostr_tags_to_json(NostrTags *tags);
NOSTR_API size_t     nostr_tags_size(const NostrTags *tags);
NOSTR_API NostrTag  *nostr_tags_get(const NostrTags *tags, size_t index);
NOSTR_API void       nostr_tags_set(NostrTags *tags, size_t index, NostrTag *tag);
NOSTR_API void       nostr_tags_append(NostrTags *tags, NostrTag *tag);
/* Capacity management */
NOSTR_API void       nostr_tags_reserve(NostrTags *tags, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_TAG_H__ */
