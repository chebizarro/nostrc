#include "nostr.h"
#include <stdlib.h>
#include <string.h>
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "security_limits_runtime.h"
#include <stdbool.h>
#include <stdint.h>

#define INITIAL_CAPACITY 4  // Initial capacity for Filters array

NostrFilter *nostr_filter_new(void) {
    NostrFilter *filter = (NostrFilter *)malloc(sizeof(NostrFilter));
    if (!filter)
        return NULL;

    string_array_init(&filter->ids);
    int_array_init(&filter->kinds);
    string_array_init(&filter->authors);
    filter->tags = nostr_tags_new(0);
    filter->since = 0;
    filter->until = 0;
    filter->limit = 0;
    filter->search = NULL;
    filter->limit_zero = false;

    return filter;
}

/* --- Legacy symbol compatibility (remove after full migration) --- */
NostrFilter *create_filter(void) {
    return nostr_filter_new();
}

bool filter_matches(NostrFilter *filter, NostrEvent *event) {
    return nostr_filter_matches(filter, event);
}

bool filter_match_ignoring_timestamp(NostrFilter *filter, NostrEvent *event) {
    return nostr_filter_match_ignoring_timestamp(filter, event);
}

bool filters_match(NostrFilters *filters, NostrEvent *event) {
    return nostr_filters_match(filters, event);
}

bool filters_match_ignoring_timestamp(NostrFilters *filters, NostrEvent *event) {
    return nostr_filters_match_ignoring_timestamp(filters, event);
}

void free_filters(NostrFilters *filters) {
    nostr_filters_free(filters);
}

NostrFilters *create_filters(void) {
    return nostr_filters_new();
}

bool filters_add(NostrFilters *filters, NostrFilter *filter) {
    return nostr_filters_add(filters, filter);
}

static void free_filter_contents(NostrFilter *filter) {
    string_array_free(&filter->ids);
    int_array_free(&filter->kinds);
    string_array_free(&filter->authors);
    if (filter->tags) {
        nostr_tags_free(filter->tags);
        filter->tags = NULL;
    }
    free(filter->search);
    filter->search = NULL;
}

void free_filter(NostrFilter *filter) {
    if (!filter) return;
    free_filter_contents(filter);
    free(filter);
}

/* New API free wrapper (needed for GBoxed) */
void nostr_filter_free(NostrFilter *filter) {
    free_filter(filter);
}

/* Clear contents of a stack- or heap-allocated filter without freeing struct */
void nostr_filter_clear(NostrFilter *filter) {
    if (!filter) return;
    /* Free any heap members and null internal pointers */
    free_filter_contents(filter);
    /* Reinitialize arrays to a usable empty state */
    string_array_init(&filter->ids);
    int_array_init(&filter->kinds);
    string_array_init(&filter->authors);
    /* Tags can be lazily allocated on demand */
    filter->tags = NULL;
    /* Reset scalars */
    filter->since = 0;
    filter->until = 0;
    filter->limit = 0;
    filter->limit_zero = false;
}

/* Deep-copy helpers for tags */
static NostrTag *filter_tag_clone(const NostrTag *src) {
    if (!src) return NULL;
    size_t n = string_array_size((StringArray *)src);
    StringArray *dst = new_string_array((int)n);
    for (size_t i = 0; i < n; i++) {
        const char *s = string_array_get((const StringArray *)src, i);
        if (s) string_array_add(dst, s);
    }
    return (NostrTag *)dst;
}

static NostrTags *filter_tags_clone(const NostrTags *src) {
    if (!src) return NULL;
    NostrTags *dst = (NostrTags *)malloc(sizeof(NostrTags));
    if (!dst) return NULL;
    dst->count = src->count;
    dst->data = (NostrTag **)calloc(dst->count, sizeof(NostrTag *));
    if (!dst->data) { nostr_tags_free(dst); return NULL; }
    for (size_t i = 0; i < dst->count; i++) {
        dst->data[i] = (NostrTag *)filter_tag_clone(src->data[i]);
    }
    return dst;
}

