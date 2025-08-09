#ifndef __NOSTR_TAG_H__
#define __NOSTR_TAG_H__

/* Public header: standardized names for Tags/Tag APIs (no legacy aliases). */

#include <stddef.h>
#include <stdbool.h>
#include "tag.h" /* provides Tag/Tags concrete structs */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical type names */
typedef Tag  NostrTag;
typedef Tags NostrTags;

/* Constructors and ownership */
NostrTag  *nostr_tag_new(const char *key, ...);
void       nostr_tag_free(NostrTag *tag);

/* Tag queries */
bool       nostr_tag_starts_with(NostrTag *tag, NostrTag *prefix);
char      *nostr_tag_get_key(NostrTag *tag);
char      *nostr_tag_get_value(NostrTag *tag);
char      *nostr_tag_get_relay(NostrTag *tag);
char      *nostr_tag_to_json(NostrTag *tag);

/* Tags (array of Tag) */
NostrTags *nostr_tags_new(size_t count, ...);
void       nostr_tags_free(NostrTags *tags);
char      *nostr_tags_get_d(NostrTags *tags);
NostrTag  *nostr_tags_get_first(NostrTags *tags, NostrTag *prefix);
NostrTag  *nostr_tags_get_last(NostrTags *tags, NostrTag *prefix);
NostrTags *nostr_tags_get_all(NostrTags *tags, NostrTag *prefix);
NostrTags *nostr_tags_filter_out(NostrTags *tags, NostrTag *prefix);
NostrTags *nostr_tags_append_unique(NostrTags *tags, NostrTag *tag);
bool       nostr_tags_contains_any(NostrTags *tags, const char *tag_name, char **values, size_t values_count);
char      *nostr_tags_to_json(NostrTags *tags);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_TAG_H__ */
