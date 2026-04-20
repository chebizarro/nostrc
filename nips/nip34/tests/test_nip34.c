/**
 * @file test_nip34.c
 * @brief Unit tests for NIP-34 Git events
 *
 * SPDX-License-Identifier: MIT
 */

#include "nip34.h"
#include "nostr-event.h"
#include "nostr-tag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  %-55s ", #name); \
        test_##name(); \
        printf("OK\n"); \
        tests_passed++; \
    } while (0)

/* ---- Repository announcement (30617) ---- */

static void test_create_repo_announcement(void)
{
    const char *clones[] = {"https://git.example.com/repo.git", "git://git.example.com/repo.git", NULL};
    const char *webs[] = {"https://example.com/repo", NULL};
    const char *relays[] = {"wss://relay.example.com", NULL};
    const char *maint[] = {"aabbccdd", NULL};

    NostrEvent *ev = nip34_create_repo_announcement(
        "my-repo", "My Repo", "A test repository",
        clones, webs, relays, maint);
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == NIP34_KIND_REPOSITORY);
    assert(ev->tags != NULL);
    assert(ev->tags->count >= 6); /* d, name, description, 2x clone, web, relays, p */

    nostr_event_free(ev);
}

static void test_create_repo_announcement_minimal(void)
{
    NostrEvent *ev = nip34_create_repo_announcement(
        "minimal", "Min", NULL, NULL, NULL, NULL, NULL);
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == NIP34_KIND_REPOSITORY);
    nostr_event_free(ev);
}

static void test_create_repo_announcement_null(void)
{
    assert(nip34_create_repo_announcement(NULL, "name", NULL, NULL, NULL, NULL, NULL) == NULL);
    assert(nip34_create_repo_announcement("id", NULL, NULL, NULL, NULL, NULL, NULL) == NULL);
}

static void test_parse_repository_roundtrip(void)
{
    const char *clones[] = {"https://git.example.com/repo.git", NULL};
    const char *webs[] = {"https://example.com/repo", NULL};

    NostrEvent *ev = nip34_create_repo_announcement(
        "test-repo", "Test Repo", "Description here",
        clones, webs, NULL, NULL);
    assert(ev != NULL);

    nip34_repository_t *repo = NULL;
    nip34_result_t res = nip34_parse_repository(ev, &repo);
    assert(res == NIP34_OK);
    assert(repo != NULL);
    assert(strcmp(repo->id, "test-repo") == 0);
    assert(strcmp(repo->name, "Test Repo") == 0);
    assert(strcmp(repo->description, "Description here") == 0);
    assert(repo->clone_count == 1);
    assert(strcmp(repo->clone[0], "https://git.example.com/repo.git") == 0);
    assert(repo->web_count == 1);
    assert(strcmp(repo->web[0], "https://example.com/repo") == 0);

    nip34_repository_free(repo);
    nostr_event_free(ev);
}

static void test_parse_repository_wrong_kind(void)
{
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1); /* not 30617 */

    nip34_repository_t *repo = NULL;
    assert(nip34_parse_repository(ev, &repo) == NIP34_ERR_INVALID_KIND);
    assert(repo == NULL);

    nostr_event_free(ev);
}

static void test_parse_repository_null(void)
{
    nip34_repository_t *repo = NULL;
    assert(nip34_parse_repository(NULL, &repo) == NIP34_ERR_NULL_PARAM);
    assert(nip34_parse_repository((NostrEvent *)1, NULL) == NIP34_ERR_NULL_PARAM);
}

/* ---- Repository state (30618) ---- */

static void test_create_repo_state(void)
{
    nip34_ref_t refs[] = {
        { .refname = "refs/heads/main",    .target = "aabbccdd11223344" },
        { .refname = "refs/heads/feature", .target = "55667788aabbccdd" },
        { .refname = "refs/tags/v1.0",     .target = "deadbeefdeadbeef" },
    };

    NostrEvent *ev = nip34_create_repo_state(
        "my-repo", refs, 3, "ref: refs/heads/main");
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == NIP34_KIND_REPOSITORY_STATE);

    nostr_event_free(ev);
}

static void test_create_repo_state_null(void)
{
    assert(nip34_create_repo_state(NULL, NULL, 0, NULL) == NULL);
}

static void test_parse_repo_state_roundtrip(void)
{
    nip34_ref_t refs[] = {
        { .refname = "refs/heads/main",    .target = "aabb1122" },
        { .refname = "refs/heads/develop", .target = "ccdd3344" },
    };

    NostrEvent *ev = nip34_create_repo_state(
        "state-test", refs, 2, "ref: refs/heads/main");
    assert(ev != NULL);

    nip34_repo_state_t *state = NULL;
    nip34_result_t res = nip34_parse_repo_state(ev, &state);
    assert(res == NIP34_OK);
    assert(state != NULL);
    assert(strcmp(state->repo_id, "state-test") == 0);
    assert(state->ref_count == 2);
    assert(strcmp(state->head, "ref: refs/heads/main") == 0);

    /* Find refs/heads/main */
    bool found_main = false, found_develop = false;
    for (size_t i = 0; i < state->ref_count; i++) {
        if (strcmp(state->refs[i].refname, "refs/heads/main") == 0) {
            assert(strcmp(state->refs[i].target, "aabb1122") == 0);
            found_main = true;
        }
        if (strcmp(state->refs[i].refname, "refs/heads/develop") == 0) {
            assert(strcmp(state->refs[i].target, "ccdd3344") == 0);
            found_develop = true;
        }
    }
    assert(found_main);
    assert(found_develop);

    nip34_repo_state_free(state);
    nostr_event_free(ev);
}

