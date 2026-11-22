#undef NDEBUG
#include "nostr-utils.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *cases[][2] = {
        {"http://Example.COM/Path", "ws://example.com/Path"},
        {"https://Example.COM/Path", "wss://example.com/Path"},
        {"ws://EXAMPLE.COM/Path/", "ws://example.com/Path"},
        {"WSS://EXAMPLE.COM/Path?Token=AbC", "wss://example.com/Path?Token=AbC"},
        {"Example.COM/Path", "wss://example.com/Path"},
        {"localhost:8080/Path", "ws://localhost:8080/Path"},
        {"  HTTP://EXAMPLE.COM/Path/  ", "ws://example.com/Path"},
        {"https://User:Pass@EXAMPLE.COM/Case#Frag", "wss://User:Pass@example.com/Case#Frag"},
        {"  ", ""}, {NULL, ""}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *url = nostr_normalize_url(cases[i][0]);
        assert(url && strcmp(url, cases[i][1]) == 0);
        free(url);
    }
    return 0;
}
