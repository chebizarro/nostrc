#include "nostr/nipb0/nipb0.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Internal: lightweight JSON helpers (same pattern as nip24)          */
/* ------------------------------------------------------------------ */

static char *json_get_string(const char *json, const char *key) {
    if (!json || !key) return NULL;
    size_t klen = strlen(key);

    const char *p = json;
    while ((p = strstr(p, "\"")) != NULL) {
        ++p;
        if (strncmp(p, key, klen) != 0 || p[klen] != '"') {
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1)) p += 2;
                else ++p;
            }
            if (*p == '"') ++p;
            continue;
        }
        p += klen + 1;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p != ':') continue;
        ++p;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p != '"') continue;
        ++p;

        const char *start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p + 1)) p += 2;
            else ++p;
        }
        size_t vlen = (size_t)(p - start);
        char *result = malloc(vlen + 1);
        if (!result) return NULL;

        size_t out = 0;
        for (const char *s = start; s < p; ++s) {
            if (*s == '\\' && s + 1 < p) {
                ++s;
                switch (*s) {
                    case '"':  result[out++] = '"'; break;
                    case '\\': result[out++] = '\\'; break;
                    case 'n':  result[out++] = '\n'; break;
                    case 't':  result[out++] = '\t'; break;
                    case '/':  result[out++] = '/'; break;
                    case 'r':  result[out++] = '\r'; break;
                    default:   result[out++] = '\\'; result[out++] = *s; break;
                }
            } else {
                result[out++] = *s;
            }
        }
        result[out] = '\0';
        return result;
    }
    return NULL;
}

static int64_t json_get_int64(const char *json, const char *key) {
    if (!json || !key) return 0;
    size_t klen = strlen(key);

    const char *p = json;
    while ((p = strstr(p, "\"")) != NULL) {
        ++p;
        if (strncmp(p, key, klen) != 0 || p[klen] != '"') {
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1)) p += 2;
                else ++p;
            }
            if (*p == '"') ++p;
            continue;
        }
        p += klen + 1;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p != ':') continue;
        ++p;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;

        /* Parse integer */
        char *end = NULL;
        int64_t val = strtoll(p, &end, 10);
        if (end > p) return val;
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* URL building                                                        */
/* ------------------------------------------------------------------ */

/* Strip trailing slash from server URL */
static size_t server_len(const char *server) {
    size_t len = strlen(server);
    while (len > 0 && server[len - 1] == '/') --len;
    return len;
}

char *nostr_blossom_url_upload(const char *server) {
    if (!server) return NULL;
    size_t slen = server_len(server);
    /* server + "/upload" + '\0' */
    char *url = malloc(slen + 8);
    if (!url) return NULL;
    memcpy(url, server, slen);
    memcpy(url + slen, "/upload", 8); /* includes '\0' */
    return url;
}

char *nostr_blossom_url_download(const char *server, const char *hash) {
    if (!server || !hash) return NULL;
    size_t slen = server_len(server);
    size_t hlen = strlen(hash);
    char *url = malloc(slen + 1 + hlen + 1);
    if (!url) return NULL;
    memcpy(url, server, slen);
    url[slen] = '/';
    memcpy(url + slen + 1, hash, hlen + 1);
    return url;
}

char *nostr_blossom_url_check(const char *server, const char *hash) {
    return nostr_blossom_url_download(server, hash);
}

char *nostr_blossom_url_list(const char *server, const char *pubkey_hex) {
    if (!server || !pubkey_hex) return NULL;
    size_t slen = server_len(server);
    size_t plen = strlen(pubkey_hex);
    /* server + "/list/" + pubkey + '\0' */
    char *url = malloc(slen + 6 + plen + 1);
    if (!url) return NULL;
    memcpy(url, server, slen);
    memcpy(url + slen, "/list/", 6);
    memcpy(url + slen + 6, pubkey_hex, plen + 1);
    return url;
}

char *nostr_blossom_url_delete(const char *server, const char *hash) {
    return nostr_blossom_url_download(server, hash);
}

char *nostr_blossom_url_mirror(const char *server) {
    if (!server) return NULL;
    size_t slen = server_len(server);
    char *url = malloc(slen + 8);
    if (!url) return NULL;
    memcpy(url, server, slen);
    memcpy(url + slen, "/mirror", 8);
    return url;
}

/* ------------------------------------------------------------------ */
/* Auth event creation                                                 */
/* ------------------------------------------------------------------ */

static const char *op_to_string(NostrBlossomOp op) {
    switch (op) {
        case NOSTR_BLOSSOM_OP_UPLOAD: return "upload";
        case NOSTR_BLOSSOM_OP_GET:    return "get";
        case NOSTR_BLOSSOM_OP_LIST:   return "list";
        case NOSTR_BLOSSOM_OP_DELETE: return "delete";
        default: return NULL;
    }
}

