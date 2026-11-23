#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
#include <nostr-event.h>
#include <nostr/nip78/nip78.h>
#include <nostr/nip78/nip78-version.h>
#ifndef NIP78_ONLY
#include <nostr/nip11/nip11.h>
#ifndef SHARED_NIPS_ONLY
#include <nostr/nip17/nip17.h>
#include <nostr/nip46/nip46_uri.h>
#include <nostr/nip51/nip51.h>
#include <nostr/nip04.h>
#include <nostr/nip44/nip44.h>
#endif
#include <nostr/nip98/nip98.h>
#include <nostr/nip11/nip11-version.h>
#ifndef SHARED_NIPS_ONLY
#include <nostr/nip17/nip17-version.h>
#include <nostr/nip46/nip46-version.h>
#include <nostr/nip51/nip51-version.h>
#endif
#include <nostr/nip98/nip98-version.h>
#ifdef NIP46_GLIB
#include <nostr/nip46/nip46_client_g.h>
#endif
#endif

int main(void) {
    const char *pk = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    NostrEvent *app = nostr_nip78_build_event(pk, "installed", "opaque", 1);
    assert(app);
    NostrNip78Document *document = NULL;
    assert(nostr_nip78_parse_event(app, &document) == 0);
    assert(document && strcmp(document->identifier, "installed") == 0);
    assert(strcmp(document->content, "opaque") == 0);
    nostr_nip78_document_free(document);
    nostr_event_free(app);
#ifndef NIP78_ONLY
    RelayInformationDocument *info = nostr_nip11_parse_info("{\"name\":\"installed\"}");
    assert(info);
    nostr_nip11_free_info(info);
#ifndef SHARED_NIPS_ONLY
    NostrEvent *rumor = nostr_nip17_create_rumor(pk, pk, "installed", 1);
    assert(rumor);
    nostr_event_free(rumor);
    const char *relays[] = {"wss://nos.lol", NULL};
    NostrEvent *relay_list = nostr_nip17_build_dm_relay_list_unsigned(relays, pk, 1);
    assert(relay_list);
    nostr_event_free(relay_list);
    NostrNip46BunkerURI uri = {0};
    char bunker[80];
    strcpy(bunker, "bunker://");
    strcat(bunker, pk);
    assert(nostr_nip46_uri_parse_bunker(bunker, &uri) == 0);
    nostr_nip46_uri_bunker_free(&uri);
    NostrList *list = nostr_nip51_list_new();
    assert(list);
    nostr_nip51_list_free(list);
    char *plaintext = NULL;
    assert(nostr_nip04_decrypt("invalid", pk, pk, &plaintext, NULL) != 0);
    uint8_t zero[32] = {0}, key[32];
    assert(nostr_nip44_convkey(zero, zero, key) != 0);
#endif
    NostrEvent *auth = nostr_nip98_create_auth_event("https://example.com/", "GET", NULL);
    assert(auth);
    nostr_event_free(auth);
#ifdef NIP46_GLIB
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_return_pointer(task, g_strdup("installed"), g_free);
    char *result = nostr_nip46_client_get_public_key_rpc_g_finish(G_ASYNC_RESULT(task), NULL);
    assert(result && strcmp(result, "installed") == 0);
    g_free(result);
    g_object_unref(task);
#endif
#endif
    return 0;
}
