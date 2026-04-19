#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nostr/nip27/nip27.h"

/*
 * Known valid bech32 strings for testing.
 * These are real NIP-19 encoded values.
 */
static const char *TEST_NPUB = "npub10elfcs4fr0l0r8af98jlmgdh9c8tcxjvz9qkw038js35mp4dma8qzvjptg";
static const char *TEST_NOTE = "note1gmtnz6q2m55epmlpe3semjdcq987av3jvx4emmjsa8g3s9x7tg4sclreky";

static void test_no_mentions(void) {
    const char *content = "Hello world, no references here.";

    assert(nostr_nip27_count_mentions(content) == 0);

    NostrNip27Block blocks[8];
    size_t count = 0;
    assert(nostr_nip27_parse(content, blocks, 8, &count) == 0);
    assert(count == 1);
    assert(blocks[0].type == NOSTR_NIP27_TEXT);
    assert(blocks[0].length == strlen(content));
    assert(strncmp(blocks[0].text, content, blocks[0].length) == 0);
}

static void test_single_mention(void) {
    /* Build content with an npub reference */
    char content[256];
    snprintf(content, sizeof(content), "Hello nostr:%s!", TEST_NPUB);

    assert(nostr_nip27_count_mentions(content) == 1);

    NostrNip27Block blocks[8];
    size_t count = 0;
    assert(nostr_nip27_parse(content, blocks, 8, &count) == 0);
    assert(count == 3);  /* "Hello " + mention + "!" */

    /* First block: "Hello " */
    assert(blocks[0].type == NOSTR_NIP27_TEXT);
    assert(blocks[0].length == 6);
    assert(strncmp(blocks[0].text, "Hello ", 6) == 0);

    /* Second block: mention */
    assert(blocks[1].type == NOSTR_NIP27_MENTION);
    assert(blocks[1].bech32_type == NOSTR_B32_NPUB);
    assert(blocks[1].bech32 != NULL);
    size_t npub_len = strlen(TEST_NPUB);
    assert(strncmp(blocks[1].bech32, TEST_NPUB, npub_len) == 0);
    assert(blocks[1].length == 6 + npub_len);  /* "nostr:" + bech32 */

    /* Third block: "!" */
    assert(blocks[2].type == NOSTR_NIP27_TEXT);
    assert(blocks[2].length == 1);
    assert(blocks[2].text[0] == '!');
}

static void test_multiple_mentions(void) {
    char content[512];
    snprintf(content, sizeof(content),
             "User nostr:%s wrote nostr:%s today",
             TEST_NPUB, TEST_NOTE);

    assert(nostr_nip27_count_mentions(content) == 2);

    NostrNip27Block blocks[16];
    size_t count = 0;
    assert(nostr_nip27_parse(content, blocks, 16, &count) == 0);
    assert(count == 5);  /* text + mention + text + mention + text */

    assert(blocks[0].type == NOSTR_NIP27_TEXT);
    assert(strncmp(blocks[0].text, "User ", 5) == 0);

    assert(blocks[1].type == NOSTR_NIP27_MENTION);
    assert(blocks[1].bech32_type == NOSTR_B32_NPUB);

    assert(blocks[2].type == NOSTR_NIP27_TEXT);
    assert(strncmp(blocks[2].text, " wrote ", 7) == 0);

    assert(blocks[3].type == NOSTR_NIP27_MENTION);
    assert(blocks[3].bech32_type == NOSTR_B32_NOTE);

    assert(blocks[4].type == NOSTR_NIP27_TEXT);
    assert(strncmp(blocks[4].text, " today", 6) == 0);
}

static void test_mention_at_start(void) {
    char content[256];
    snprintf(content, sizeof(content), "nostr:%s is great", TEST_NPUB);

    NostrNip27Block blocks[8];
    size_t count = 0;
    assert(nostr_nip27_parse(content, blocks, 8, &count) == 0);
    assert(count == 2);  /* mention + text */

    assert(blocks[0].type == NOSTR_NIP27_MENTION);
    assert(blocks[0].bech32_type == NOSTR_B32_NPUB);

    assert(blocks[1].type == NOSTR_NIP27_TEXT);
    assert(strncmp(blocks[1].text, " is great", 9) == 0);
}

