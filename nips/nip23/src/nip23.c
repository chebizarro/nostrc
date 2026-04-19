#include "nostr/nip23/nip23.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_WPM 200

int nostr_nip23_parse(const NostrEvent *ev, NostrNip23Article *out) {
    if (!ev || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        const char *v = nostr_tag_get(t, 1);
        if (!k || !v) continue;

        if (strcmp(k, "d") == 0 && !out->identifier) {
            out->identifier = v;
        } else if (strcmp(k, "title") == 0 && !out->title) {
            out->title = v;
        } else if (strcmp(k, "summary") == 0 && !out->summary) {
            out->summary = v;
        } else if (strcmp(k, "image") == 0 && !out->image) {
            out->image = v;
        } else if (strcmp(k, "client") == 0 && !out->client) {
            out->client = v;
        } else if (strcmp(k, "published_at") == 0 && out->published_at == 0) {
            char *endptr;
            long long ts = strtoll(v, &endptr, 10);
            if (endptr != v && *endptr == '\0' && ts > 0)
                out->published_at = (int64_t)ts;
        }
    }

    return out->identifier ? 0 : -ENOENT;
}

bool nostr_nip23_is_article(const NostrEvent *ev) {
    if (!ev) return false;
    return nostr_event_get_kind(ev) == NOSTR_NIP23_KIND_LONG_FORM;
}

bool nostr_nip23_is_draft(const NostrEvent *ev) {
    if (!ev) return false;
    return nostr_event_get_kind(ev) == NOSTR_NIP23_KIND_DRAFT;
}

bool nostr_nip23_is_long_form(const NostrEvent *ev) {
    if (!ev) return false;
    int kind = nostr_event_get_kind(ev);
    return kind == NOSTR_NIP23_KIND_LONG_FORM ||
           kind == NOSTR_NIP23_KIND_DRAFT;
}

int nostr_nip23_get_hashtags(const NostrEvent *ev, const char **tags,
                              size_t max, size_t *out_count) {
    if (!ev || !tags || !out_count) return -EINVAL;
    *out_count = 0;

    NostrTags *etags = (NostrTags *)nostr_event_get_tags(ev);
    if (!etags) return 0;

    size_t n = nostr_tags_size(etags);
    size_t count = 0;

    for (size_t i = 0; i < n && count < max; ++i) {
        NostrTag *t = nostr_tags_get(etags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "t") != 0) continue;
        const char *v = nostr_tag_get(t, 1);
        if (v && *v != '\0')
            tags[count++] = v;
    }

    *out_count = count;
    return 0;
}

size_t nostr_nip23_count_hashtags(const NostrEvent *ev) {
    if (!ev) return 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, "t") == 0) ++count;
    }
    return count;
}

int nostr_nip23_estimate_reading_time(const char *content,
                                       int words_per_minute) {
    if (!content || *content == '\0') return 0;

    if (words_per_minute <= 0)
        words_per_minute = DEFAULT_WPM;

    int word_count = 0;
    bool in_word = false;

    for (const char *p = content; *p; ++p) {
        if (isspace((unsigned char)*p)) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++word_count;
        }
    }

    int minutes = (word_count + words_per_minute - 1) / words_per_minute;
    return minutes > 0 ? minutes : 1;
}

/*
 * Internal helper: populate event with article tags.
 */
static int create_article_internal(NostrEvent *ev,
                                    const NostrNip23Article *article,
                                    int kind) {
    if (!ev || !article || !article->identifier) return -EINVAL;

    nostr_event_set_kind(ev, kind);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* Required: d tag */
    nostr_tags_append(tags, nostr_tag_new("d", article->identifier, NULL));

    /* Optional metadata tags */
    if (article->title)
        nostr_tags_append(tags, nostr_tag_new("title", article->title, NULL));

    if (article->summary)
        nostr_tags_append(tags, nostr_tag_new("summary", article->summary, NULL));

    if (article->image)
        nostr_tags_append(tags, nostr_tag_new("image", article->image, NULL));

    if (article->published_at > 0) {
        char ts_buf[24];
        snprintf(ts_buf, sizeof(ts_buf), "%lld", (long long)article->published_at);
        nostr_tags_append(tags, nostr_tag_new("published_at", ts_buf, NULL));
    }

    if (article->client)
        nostr_tags_append(tags, nostr_tag_new("client", article->client, NULL));

    return 0;
}

int nostr_nip23_create_article(NostrEvent *ev, const NostrNip23Article *article) {
    return create_article_internal(ev, article, NOSTR_NIP23_KIND_LONG_FORM);
}

int nostr_nip23_create_draft(NostrEvent *ev, const NostrNip23Article *article) {
    return create_article_internal(ev, article, NOSTR_NIP23_KIND_DRAFT);
}

int nostr_nip23_add_hashtag(NostrEvent *ev, const char *hashtag) {
    if (!ev || !hashtag || *hashtag == '\0') return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("t", hashtag, NULL));
    return 0;
}