NostrFilter *nostr_filter_copy(const NostrFilter *src) {
    if (!src) return NULL;
    NostrFilter *f = nostr_filter_new();
    if (!f) return NULL;
    /* ids */
    for (size_t i = 0, n = string_array_size((StringArray *)&src->ids); i < n; i++) {
        const char *s = string_array_get(&src->ids, i);
        if (s) string_array_add(&f->ids, s);
    }
    /* kinds */
    for (size_t i = 0, n = int_array_size((IntArray *)&src->kinds); i < n; i++) {
        int_array_add(&f->kinds, int_array_get(&src->kinds, i));
    }
    /* authors */
    for (size_t i = 0, n = string_array_size((StringArray *)&src->authors); i < n; i++) {
        const char *s = string_array_get(&src->authors, i);
        if (s) string_array_add(&f->authors, s);
    }
    /* tags */
    if (f->tags) { nostr_tags_free(f->tags); f->tags = NULL; }
    f->tags = filter_tags_clone(src->tags);
    /* scalars */
    f->since = src->since;
    f->until = src->until;
    f->limit = src->limit;
    f->limit_zero = src->limit_zero;
    /* search */
    if (src->search) f->search = strdup(src->search);
    return f;
}

NostrFilters *nostr_filters_new(void) {
    NostrFilters *filters = (NostrFilters *)malloc(sizeof(NostrFilters));
    if (!filters)
        return NULL;

    filters->count = 0;
    filters->capacity = INITIAL_CAPACITY;
    filters->filters = (NostrFilter *)malloc(filters->capacity * sizeof(NostrFilter));
    if (!filters->filters) {
        free(filters);
        return NULL;
    }

    return filters;
}

// Resizes the internal array when needed
static bool filters_resize(NostrFilters *filters) {
    size_t new_capacity = filters->capacity * 2;
    NostrFilter *new_filters = (NostrFilter *)realloc(filters->filters, new_capacity * sizeof(NostrFilter));
    if (!new_filters)
        return false;

    filters->filters = new_filters;
    filters->capacity = new_capacity;
    return true;
}

bool nostr_filters_add(NostrFilters *filters, NostrFilter *filter) {
    if (!filters || !filter)
        return false;

    // Resize the array if necessary
    if (filters->count == filters->capacity) {
        if (!filters_resize(filters)) {
            return false;
        }
    }

    /* Move semantics: transfer ownership of internals into the array slot */
    filters->filters[filters->count] = *filter; // shallow copy fields
    /* Invalidate source to prevent double-free by callers */
    memset(filter, 0, sizeof(*filter));
    filters->count++;
    return true;
}


void nostr_filters_free(NostrFilters *filters) {
    for (size_t i = 0; i < filters->count; i++) {
        free_filter_contents(&filters->filters[i]);
    }
    free(filters->filters);
    free(filters);
}

bool nostr_filter_matches(NostrFilter *filter, NostrEvent *event) {
    if (!filter || !event)
        return false;

    bool match = true;
    if (!filter_match_ignoring_timestamp(filter, event))
        return false;
    if (filter->since && event->created_at < (filter->since))
        return false;
    if (filter->until && event->created_at > (filter->until))
        return false;
    return match;
}

bool nostr_filter_match_ignoring_timestamp(NostrFilter *filter, NostrEvent *event) {
    if (!filter || !event)
        return false;

    bool match = true;

    if (string_array_size(&filter->ids) > 0) {
        if (!string_array_contains(&filter->ids, event->id))
            return false;
    }

    if (int_array_size(&filter->kinds) > 0) {
        if (!int_array_contains(&filter->kinds, event->kind))
            return false;
    }

    if (string_array_size(&filter->authors) > 0) {
        if (!string_array_contains(&filter->authors, event->pubkey))
            return false;
    }

    if (filter->tags && filter->tags->count > 0) {
        // TODO implement
    }

    return match;
}

bool nostr_filters_match(NostrFilters *filters, NostrEvent *event) {
    if (!filters || !event)
        return false;

    for (size_t i = 0; i < filters->count; i++) {
        if (filter_matches(&filters->filters[i], event)) {
            return true;
        }
    }

    return false;
}