static void test_mention_at_end(void) {
    char content[256];
    snprintf(content, sizeof(content), "Check out nostr:%s", TEST_NPUB);

    NostrNip27Block blocks[8];
    size_t count = 0;
    assert(nostr_nip27_parse(content, blocks, 8, &count) == 0);
    assert(count == 2);  /* text + mention */

    assert(blocks[0].type == NOSTR_NIP27_TEXT);
    assert(blocks[1].type == NOSTR_NIP27_MENTION);
}

static void test_only_mention(void) {
    char content[256];
    snprintf(content, sizeof(content), "nostr:%s", TEST_NPUB);

    NostrNip27Block blocks[8];
    size_t count = 0;
    assert(nostr_nip27_parse(content, blocks, 8, &count) == 0);
    assert(count == 1);

    assert(blocks[0].type == NOSTR_NIP27_MENTION);
    assert(blocks[0].bech32_type == NOSTR_B32_NPUB);
}

static void test_invalid_mention_ignored(void) {
    const char *content = "Check nostr:invalid and nostr:llll for fun";

    assert(nostr_nip27_count_mentions(content) == 0);

    NostrNip27Block blocks[8];
    size_t count = 0;
    assert(nostr_nip27_parse(content, blocks, 8, &count) == 0);
    assert(count == 1);  /* Single text block */
    assert(blocks[0].type == NOSTR_NIP27_TEXT);
    assert(blocks[0].length == strlen(content));
}

static void test_fake_nostr_prefix(void) {
    /* "nostr:" followed by garbage */
    const char *content = "Try nostr: with nothing";

    assert(nostr_nip27_count_mentions(content) == 0);

    NostrNip27Block blocks[8];
    size_t count = 0;
    assert(nostr_nip27_parse(content, blocks, 8, &count) == 0);
    assert(count == 1);
    assert(blocks[0].type == NOSTR_NIP27_TEXT);
}

static void test_empty_content(void) {
    NostrNip27Block blocks[4];
    size_t count = 0;
    assert(nostr_nip27_parse("", blocks, 4, &count) == 0);
    assert(count == 0);
}

/* Formatter callback for replace tests */
static bool test_formatter(const char *bech32, NostrBech32Type type,
                            const char **out_text, size_t *out_len,
                            void *user_data) {
    (void)bech32;
    (void)user_data;
    if (type == NOSTR_B32_NPUB) {
        *out_text = "@user";
        *out_len = 5;
        return true;
    }
    if (type == NOSTR_B32_NOTE) {
        *out_text = "[note]";
        *out_len = 6;
        return true;
    }
    return false;
}

static void test_replace(void) {
    char content[256];
    snprintf(content, sizeof(content),
             "Hello nostr:%s check nostr:%s!",
             TEST_NPUB, TEST_NOTE);

    char *result = nostr_nip27_replace(content, test_formatter, NULL);
    assert(result != NULL);
    assert(strcmp(result, "Hello @user check [note]!") == 0);

    free(result);
}

static void test_replace_no_mentions(void) {
    const char *content = "Just plain text here";
    char *result = nostr_nip27_replace(content, test_formatter, NULL);
    assert(result != NULL);
    assert(strcmp(result, content) == 0);
    free(result);
}

static void test_invalid_inputs(void) {
    NostrNip27Block blocks[4];
    size_t count;

    assert(nostr_nip27_parse(NULL, blocks, 4, &count) == -EINVAL);
    assert(nostr_nip27_parse("test", NULL, 4, &count) == -EINVAL);
    assert(nostr_nip27_parse("test", blocks, 4, NULL) == -EINVAL);

    assert(nostr_nip27_count_mentions(NULL) == 0);

    assert(nostr_nip27_replace(NULL, test_formatter, NULL) == NULL);
    assert(nostr_nip27_replace("test", NULL, NULL) == NULL);
}

int main(void) {
    test_no_mentions();
    test_single_mention();
    test_multiple_mentions();
    test_mention_at_start();
    test_mention_at_end();
    test_only_mention();
    test_invalid_mention_ignored();
    test_fake_nostr_prefix();
    test_empty_content();
    test_replace();
    test_replace_no_mentions();
    test_invalid_inputs();
    printf("nip27 ok\n");
    return 0;
}
