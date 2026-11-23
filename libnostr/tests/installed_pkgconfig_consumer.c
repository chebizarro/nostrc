/* Installed static consumer: no source/build-tree includes or hand-added libs. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <nostr-tag.h>
#include <nostr-keys.h>
#include <nostr-relay.h>
#include <libnostr_store.h>

int main(int argc, char **argv) {
    assert(argc == 3);
    NostrTag *tag = nostr_tag_new("p", "installed-consumer", NULL);
    assert(tag && strcmp(nostr_tag_get_value(tag), "installed-consumer") == 0);
    nostr_tag_free(tag);
    char *pub = nostr_key_get_public("0000000000000000000000000000000000000000000000000000000000000001");
    assert(pub && strlen(pub) == 64);
    free(pub);

    GoContext *parent = go_context_background();
    Error *error = NULL;
    NostrRelay *relay = nostr_relay_new(parent, "ws://127.0.0.1:1", &error);
    assert(relay && !error);
    go_context_unref(parent);
    nostr_relay_unref(relay); /* never connects to the network */

    ln_store *store = NULL;
    int rc = ln_store_open("nostrdb", argv[1],
                           "{\"mapsize\":16777216,\"ingester_threads\":1}", &store);
    if (strcmp(argv[2], "ON") == 0) {
        assert(rc == LN_OK && store);
        ln_store_close(store);
    } else {
        assert(rc != LN_OK && !store);
    }
    return 0;
}