bool nostr_filters_match_ignoring_timestamp(NostrFilters *filters, NostrEvent *event) {
    if (!filters || !event)
        return false;

    for (size_t i = 0; i < filters->count; i++) {
        if (filter_match_ignoring_timestamp(&filters->filters[i], event)) {
            return true;
        }
    }

    return false;
}

/* Getters/Setters for Filter fields (public API via nostr-filter.h) */

const StringArray *nostr_filter_get_ids(const NostrFilter *filter) {
    return filter ? &filter->ids : NULL;
}

void nostr_filter_set_ids(NostrFilter *filter, const char *const *ids, size_t count) {
    if (!filter) return;
    string_array_free(&filter->ids);
    string_array_init(&filter->ids);
    if (!ids) return;
    size_t max = count;
    if (max > (size_t)nostr_limit_max_ids_per_filter()) max = (size_t)nostr_limit_max_ids_per_filter();
    for (size_t i = 0; i < max; i++) {
        if (ids[i]) string_array_add(&filter->ids, ids[i]);
    }
}

const IntArray *nostr_filter_get_kinds(const NostrFilter *filter) {
    return filter ? &filter->kinds : NULL;
}

void nostr_filter_set_kinds(NostrFilter *filter, const int *kinds, size_t count) {
    if (!filter) return;
    int_array_free(&filter->kinds);
    int_array_init(&filter->kinds);
    if (!kinds) return;
    for (size_t i = 0; i < count; i++) {
        int_array_add(&filter->kinds, kinds[i]);
    }
}

const StringArray *nostr_filter_get_authors(const NostrFilter *filter) {
    return filter ? &filter->authors : NULL;
}

void nostr_filter_set_authors(NostrFilter *filter, const char *const *authors, size_t count) {
    if (!filter) return;
    string_array_free(&filter->authors);
    string_array_init(&filter->authors);
    if (!authors) return;
    for (size_t i = 0; i < count; i++) {
        if (authors[i]) string_array_add(&filter->authors, authors[i]);
    }
}

NostrTags *nostr_filter_get_tags(const NostrFilter *filter) {
    return filter ? filter->tags : NULL;
}

void nostr_filter_set_tags(NostrFilter *filter, NostrTags *tags) {
    if (!filter) return;
    if (filter->tags && filter->tags != tags) {
        nostr_tags_free(filter->tags);
    }
    filter->tags = tags; /* takes ownership */
}

int64_t nostr_filter_get_since_i64(const NostrFilter *filter) {
    return filter ? filter->since : 0;
}

void nostr_filter_set_since_i64(NostrFilter *filter, int64_t since) {
    if (!filter) return;
    filter->since = since;
}

int64_t nostr_filter_get_until_i64(const NostrFilter *filter) {
    return filter ? filter->until : 0;
}

void nostr_filter_set_until_i64(NostrFilter *filter, int64_t until) {
    if (!filter) return;
    filter->until = until;
}

int nostr_filter_get_limit(const NostrFilter *filter) {
    return filter ? filter->limit : 0;
}

void nostr_filter_set_limit(NostrFilter *filter, int limit) {
    if (!filter) return;
    filter->limit = limit;
}

const char *nostr_filter_get_search(const NostrFilter *filter) {
    return filter ? filter->search : NULL;
}

void nostr_filter_set_search(NostrFilter *filter, const char *search) {
    if (!filter) return;
    if (filter->search) { free(filter->search); filter->search = NULL; }
    if (search) filter->search = strdup(search);
}

/* limit_zero accessors are defined once below */

bool nostr_filter_get_limit_zero(const NostrFilter *filter) {
    return filter ? filter->limit_zero : false;
}

void nostr_filter_set_limit_zero(NostrFilter *filter, bool limit_zero) {
    if (!filter) return;
    filter->limit_zero = limit_zero;
}

/* === GI-friendly accessors (now declared in nostr-filter.h) === */

size_t nostr_filter_ids_len(const NostrFilter *filter) {
    return filter ? string_array_size((StringArray *)&filter->ids) : 0;
}

const char *nostr_filter_ids_get(const NostrFilter *filter, size_t index) {
    if (!filter) return NULL;
    size_t n = string_array_size((StringArray *)&filter->ids);
    if (index >= n) return NULL;
    return string_array_get(&filter->ids, index);
}

