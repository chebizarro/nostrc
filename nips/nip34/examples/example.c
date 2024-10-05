#include <stdio.h>
#include "nostr/nip34.h"
#include "nostr/event.h"

int main() {
    // Create a sample event with relevant tags
    nostr_event_t event;
    nostr_event_init(&event);
    nostr_event_add_tag(&event, "d", "repo-id");
    nostr_event_add_tag(&event, "name", "repo-name");
    nostr_event_add_tag(&event, "description", "repo-description");
    nostr_event_add_tag(&event, "web", "https://example.com");
    nostr_event_add_tag(&event, "clone", "https://github.com/example/repo.git");
    nostr_event_add_tag(&event, "relays", "wss://relay.example.com");
    nostr_event_add_tag(&event, "r", "commit-id");
    nostr_event_add_tag(&event, "maintainers", "maintainer-key");

    // Parse the repository from the event
    nostr_repository_t *repo = nostr_parse_repository(&event);
    if (repo) {
        printf("Repository ID: %s\n", repo->id);
        printf("Repository Name: %s\n", repo->name);
        printf("Repository Description: %s\n", repo->description);
        printf("Repository Web: %s\n", repo->web[0]);
        printf("Repository Clone: %s\n", repo->clone[0]);
        printf("Repository Relays: %s\n", repo->relays[0]);
        printf("Repository Earliest Commit ID: %s\n", repo->earliest_unique_commit_id);
        printf("Repository Maintainers: %s\n", repo->maintainers[0]);

        // Free the repository
        nostr_free_repository(repo);
    } else {
        printf("Failed to parse repository\n");
    }

    // Free the event
    nostr_event_free(&event);

    return 0;
}
