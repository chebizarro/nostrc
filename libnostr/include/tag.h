#ifndef NOSTR_TAG_H
#define NOSTR_TAG_H

#include <stdlib.h>
#include <stdbool.h>

typedef struct _Tag {
    char **elements;
    size_t count;
} Tag;

typedef struct _Tags {
    Tag **data;
    size_t count;
} Tags;

Tag *create_tag(const char *key, ...);
void free_tag(Tag *tag);
bool tag_starts_with(Tag *tag, Tag *prefix);
char *tag_key(Tag *tag);
char *tag_value(Tag *tag);
char *tag_relay(Tag *tag);
char *tag_marshal_to_json(Tag *tag);

Tags *create_tags(size_t count, ...);
void free_tags(Tags *tags);
char *tags_get_d(Tags *tags);
Tag *tags_get_first(Tags *tags, Tag *prefix);
Tag *tags_get_last(Tags *tags, Tag *prefix);
Tags *tags_get_all(Tags *tags, Tag *prefix);
Tags *tags_filter_out(Tags *tags, Tag *prefix);
Tags *tags_append_unique(Tags *tags, Tag *tag);
bool tags_contains_any(Tags *tags, const char *tag_name, char **values, size_t values_count);
char *tags_marshal_to_json(Tags *tags);

#endif // NOSTR_TAG_H