size_t nostr_filter_kinds_len(const NostrFilter *filter) {
    return filter ? int_array_size((IntArray *)&filter->kinds) : 0;
}

int nostr_filter_kinds_get(const NostrFilter *filter, size_t index) {
    if (!filter) return 0;
    size_t n = int_array_size((IntArray *)&filter->kinds);
    if (index >= n) return 0;
    return int_array_get(&filter->kinds, index);
}

size_t nostr_filter_authors_len(const NostrFilter *filter) {
    return filter ? string_array_size((StringArray *)&filter->authors) : 0;
}

const char *nostr_filter_authors_get(const NostrFilter *filter, size_t index) {
    if (!filter) return NULL;
    size_t n = string_array_size((StringArray *)&filter->authors);
    if (index >= n) return NULL;
    return string_array_get(&filter->authors, index);
}

size_t nostr_filter_tags_len(const NostrFilter *filter) {
    return (filter && filter->tags) ? nostr_tags_size(filter->tags) : 0;
}

size_t nostr_filter_tag_len(const NostrFilter *filter, size_t tag_index) {
    if (!filter || !filter->tags) return 0;
    if (tag_index >= nostr_tags_size(filter->tags)) return 0;
    NostrTag *t = nostr_tags_get(filter->tags, tag_index);
    return t ? nostr_tag_size(t) : 0;
}

const char *nostr_filter_tag_get(const NostrFilter *filter, size_t tag_index, size_t item_index) {
    if (!filter || !filter->tags) return NULL;
    if (tag_index >= nostr_tags_size(filter->tags)) return NULL;
    NostrTag *t = nostr_tags_get(filter->tags, tag_index);
    if (!t) return NULL;
    size_t n = nostr_tag_size(t);
    if (item_index >= n) return NULL;
    return nostr_tag_get(t, item_index);
}

void nostr_filter_add_id(NostrFilter *filter, const char *id) {
    if (!filter || !id) return;
    size_t n = string_array_size((StringArray *)&filter->ids);
    if (n >= (size_t)nostr_limit_max_ids_per_filter()) return;
    string_array_add(&filter->ids, id);
}

void nostr_filter_add_kind(NostrFilter *filter, int kind) {
    if (!filter) return;
    int_array_add(&filter->kinds, kind);
}

void nostr_filter_add_author(NostrFilter *filter, const char *author) {
    if (!filter || !author) return;
    string_array_add(&filter->authors, author);
}

void nostr_filter_tags_append(NostrFilter *filter, const char *key, const char *value, const char *relay) {
    if (!filter || !key) return;
    /* Enforce tags-per-event cap */
    if (filter->tags && nostr_tags_size(filter->tags) >= (size_t)nostr_limit_max_tags_per_event()) return;
    NostrTag *t = NULL;
    if (relay && *relay) {
        t = nostr_tag_new(key, value ? value : "", relay, NULL);
    } else {
        t = nostr_tag_new(key, value ? value : "", NULL);
    }
    if (!t) return;
    if (!filter->tags) filter->tags = nostr_tags_new(0);
    if (nostr_tags_size(filter->tags) < (size_t)nostr_limit_max_tags_per_event()) {
        filter->tags = nostr_tags_append_unique(filter->tags, t);
    } else {
        nostr_tag_free(t);
    }
}

/* === Compact JSON fast-path for NostrFilter === */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} sb_t;