NostrEvent *nostr_blossom_create_auth(NostrBlossomOp op,
                                       const char *hash,
                                       int expiration_secs) {
    const char *op_str = op_to_string(op);
    if (!op_str) return NULL;

    /* hash is required for upload/get/delete, optional for list */
    if (op != NOSTR_BLOSSOM_OP_LIST && !hash) return NULL;

    if (expiration_secs <= 0)
        expiration_secs = NOSTR_BLOSSOM_DEFAULT_EXPIRATION;

    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    nostr_event_set_kind(ev, NOSTR_BLOSSOM_AUTH_KIND);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, "Authorize Blossom Operation");

    /* Build tags using the tag API */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* "t" tag — operation type */
    nostr_tags_append(tags, nostr_tag_new("t", op_str, NULL));

    /* "x" tag — blob hash (skip for list) */
    if (hash) {
        nostr_tags_append(tags, nostr_tag_new("x", hash, NULL));
    }

    /* "expiration" tag */
    char exp_buf[32];
    snprintf(exp_buf, sizeof(exp_buf), "%lld",
             (long long)(time(NULL) + expiration_secs));
    nostr_tags_append(tags, nostr_tag_new("expiration", exp_buf, NULL));

    return ev;
}

/* ------------------------------------------------------------------ */
/* Blob descriptor parsing                                             */
/* ------------------------------------------------------------------ */

static int parse_single_descriptor(const char *json, size_t len,
                                    NostrBlossomBlobDescriptor *out) {
    /* Make a null-terminated copy if needed */
    char *buf = NULL;
    const char *src = json;
    if (json[len] != '\0') {
        buf = malloc(len + 1);
        if (!buf) return -ENOMEM;
        memcpy(buf, json, len);
        buf[len] = '\0';
        src = buf;
    }

    memset(out, 0, sizeof(*out));
    out->url = json_get_string(src, "url");
    out->sha256 = json_get_string(src, "sha256");
    out->size = json_get_int64(src, "size");
    out->content_type = json_get_string(src, "type");
    out->uploaded = json_get_int64(src, "uploaded");

    free(buf);
    return 0;
}

int nostr_blossom_parse_descriptor(const char *json,
                                    NostrBlossomBlobDescriptor *out) {
    if (!json || !out) return -EINVAL;
    return parse_single_descriptor(json, strlen(json), out);
}

/* Find matching brace — handles nested objects and strings */
static const char *find_closing_brace(const char *p) {
    int depth = 0;
    while (*p) {
        if (*p == '{') {
            ++depth;
        } else if (*p == '}') {
            --depth;
            if (depth == 0) return p;
        } else if (*p == '"') {
            ++p;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1)) p += 2;
                else ++p;
            }
        }
        if (*p) ++p;
    }
    return NULL;
}

int nostr_blossom_parse_descriptor_list(const char *json,
                                         NostrBlossomBlobDescriptor *out,
                                         size_t max_entries,
                                         size_t *out_count) {
    if (!json || !out || !out_count) return -EINVAL;
    *out_count = 0;

    /* Skip to opening '[' */
    const char *p = json;
    while (*p && *p != '[') ++p;
    if (!*p) return -EINVAL;
    ++p;

    size_t count = 0;
    while (*p && count < max_entries) {
        /* Skip whitespace and commas */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')
            ++p;
        if (*p == ']' || !*p) break;

        if (*p != '{') return -EINVAL;

        const char *end = find_closing_brace(p);
        if (!end) return -EINVAL;

        size_t obj_len = (size_t)(end - p + 1);
        int rc = parse_single_descriptor(p, obj_len, &out[count]);
        if (rc != 0) return rc;
        ++count;

        p = end + 1;
    }

    *out_count = count;
    return 0;
}

void nostr_blossom_descriptor_free(NostrBlossomBlobDescriptor *desc) {
    if (!desc) return;
    free(desc->url);
    free(desc->sha256);
    free(desc->content_type);
    memset(desc, 0, sizeof(*desc));
}

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

char *nostr_blossom_mirror_body(const char *remote_url) {
    if (!remote_url) return NULL;
    /* Build {"url":"<remote_url>"} */
    size_t ulen = strlen(remote_url);
    /* {"url":"..."} = 8 + ulen + 2 + 1(null) */
    size_t total = 8 + ulen + 2 + 1;
    char *body = malloc(total);
    if (!body) return NULL;
    snprintf(body, total, "{\"url\":\"%s\"}", remote_url);
    return body;
}

char *nostr_blossom_extract_hash(const char *url) {
    if (!url) return NULL;

    /* Find last '/' */
    const char *last_slash = strrchr(url, '/');
    if (!last_slash) return NULL;
    ++last_slash; /* past the slash */

    /* Find '.' for extension, or end of string */
    const char *dot = strchr(last_slash, '.');
    size_t hash_len = dot ? (size_t)(dot - last_slash) : strlen(last_slash);

    if (hash_len == 0) return NULL;

    char *hash = malloc(hash_len + 1);
    if (!hash) return NULL;
    memcpy(hash, last_slash, hash_len);
    hash[hash_len] = '\0';
    return hash;
}
