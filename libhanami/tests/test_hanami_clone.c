/* SPDX-License-Identifier: MIT — hanami_clone over loopback relay/Blossom/GRASP.
 *
 * Driven by clone_fixture.py: argv = blossom relay grasp tmp mode. Each mode
 * runs in a fresh process with an empty TMPDIR. */
#include <hanami/hanami.h>
#include <hanami/hanami-config.h>
#include <hanami/hanami-nostr.h>
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "keys.h"
#include <curl/curl.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *blossom, *relay, *grasp, *root;
static char *pk;
static hanami_signer_t signer;

#define OK(expr) do { int rc_ = (expr); if (rc_ != 0) { \
    const git_error *e_ = git_error_last(); \
    fprintf(stderr, "%s failed: %d (%s) at %d\n", #expr, rc_, e_ ? e_->message : "", __LINE__); \
    abort(); } } while (0)

static hanami_error_t sign_event(const char *json, char **out, void *data)
{
    (void)data;
    NostrEvent *event = nostr_event_new(); assert(event);
    assert(nostr_event_deserialize_compact(event, json, NULL) == 1);
    assert(nostr_event_sign(event, sk) == 0);
    *out = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    return *out ? HANAMI_OK : HANAMI_ERR_NOMEM;
}

typedef struct { char buf[64]; size_t n; } small_body;
static size_t receive(void *ptr, size_t size, size_t count, void *payload)
{
    small_body *b = payload; size_t n = size * count;
    if (b->n + n >= sizeof(b->buf)) return 0;
    memcpy(b->buf + b->n, ptr, n); b->n += n; b->buf[b->n] = 0; return n;
}
static long counter(const char *name)
{
    char url[256]; snprintf(url, sizeof(url), "%s/control/%s", blossom, name);
    CURL *curl = curl_easy_init(); assert(curl);
    small_body b = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    assert(curl_easy_perform(curl) == CURLE_OK);
    curl_easy_cleanup(curl);
    return strtol(b.buf, NULL, 10);
}

static hanami_config_t *fixture_config(const char *endpoint)
{
    hanami_config_t *config = NULL;
    const char *relays[] = { relay, NULL };
    OK(hanami_config_new(&config));
    OK(hanami_config_set_relays(config, relays, 1));
    hanami_config_set_allow_insecure(config, true);
    if (endpoint) OK(hanami_config_set_endpoint(config, endpoint));
    return config;
}

static void publish_server_list(const char *const *servers, size_t count)
{
    const char *relays[] = { relay, NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    OK(hanami_nostr_ctx_new(relays, &signer, &ctx));
    NostrEvent *event = nostr_event_new(); assert(event);
    nostr_event_set_kind(event, NOSTR_KIND_BLOSSOM_SERVER_LIST);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_content(event, "");
    NostrTags *tags = nostr_tags_new(0); assert(tags);
    for (size_t i = 0; i < count; i++)
        nostr_tags_append(tags, nostr_tag_new("server", servers[i], NULL));
    nostr_event_set_tags(event, tags);
    assert(nostr_event_sign(event, sk) == 0);
    OK(hanami_nostr_publish_event(ctx, event));
    nostr_event_free(event);
    hanami_nostr_ctx_free(ctx);
}

static char *repo_config_string(git_repository *repo, const char *key)
{
    git_config *config = NULL;
    git_buf value = GIT_BUF_INIT;
    OK(git_repository_config(&config, repo));
    char *out = NULL;
    if (git_config_get_string_buf(&value, config, key) == 0) out = strdup(value.ptr);
    git_buf_dispose(&value);
    git_config_free(config);
    return out;
}

static void clone_uri(char *out, size_t n) { snprintf(out, n, "nostr://%s/repo", pk); }

/* nostrc-g2g37: a 30617 whose `clone` URL is a GRASP Git Smart HTTP endpoint
 * must never be treated as the Blossom API. */
static void test_discovery(void)
{
    const char *relays[] = { relay, NULL };
    const char *clones[] = { grasp, NULL };
    OK(hanami_announce_repo("repo", "repo", "clone fixture", clones, relays, &signer));
    char uri[160], dest[PATH_MAX];
    clone_uri(uri, sizeof(uri));
    git_repository *repo = NULL;

    /* 1. Only a Git clone URL is known: fail visibly, touch nothing. */
    hanami_config_t *config = fixture_config(NULL);
    snprintf(dest, sizeof(dest), "%s/discovery-none", root);
    assert(hanami_clone(&repo, uri, dest, NULL, config) == HANAMI_ERR_CONFIG);
    assert(!repo);
    const git_error *e = git_error_last();
    assert(e && strstr(e->message, "no Blossom server known"));
    assert(access(dest, F_OK) != 0);
    assert(counter("grasp-hits") == 0);
    hanami_config_free(config);

    /* 2. Misconfigured endpoint pointing at the Git URL: the probe
     *    classifies it as not-Blossom instead of using it. */
    config = fixture_config(grasp);
    snprintf(dest, sizeof(dest), "%s/discovery-misconfigured", root);
    assert(hanami_clone(&repo, uri, dest, NULL, config) == HANAMI_ERR_BLOSSOM);
    e = git_error_last();
    assert(e && strstr(e->message, "Blossom server"));
    assert(access(dest, F_OK) != 0);
    hanami_config_free(config);

    /* 3. Owner's BUD-03 list names the Git URL first, then the Blossom server:
     *    the probe skips the Git URL and selects the Blossom server. */
    const char *servers[] = { grasp, blossom };
    publish_server_list(servers, 2);
    config = fixture_config(NULL);
    snprintf(dest, sizeof(dest), "%s/discovery-listed", root);
    OK(hanami_clone(&repo, uri, dest, NULL, config));
    char *selected = repo_config_string(repo, "hanami.endpoint");
    assert(selected && strcmp(selected, blossom) == 0);
    free(selected);
    git_repository_free(repo);
    hanami_config_free(config);
    printf("discovery: clone URL never used as Blossom; kind-10063 server selected\n");
}

int main(int argc, char **argv)
{
    if (argc != 6) { fprintf(stderr, "usage: %s blossom relay grasp tmp mode\n", argv[0]); return 2; }
    blossom = argv[1]; relay = argv[2]; grasp = argv[3]; root = argv[4];
    const char *mode = argv[5];
    git_libgit2_init();
    assert(hanami_init() == HANAMI_OK);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pk = nostr_key_get_public(sk); assert(pk);
    signer = (hanami_signer_t){ .pubkey = pk, .sign = sign_event };

    if (!strcmp(mode, "discovery")) test_discovery();
    else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }

    free(pk);
    curl_global_cleanup();
    hanami_shutdown();
    git_libgit2_shutdown();
    return 0;
}