static void sb_init(sb_t *sb) {
    sb->cap = 256; sb->len = 0; sb->buf = (char *)malloc(sb->cap);
    if (sb->buf) sb->buf[0] = '\0';
}
static bool sb_reserve(sb_t *sb, size_t add) {
    if (sb->len + add + 1 <= sb->cap) return true;
    size_t ncap = sb->cap;
    while (sb->len + add + 1 > ncap) ncap = ncap ? ncap * 2 : 256;
    char *nbuf = (char *)realloc(sb->buf, ncap);
    if (!nbuf) return false;
    sb->buf = nbuf; sb->cap = ncap; return true;
}
static bool sb_putc(sb_t *sb, char c) {
    if (!sb_reserve(sb, 1)) return false;
    sb->buf[sb->len++] = c; sb->buf[sb->len] = '\0'; return true;
}
static bool sb_puts_raw(sb_t *sb, const char *s, size_t n) {
    if (!sb_reserve(sb, n)) return false;
    memcpy(sb->buf + sb->len, s, n); sb->len += n; sb->buf[sb->len] = '\0'; return true;
}
static bool sb_puts(sb_t *sb, const char *s) { return sb_puts_raw(sb, s, s ? strlen(s) : 0); }
static bool sb_put_quoted(sb_t *sb, const char *s) {
    if (!sb_putc(sb, '"')) return false;
    /* Minimal escaping: escape backslash and quote */
    for (const char *p = s ? s : ""; *p; ++p) {
        if (*p == '"' || *p == '\\') { if (!sb_putc(sb, '\\')) return false; }
        if (!sb_putc(sb, *p)) return false;
    }
    return sb_putc(sb, '"');
}

/* Emit dynamic tag hash-keys: {"#e":[".."],"#p":[".."],...} */
static bool emit_tag_hash_keys(sb_t *sb, const NostrTags *tags, bool *need_comma) {
    if (!tags || nostr_tags_size((NostrTags *)tags) == 0) return true;
    /* collect unique keys (first element) */
    size_t tcount = nostr_tags_size((NostrTags *)tags);
    const char **keys = (const char **)calloc(tcount, sizeof(char *));
    size_t kcnt = 0;
    if (!keys) return false;
    for (size_t i = 0; i < tcount; i++) {
        NostrTag *t = nostr_tags_get((NostrTags *)tags, i);
        const char *k = t ? nostr_tag_get(t, 0) : NULL;
        if (!k) continue;
        bool seen = false;
        for (size_t j = 0; j < kcnt; j++) { if (strcmp(keys[j], k) == 0) { seen = true; break; } }
        if (!seen) keys[kcnt++] = k;
    }
    bool ok = true;
    for (size_t j = 0; j < kcnt && ok; j++) {
        const char *k = keys[j];
        /* open key */
        if (*need_comma) ok &= sb_putc(sb, ',');
        ok &= sb_putc(sb, '"') && sb_putc(sb, '#') && sb_puts(sb, k) && sb_putc(sb, '"') && sb_putc(sb, ':');
        ok &= sb_putc(sb, '[');
        bool first = true;
        for (size_t i = 0; i < tcount && ok; i++) {
            NostrTag *t = nostr_tags_get((NostrTags *)tags, i);
            const char *k2 = t ? nostr_tag_get(t, 0) : NULL;
            if (!k2 || strcmp(k2, k) != 0) continue;
            const char *v = nostr_tag_get(t, 1);
            if (!v) continue;
            if (!first) ok &= sb_putc(sb, ',');
            ok &= sb_put_quoted(sb, v);
            first = false;
        }
        ok &= sb_putc(sb, ']');
        *need_comma = true;
    }
    free(keys);
    return ok;
}

