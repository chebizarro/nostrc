#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef nostr_nip44_encrypt_v2
#include "nostr/nip51/nip51.h"
#include "nostr/nip44/nip44.h"
#include "nostr-kinds.h"
#include "json.h"
#include <jansson.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SK "0000000000000000000000000000000000000000000000000000000000000001"
static long fail_at = -1, calls;
static bool encrypt_fail;
/* An exhausted allocation budget stays exhausted; a backend may legitimately
 * recover from a single optional scratch allocation failing. */
static bool failing(void) { long call = calls++; return fail_at >= 0 && call >= fail_at; }
void *nip51_test_malloc(size_t n) { return failing() ? NULL : malloc(n); }
void *nip51_test_calloc(size_t n, size_t s) { return failing() ? NULL : calloc(n, s); }
void *nip51_test_realloc(void *p, size_t n) { return failing() ? NULL : realloc(p, n); }
char *nip51_test_strdup(const char *s) { return failing() ? NULL : strdup(s); }
int nip51_test_encrypt(const uint8_t sk[32], const uint8_t pk[32], const uint8_t *plain, size_t n, char **out) {
    if (encrypt_fail) { *out = NULL; return -EIO; }
    return nostr_nip44_encrypt_v2(sk, pk, plain, n, out);
}
int main(void) {
    nostr_json_init();
    NostrList *list = nostr_nip51_list_new(); assert(list);
    nostr_nip51_mute_word(list, "public", false);
    nostr_nip51_mute_word(list, "private \"\\\n", true);
    encrypt_fail = true;
    assert(!nostr_nip51_create_mute_list(list, SK)); /* the original defect signs empty content */
    encrypt_fail = false;
    calls = 0;
    NostrEvent *ev = nostr_nip51_create_mute_list(list, SK); assert(ev);
    long allocations = calls; nostr_event_free(ev);
    for (long i = 0; i < allocations; i++) {
        calls = 0; fail_at = i;
        ev = nostr_nip51_create_mute_list(list, SK);
        fail_at = -1;
        assert(!ev); /* no omitted metadata, entries or empty private replacement */
    }
    /* Exercise JSON backend allocation failures too, including serialization. */
    json_malloc_t old_malloc; json_free_t old_free;
    json_get_alloc_funcs(&old_malloc, &old_free);
    json_set_alloc_funcs(nip51_test_malloc, free);
    calls = 0; fail_at = -1;
    ev = nostr_nip51_create_mute_list(list, SK); assert(ev);
    allocations = calls; nostr_event_free(ev);
    for (long i = 0; i < allocations; i++) {
        calls = 0; fail_at = i;
        ev = nostr_nip51_create_mute_list(list, SK);
        fail_at = -1;
        assert(!ev);
    }
    json_set_alloc_funcs(old_malloc, old_free);
    nostr_nip51_list_free(list);
    list = nostr_nip51_list_new(); assert(list);
    nostr_nip51_mute_word(list, "keep", false);
    calls = 0; fail_at = 0; nostr_nip51_mute_word(list, "cannot allocate", false); fail_at = -1;
    assert(!nostr_nip51_create_mute_list(list, SK)); nostr_nip51_list_free(list);
    list = nostr_nip51_list_new(); assert(list);
    for (int i = 0; i < 8; i++) nostr_nip51_mute_word(list, "keep", false);
    NostrListEntry *entry = nostr_nip51_entry_new("word", "cannot grow", NULL, false); assert(entry);
    calls = 0; fail_at = 0; nostr_nip51_list_add_entry(list, entry); fail_at = -1;
    assert(!nostr_nip51_create_mute_list(list, SK)); nostr_nip51_list_free(list);
    list = nostr_nip51_list_new(); assert(list);
    calls = 0; fail_at = 0; nostr_nip51_list_set_title(list, "cannot allocate"); fail_at = -1;
    assert(!nostr_nip51_create_mute_list(list, SK)); nostr_nip51_list_free(list);
    nostr_json_cleanup();
    printf("NIP-51 fail-closed encryption and %ld allocation budgets passed\n", allocations);
    return 0;
}
