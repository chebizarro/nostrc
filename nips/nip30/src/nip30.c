#include "nostr/nip30/nip30.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "nostr-kinds.h"
#include "codec-internal.h"

/* NIP-30 shortcodes are ASCII, independent of the process locale. */
static inline bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

int nostr_nip30_parse(const NostrEvent *ev, NostrNip30Emoji *entries,
                       size_t max_entries, size_t *out_count) {
    if (!ev || !entries || !out_count) return -EINVAL;
    *out_count = 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n && count < max_entries; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 3) continue;

        const char *k = codec_field(t, 0);
        if (!k || strcmp(k, "emoji") != 0) continue;

        const char *name = codec_field(t, 1);
        const char *url = codec_field(t, 2);
        if (!name || !url || *name == '\0' || *url == '\0') continue;

        entries[count].shortcode = name;
        entries[count].url = url;
        ++count;
    }

    *out_count = count;
    return 0;
}

size_t nostr_nip30_count(const NostrEvent *ev) {
    if (!ev) return 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 3) continue;
        const char *k = codec_field(t, 0);
        if (k && strcmp(k, "emoji") == 0) ++count;
    }
    return count;
}

const char *nostr_nip30_get_url(const NostrEvent *ev, const char *shortcode) {
    if (!ev || !shortcode) return NULL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return NULL;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 3) continue;

        const char *k = codec_field(t, 0);
        if (!k || strcmp(k, "emoji") != 0) continue;

        const char *name = codec_field(t, 1);
        if (name && strcmp(name, shortcode) == 0)
            return codec_field(t, 2);
    }
    return NULL;
}

int nostr_nip30_find_all(const char *content, NostrNip30Match *matches,
                          size_t max, size_t *out_count) {
    if (!content || !matches || !out_count) return -EINVAL;
    *out_count = 0;

    size_t count = 0;
    const char *p = content;

    while (*p && count < max) {
        if (*p != ':') { ++p; continue; }

        /* Found opening colon — scan word chars */
        const char *name_start = p + 1;
        const char *q = name_start;

        while (*q && is_word_char(*q)) ++q;

        /* Must have at least 1 char and end with colon */
        if (q > name_start && *q == ':') {
            matches[count].name = name_start;
            matches[count].name_len = (size_t)(q - name_start);
            matches[count].start = (size_t)(p - content);
            matches[count].end = (size_t)(q + 1 - content);
            ++count;
            p = q + 1;
        } else {
            ++p;
        }
    }

    *out_count = count;
    return 0;
}

char *nostr_nip30_replace_all(const char *content,
                               NostrNip30Replacer replacer,
                               void *user_data) {
    if (!content || !replacer) return NULL;

    /* First pass: find all matches */
    NostrNip30Match matches[64];
    size_t match_count = 0;
    nostr_nip30_find_all(content, matches, 64, &match_count);

    if (match_count == 0) {
        /* No matches: return copy of original */
        return strdup(content);
    }

    /* Calculate output size (worst case: each replacement might be longer) */
    size_t content_len = strlen(content);
    /* Start with content_len, adjust for each replacement */
    size_t out_cap = content_len + 256;
    char *out = malloc(out_cap);
    if (!out) return NULL;

    size_t out_len = 0;
    size_t prev_end = 0;

    for (size_t i = 0; i < match_count; ++i) {
        /* Copy text before this match */
        size_t gap = matches[i].start - prev_end;
        if (out_len + gap >= out_cap) {
            out_cap = (out_len + gap + 256) * 2;
            char *tmp = realloc(out, out_cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }
        memcpy(out + out_len, content + prev_end, gap);
        out_len += gap;

        /* Get replacement */
        const char *repl = replacer(matches[i].name, matches[i].name_len,
                                     user_data);
        if (repl) {
            size_t rlen = strlen(repl);
            if (out_len + rlen >= out_cap) {
                out_cap = (out_len + rlen + 256) * 2;
                char *tmp = realloc(out, out_cap);
                if (!tmp) { free(out); return NULL; }
                out = tmp;
            }
            memcpy(out + out_len, repl, rlen);
            out_len += rlen;
        } else {
            /* Keep original :shortcode: */
            size_t orig_len = matches[i].end - matches[i].start;
            if (out_len + orig_len >= out_cap) {
                out_cap = (out_len + orig_len + 256) * 2;
                char *tmp = realloc(out, out_cap);
                if (!tmp) { free(out); return NULL; }
                out = tmp;
            }
            memcpy(out + out_len, content + matches[i].start, orig_len);
            out_len += orig_len;
        }

        prev_end = matches[i].end;
    }

    /* Copy remaining text */
    size_t remaining = content_len - prev_end;
    if (out_len + remaining + 1 >= out_cap) {
        out_cap = out_len + remaining + 1;
        char *tmp = realloc(out, out_cap);
        if (!tmp) { free(out); return NULL; }
        out = tmp;
    }
    memcpy(out + out_len, content + prev_end, remaining);
    out_len += remaining;
    out[out_len] = '\0';

    return out;
}

bool nostr_nip30_valid_shortcode(const char *s) {
    if (!s || !*s) return false;
    for (; *s; s++) if (!is_word_char(*s)) return false;
    return true;
}
bool nostr_nip30_valid_image_url(const char *url) { return codec_url(url, false); }
static bool valid_set(const char *s) {
    int kind;
    return !s || (codec_address(s, &kind, NULL, NULL) && kind == NOSTR_KIND_EMOJI_SETS);
}
int nostr_nip30_parse_full(const NostrEvent *ev, NostrNip30EmojiTag *entries, size_t max, size_t *count) {
    if (!ev || !count || (max && !entries)) return -EINVAL;
    *count = 0;
    NostrTags *tags = nostr_event_get_tags(ev);
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        const char *key = codec_field(t, 0), *name = codec_field(t, 1),
                   *url = codec_field(t, 2), *set = codec_field(t, 3);
        if (!key || strcmp(key, "emoji") || !nostr_nip30_valid_shortcode(name) ||
            !nostr_nip30_valid_image_url(url) || !valid_set(set)) continue;
        (*count)++;
    }
    if (*count > max) return -ENOSPC;
    size_t n = 0;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        const char *key = codec_field(t, 0), *name = codec_field(t, 1),
                   *url = codec_field(t, 2), *set = codec_field(t, 3);
        if (!key || strcmp(key, "emoji") || !nostr_nip30_valid_shortcode(name) ||
            !nostr_nip30_valid_image_url(url) || !valid_set(set)) continue;
        entries[n++] = (NostrNip30EmojiTag){name, url, set};
    }
    return 0;
}
int nostr_nip30_add_emoji_full(NostrEvent *ev, const char *name, const char *url, const char *set) {
    if (!ev || !nostr_nip30_valid_shortcode(name) || !nostr_nip30_valid_image_url(url) || !valid_set(set)) return -EINVAL;
    NostrTags *tags = nostr_event_get_tags(ev);
    bool fresh = !tags;
    if (fresh) tags = nostr_tags_new(0);
    if (!tags) return -ENOMEM;
    const char *fields[] = {"emoji", name, url, set};
    int rc = codec_tag_append(tags, fields, set ? 4 : 3);
    if (fresh) {
        if (rc) nostr_tags_free(tags);
        else nostr_event_set_tags(ev, tags);
    }
    return rc;
}

int nostr_nip30_add_emoji(NostrEvent *ev, const char *shortcode,
                           const char *url) {
    return nostr_nip30_add_emoji_full(ev, shortcode, url, NULL);
}