char *nostr_filter_serialize_compact(const NostrFilter *f) {
    if (!f) return NULL;
    sb_t sb; sb_init(&sb);
    if (!sb.buf) return NULL;
    bool ok = sb_putc(&sb, '{');
    bool need_comma = false;
    /* ids */
    size_t n = string_array_size((StringArray *)&f->ids);
    if (ok && n > 0) {
        if (need_comma) ok &= sb_putc(&sb, ',');
        ok &= sb_puts(&sb, "\"ids\":[");
        for (size_t i = 0; i < n && ok; i++) {
            if (i) ok &= sb_putc(&sb, ',');
            ok &= sb_put_quoted(&sb, string_array_get((StringArray *)&f->ids, i));
        }
        ok &= sb_putc(&sb, ']');
        need_comma = true;
    }
    /* kinds */
    n = int_array_size((IntArray *)&f->kinds);
    if (ok && n > 0) {
        if (need_comma) ok &= sb_putc(&sb, ',');
        ok &= sb_puts(&sb, "\"kinds\":[");
        for (size_t i = 0; i < n && ok; i++) {
            char tmp[32];
            int v = int_array_get((IntArray *)&f->kinds, i);
            int m = snprintf(tmp, sizeof(tmp), "%d", v);
            if (i) ok &= sb_putc(&sb, ',');
            ok &= sb_puts_raw(&sb, tmp, (size_t)m);
        }
        ok &= sb_putc(&sb, ']');
        need_comma = true;
    }
    /* authors */
    n = string_array_size((StringArray *)&f->authors);
    if (ok && n > 0) {
        if (need_comma) ok &= sb_putc(&sb, ',');
        ok &= sb_puts(&sb, "\"authors\":[");
        for (size_t i = 0; i < n && ok; i++) {
            if (i) ok &= sb_putc(&sb, ',');
            ok &= sb_put_quoted(&sb, string_array_get((StringArray *)&f->authors, i));
        }
        ok &= sb_putc(&sb, ']');
        need_comma = true;
    }
    /* since/until */
    if (ok && f->since) {
        if (need_comma) ok &= sb_putc(&sb, ',');
        char tmp[64]; int m = snprintf(tmp, sizeof(tmp), "\"since\":%lld", (long long)f->since);
        ok &= sb_puts_raw(&sb, tmp, (size_t)m);
        need_comma = true;
    }
    if (ok && f->until) {
        if (need_comma) ok &= sb_putc(&sb, ',');
        char tmp[64]; int m = snprintf(tmp, sizeof(tmp), "\"until\":%lld", (long long)f->until);
        ok &= sb_puts_raw(&sb, tmp, (size_t)m);
        need_comma = true;
    }
    /* limit: include if >0 or explicitly set to zero flag */
    if (ok && (f->limit > 0 || f->limit_zero)) {
        if (need_comma) ok &= sb_putc(&sb, ',');
        char tmp[32]; int m = snprintf(tmp, sizeof(tmp), "\"limit\":%d", f->limit);
        ok &= sb_puts_raw(&sb, tmp, (size_t)m);
        need_comma = true;
    }
    /* search */
    if (ok && f->search && *f->search) {
        if (need_comma) ok &= sb_putc(&sb, ',');
        ok &= sb_puts(&sb, "\"search\":");
        ok &= sb_put_quoted(&sb, f->search);
        need_comma = true;
    }
    /* tags as dynamic hash keys */
    if (ok) ok &= emit_tag_hash_keys(&sb, f->tags, &need_comma);
    ok &= sb_putc(&sb, '}');
    if (!ok) { free(sb.buf); return NULL; }
    return sb.buf;
}

/* --- Compact deserializer helpers --- */
static const char *skip_ws_f(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
    return p;
}
static const char *skip_literal(const char *p) {
    // skip true/false/null or number
    if (*p == '-' || (*p >= '0' && *p <= '9')) { while ((*p >= '0' && *p <= '9') || *p=='.' || *p=='e' || *p=='E' || *p=='+' || *p=='-') ++p; return p; }
    if (strncmp(p, "true", 4) == 0) return p+4;
    if (strncmp(p, "false", 5) == 0) return p+5;
    if (strncmp(p, "null", 4) == 0) return p+4;
    return p;
}
static const char *skip_string(const char *p) {
    if (*p != '"') return NULL;
    ++p;
    while (*p) {
        if (*p == '\\') { if (*(p+1)) p += 2; else return NULL; }
        else if (*p == '"') { return p+1; }
        else { ++p; }
    }
    return NULL;
}
static const char *skip_array(const char *p);
static const char *skip_object(const char *p);
static const char *skip_value(const char *p) {
    p = skip_ws_f(p);
    if (*p == '{') return skip_object(p);
    if (*p == '[') return skip_array(p);
    if (*p == '"') return skip_string(p);
    return skip_literal(p);
}
static const char *skip_array(const char *p) {
    if (*p != '[') return NULL;
    ++p;
    p = skip_ws_f(p);
    if (*p == ']') return p+1;
    while (*p) {
        const char *q = skip_value(p);
        if (!q) return NULL; p = skip_ws_f(q);
        if (*p == ',') { ++p; p = skip_ws_f(p); continue; }
        if (*p == ']') return p+1;
        return NULL;
    }
    return NULL;
}
static const char *skip_object(const char *p) {
    if (*p != '{') return NULL; ++p; p = skip_ws_f(p);
    if (*p == '}') return p+1;
    while (*p) {
        const char *q = skip_string(p);
        if (!q) return NULL; p = skip_ws_f(q);
        if (*p != ':') return NULL; ++p; p = skip_ws_f(p);
        q = skip_value(p); if (!q) return NULL; p = skip_ws_f(q);
        if (*p == ',') { ++p; p = skip_ws_f(p); continue; }
        if (*p == '}') return p+1;
        return NULL;
    }
    return NULL;
}