static void test_parse_repo_state_empty(void)
{
    NostrEvent *ev = nip34_create_repo_state("empty-repo", NULL, 0, NULL);
    assert(ev != NULL);

    nip34_repo_state_t *state = NULL;
    assert(nip34_parse_repo_state(ev, &state) == NIP34_OK);
    assert(state->ref_count == 0);
    assert(state->head == NULL);

    nip34_repo_state_free(state);
    nostr_event_free(ev);
}

/* ---- Patch (1617) ---- */

static void test_create_patch(void)
{
    NostrEvent *ev = nip34_create_patch(
        "30617:aabbccdd:my-repo",
        "From aabb1122...\n---\ndiff --git a/file.c b/file.c\n",
        "aabb1122",
        "00112233",
        "Fix buffer overflow in parser");
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == NIP34_KIND_PATCH);

    const char *content = nostr_event_get_content(ev);
    assert(content != NULL);
    assert(strstr(content, "diff --git") != NULL);

    nostr_event_free(ev);
}

static void test_create_patch_null(void)
{
    assert(nip34_create_patch(NULL, "content", NULL, NULL, NULL) == NULL);
    assert(nip34_create_patch("addr", NULL, NULL, NULL, NULL) == NULL);
}

static void test_parse_patch_roundtrip(void)
{
    NostrEvent *ev = nip34_create_patch(
        "30617:aabbccdd:repo",
        "patch content here",
        "deadbeef",
        "cafebabe",
        "Add feature X");
    assert(ev != NULL);

    nip34_patch_t *patch = NULL;
    nip34_result_t res = nip34_parse_patch(ev, &patch);
    assert(res == NIP34_OK);
    assert(patch != NULL);
    assert(strcmp(patch->repo_addr, "30617:aabbccdd:repo") == 0);
    assert(strcmp(patch->commit_id, "deadbeef") == 0);
    assert(strcmp(patch->parent_id, "cafebabe") == 0);
    assert(strcmp(patch->subject, "Add feature X") == 0);
    assert(strcmp(patch->content, "patch content here") == 0);

    nip34_patch_free(patch);
    nostr_event_free(ev);
}

/* ---- Status events (1630–1633) ---- */

static void test_create_status_open(void)
{
    NostrEvent *ev = nip34_create_status("event123", NIP34_STATUS_OPEN, "Looks good");
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == 1630);
    nostr_event_free(ev);
}

static void test_create_status_applied(void)
{
    NostrEvent *ev = nip34_create_status("event456", NIP34_STATUS_APPLIED, "Merged!");
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == 1631);
    nostr_event_free(ev);
}

static void test_create_status_closed(void)
{
    NostrEvent *ev = nip34_create_status("event789", NIP34_STATUS_CLOSED, NULL);
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == 1632);
    nostr_event_free(ev);
}

static void test_create_status_draft(void)
{
    NostrEvent *ev = nip34_create_status("eventabc", NIP34_STATUS_DRAFT, "WIP");
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == 1633);
    nostr_event_free(ev);
}

static void test_create_status_null(void)
{
    assert(nip34_create_status(NULL, NIP34_STATUS_OPEN, NULL) == NULL);
}

static void test_create_status_invalid_kind(void)
{
    assert(nip34_create_status("ev", (nip34_status_kind_t)9999, NULL) == NULL);
}

/* ---- Error strings ---- */

static void test_strerror(void)
{
    assert(strcmp(nip34_strerror(NIP34_OK), "Success") == 0);
    assert(strcmp(nip34_strerror(NIP34_ERR_NULL_PARAM), "Null parameter") == 0);
    assert(strcmp(nip34_strerror(NIP34_ERR_INVALID_KIND), "Invalid event kind") == 0);
    const char *unk = nip34_strerror((nip34_result_t)-999);
    assert(unk != NULL);
}

/* Free NULL should not crash */
static void test_free_null(void)
{
    nip34_repository_free(NULL);
    nip34_repo_state_free(NULL);
    nip34_patch_free(NULL);
}

/* ---- Main ---- */

int main(void)
{
    printf("NIP-34 Git event tests\n");
    printf("======================\n");

    /* Repository */
    TEST(create_repo_announcement);
    TEST(create_repo_announcement_minimal);
    TEST(create_repo_announcement_null);
    TEST(parse_repository_roundtrip);
    TEST(parse_repository_wrong_kind);
    TEST(parse_repository_null);

    /* Repository state */
    TEST(create_repo_state);
    TEST(create_repo_state_null);
    TEST(parse_repo_state_roundtrip);
    TEST(parse_repo_state_empty);

    /* Patch */
    TEST(create_patch);
    TEST(create_patch_null);
    TEST(parse_patch_roundtrip);

    /* Status */
    TEST(create_status_open);
    TEST(create_status_applied);
    TEST(create_status_closed);
    TEST(create_status_draft);
    TEST(create_status_null);
    TEST(create_status_invalid_kind);

    /* Misc */
    TEST(strerror);
    TEST(free_null);

    printf("\n%d passed, 0 failed\n", tests_passed);
    return 0;
}
