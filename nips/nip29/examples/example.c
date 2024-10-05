#include <stdio.h>
#include "nip29.h"
#include "nostr/event.h"

int main() {
    nostr_group_t *group = nostr_new_group("group-id'relay-url");
    if (group) {
        printf("Group created: %s\n", group->name);
        nostr_free_group(group);
    } else {
        printf("Failed to create group\n");
    }

    return 0;
}