static char *parse_string_dup(const char **pp) {
    const char *p = skip_ws_f(*pp);
    if (*p != '"') return NULL; ++p; const char *start = p;
    sb_t sb; sb_init(&sb); if (!sb.buf) return NULL;
    while (*p) {
        if (*p == '\\') {
            if (!*(p+1)) { free(sb.buf); return NULL; }
            // preserve escaped char without interpretation
            if (!sb_putc(&sb, *(p+1))) { free(sb.buf); return NULL; }
            p += 2; continue;
        } else if (*p == '"') {
            char *out = sb.buf; out[sb.len] = '\0'; *pp = p+1; return out;
        } else {
            if (!sb_putc(&sb, *p)) { free(sb.buf); return NULL; }
            ++p;
        }
    }
    free(sb.buf); return NULL;
}

static int parse_string_array_values_as_tags(NostrFilter *filter, const char *tag_key, const char **pp) {
    const char *p = skip_ws_f(*pp);
    if (*p != '[') return 0; ++p; p = skip_ws_f(p);
    if (*p == ']') { *pp = p+1; return 1; }
    while (*p) {
        // value must be string
        if (*p != '"') return 0;
        const char *ps = p; char *val = parse_string_dup(&ps);
        if (!val) return 0;
        // append tag [key, val]
        if (filter->tags && nostr_tags_size(filter->tags) >= (size_t)nostr_limit_max_tags_per_event()) { free(val); return 0; }
        NostrTag *t = nostr_tag_new(tag_key, val, NULL);
        free(val);
        if (t) {
            if (!filter->tags) filter->tags = nostr_tags_new(0);
            if (nostr_tags_size(filter->tags) < (size_t)nostr_limit_max_tags_per_event()) {
                NostrTags *tmp = nostr_tags_append_unique(filter->tags, t);
                if (tmp) filter->tags = tmp;
            } else {
                nostr_tag_free(t);
                return 0;
            }
        }
        p = skip_ws_f(ps);
        if (*p == ',') { ++p; p = skip_ws_f(p); continue; }
        if (*p == ']') { *pp = p+1; return 1; }
        return 0;
    }
    return 0;
}

int nostr_filter_deserialize_compact(NostrFilter *filter, const char *json) {
    if (!filter || !json) return 0;
    const char *p = skip_ws_f(json);
    if (*p != '{') return 0; ++p; p = skip_ws_f(p);
    int touched = 0;
    if (*p == '}') return 1; // empty object ok
    while (*p) {
        // parse key
        char *key = parse_string_dup(&p);
        if (!key) return 0;
        p = skip_ws_f(p);
        if (*p != ':') { free(key); return 0; }
        ++p; p = skip_ws_f(p);
        if (key[0] == '#' && key[1] != '\0' && key[2] == '\0') {
            // dynamic tag key of form "#e"
            if (*p != '[') { free(key); return 0; }
            char kbuf[2] = { key[1], '\0' };
            int ok = parse_string_array_values_as_tags(filter, kbuf, &p);
            free(key);
            if (!ok) return 0;
            touched = 1;
        } else {
            // skip other values quickly
            const char *q = skip_value(p);
            free(key);
            if (!q) return 0;
            p = q;
        }
        p = skip_ws_f(p);
        if (*p == ',') { ++p; p = skip_ws_f(p); continue; }
        if (*p == '}') return touched ? 1 : 0;
        return 0;
    }
    return 0;
}
