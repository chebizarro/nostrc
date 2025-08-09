#include <nostr/nostr.h>
#include <nostr/nip04.h>
#include <nostr/nip05.h>
#include <nostr/nip06.h>
#include <nostr/nip10.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    // Set up and initialize the JSON interface
    nostr_set_json_interface(&cjson_interface);
    nostr_json_init();

    // Example NIP-10 usage
    Tags tags = {
        .count = 3,
        .data = (Tag*[]) {
            &(Tag){.size = 4, .data = (char*[]){"e", "tag1", "", "root"}},
            &(Tag){.size = 2, .data = (char*[]){"e", "tag2"}},
            &(Tag){.size = 4, .data = (char*[]){"e", "tag3", "", "reply"}}
        }
    };

    Tag* root_tag = get_thread_root(&tags);
    if (root_tag) {
        printf("Thread Root Tag: %s\n", root_tag->data[1]);
    } else {
        printf("Thread Root Tag not found\n");
    }

    Tag* reply_tag = get_immediate_reply(&tags);
    if (reply_tag) {
        printf("Immediate Reply Tag: %s\n", reply_tag->data[1]);
    } else {
        printf("Immediate Reply Tag not found\n");
    }

    // Clean up
    nostr_json_cleanup();

    return 0;
}
