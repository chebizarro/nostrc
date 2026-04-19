#include "nostr/nip27/nip27.h"
#include "nostr/nip21/nip21.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * Bech32 characters: a-z, 0-9 (lowercase only, no 1, b, i, o per bech32 spec,
 * but we're generous here — NIP-19 inspect will reject invalid ones).
 * We accept alphanumeric to find the end boundary, then validate.
 */
static bool is_bech32_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/*
 * Find the end of a bech32 string starting at `start`.
 * Returns pointer to first non-bech32 character.
 */
static const char *find_bech32_end(const char *start) {
    const char *p = start;
    while (*p && is_bech32_char(*p))
        ++p;
    return p;
}

/*
 * Try to parse a nostr: mention at `pos` (pointing to the 'n' of "nostr:").
 * If valid, sets *end to one past the last bech32 char and *type to the entity type.
 * Returns true if valid mention found.
 */
static bool try_parse_mention(const char *pos, const char **end,
                               NostrBech32Type *type) {
    /* pos should point to "nostr:" */
    if (strncmp(pos, "nostr:", 6) != 0) return false;

    const char *bech32 = pos + 6;
    if (*bech32 == '\0') return false;

    /* Find end of bech32 portion */
    const char *bech_end = find_bech32_end(bech32);

    /* Minimum bech32 length: HRP (4-8) + "1" + data (at least 6 for checksum) = ~11 min */
    size_t bech_len = (size_t)(bech_end - bech32);
    if (bech_len < 10) return false;

    /* We need a null-terminated copy for NIP-19 inspect */
    char *tmp = malloc(bech_len + 1);
    if (!tmp) return false;
    memcpy(tmp, bech32, bech_len);
    tmp[bech_len] = '\0';

    NostrBech32Type t = NOSTR_B32_UNKNOWN;
    int rc = nostr_nip19_inspect(tmp, &t);
    free(tmp);

    if (rc != 0 || t == NOSTR_B32_UNKNOWN)
        return false;

    *end = bech_end;
    *type = t;
    return true;
}

/*
 * Internal: emit a block into the blocks array if there's space.
 */
static void emit_block(NostrNip27Block *blocks, size_t max_blocks,
                        size_t *count, NostrNip27BlockType type,
                        const char *text, size_t length,
                        const char *bech32, NostrBech32Type bech32_type) {
    if (*count >= max_blocks) return;
    NostrNip27Block *b = &blocks[*count];
    b->type = type;
    b->text = text;
    b->length = length;
    b->bech32 = bech32;
    b->bech32_type = bech32_type;
    (*count)++;
}

int nostr_nip27_parse(const char *content, NostrNip27Block *blocks,
                       size_t max_blocks, size_t *out_count) {
    if (!content || !blocks || !out_count) return -EINVAL;
    *out_count = 0;

    const char *pos = content;
    const char *prev = content;

    while (*pos) {
        /* Look for "nostr:" */
        const char *found = strstr(pos, "nostr:");
        if (!found) break;

        const char *mention_end = NULL;
        NostrBech32Type btype = NOSTR_B32_UNKNOWN;

        if (try_parse_mention(found, &mention_end, &btype)) {
            /* Emit text block for content before this mention */
            if (found > prev && *out_count < max_blocks) {
                emit_block(blocks, max_blocks, out_count,
                           NOSTR_NIP27_TEXT, prev, (size_t)(found - prev),
                           NULL, NOSTR_B32_UNKNOWN);
            }

            /* Emit mention block */
            if (*out_count < max_blocks) {
                emit_block(blocks, max_blocks, out_count,
                           NOSTR_NIP27_MENTION, found,
                           (size_t)(mention_end - found),
                           found + 6, btype);
            }

            pos = mention_end;
            prev = mention_end;
        } else {
            /* Not a valid mention, skip past "nostr:" */
            pos = found + 6;
        }
    }

    /* Emit trailing text */
    if (*pos || prev < content + strlen(content)) {
        size_t remaining = strlen(prev);
        if (remaining > 0 && *out_count < max_blocks) {
            emit_block(blocks, max_blocks, out_count,
                       NOSTR_NIP27_TEXT, prev, remaining,
                       NULL, NOSTR_B32_UNKNOWN);
        }
    }

    return 0;
}

size_t nostr_nip27_count_mentions(const char *content) {
    if (!content) return 0;

    size_t count = 0;
    const char *pos = content;

    while (*pos) {
        const char *found = strstr(pos, "nostr:");
        if (!found) break;

        const char *end = NULL;
        NostrBech32Type type = NOSTR_B32_UNKNOWN;

        if (try_parse_mention(found, &end, &type)) {
            ++count;
            pos = end;
        } else {
            pos = found + 6;
        }
    }
    return count;
}

char *nostr_nip27_replace(const char *content,
                           NostrNip27Formatter formatter,
                           void *user_data) {
    if (!content || !formatter) return NULL;

    /* First pass: parse to count blocks */
    size_t mention_count = nostr_nip27_count_mentions(content);
    size_t max_blocks = mention_count * 2 + 1; /* text + mention interleaved + trailing */

    NostrNip27Block *blocks = calloc(max_blocks, sizeof(NostrNip27Block));
    if (!blocks) return NULL;

    size_t block_count = 0;
    if (nostr_nip27_parse(content, blocks, max_blocks, &block_count) != 0) {
        free(blocks);
        return NULL;
    }

    /* Calculate output size */
    size_t out_len = 0;
    /* Store replacement texts temporarily */
    const char **replacements = calloc(block_count, sizeof(char *));
    size_t *rep_lens = calloc(block_count, sizeof(size_t));
    bool *replaced = calloc(block_count, sizeof(bool));
    if (!replacements || !rep_lens || !replaced) {
        free(blocks); free(replacements); free(rep_lens); free(replaced);
        return NULL;
    }

    for (size_t i = 0; i < block_count; ++i) {
        if (blocks[i].type == NOSTR_NIP27_MENTION) {
            const char *rep = NULL;
            size_t rlen = 0;
            /* Build null-terminated bech32 for the callback */
            size_t bech_len = blocks[i].length - 6; /* strip "nostr:" */
            char *bech_tmp = malloc(bech_len + 1);
            if (bech_tmp) {
                memcpy(bech_tmp, blocks[i].bech32, bech_len);
                bech_tmp[bech_len] = '\0';
                replaced[i] = formatter(bech_tmp, blocks[i].bech32_type,
                                         &rep, &rlen, user_data);
                free(bech_tmp);
            }
            if (replaced[i] && rep) {
                replacements[i] = rep;
                rep_lens[i] = rlen;
                out_len += rlen;
            } else {
                out_len += blocks[i].length;
            }
        } else {
            out_len += blocks[i].length;
        }
    }

    /* Build output */
    char *result = malloc(out_len + 1);
    if (!result) {
        free(blocks); free(replacements); free(rep_lens); free(replaced);
        return NULL;
    }

    char *wp = result;
    for (size_t i = 0; i < block_count; ++i) {
        if (blocks[i].type == NOSTR_NIP27_MENTION && replaced[i]) {
            memcpy(wp, replacements[i], rep_lens[i]);
            wp += rep_lens[i];
        } else {
            memcpy(wp, blocks[i].text, blocks[i].length);
            wp += blocks[i].length;
        }
    }
    *wp = '\0';

    free(blocks);
    free(replacements);
    free(rep_lens);
    free(replaced);
    return result;
}
