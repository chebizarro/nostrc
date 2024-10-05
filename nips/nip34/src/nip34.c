#include "nostr/nip34.h"
#include <stdlib.h>
#include <string.h>

nostr_patch_t* nostr_parse_patch(const nostr_event_t *event) {
    nostr_patch_t *patch = malloc(sizeof(nostr_patch_t));
    if (!patch) return NULL;

    patch->event = *event;
    patch->tags_len = event->tags_len;
    patch->tags = malloc(sizeof(nostr_tag_t) * event->tags_len);
    if (!patch->tags) {
        free(patch);
        return NULL;
    }
    memcpy(patch->tags, event->tags, sizeof(nostr_tag_t) * event->tags_len);

    for (size_t i = 0; i < event->tags_len; ++i) {
        if (strcmp(event->tags[i].key, "a") == 0) {
            char *token = strtok(event->tags[i].value, ":");
            if (token) patch->repository.kind = atoi(token);
            token = strtok(NULL, ":");
            if (token) patch->repository.public_key = strdup(token);
            token = strtok(NULL, ":");
            if (token) patch->repository.identifier = strdup(token);
            if (event->tags[i].relay) {
                patch->repository.relays = malloc(sizeof(char*));
                patch->repository.relays[0] = strdup(event->tags[i].relay);
            }
        }
    }

    return patch;
}

nostr_repository_t* nostr_parse_repository(const nostr_event_t *event) {
    nostr_repository_t *repo = malloc(sizeof(nostr_repository_t));
    if (!repo) return NULL;

    repo->event = *event;

    for (size_t i = 0; i < event->tags_len; ++i) {
        if (strcmp(event->tags[i].key, "d") == 0) {
            repo->id = strdup(event->tags[i].value);
        } else if (strcmp(event->tags[i].key, "name") == 0) {
            repo->name = strdup(event->tags[i].value);
        } else if (strcmp(event->tags[i].key, "description") == 0) {
            repo->description = strdup(event->tags[i].value);
        } else if (strcmp(event->tags[i].key, "web") == 0) {
            repo->web = malloc(sizeof(char*));
            repo->web[0] = strdup(event->tags[i].value);
            repo->web_len = 1;
        } else if (strcmp(event->tags[i].key, "clone") == 0) {
            repo->clone = malloc(sizeof(char*));
            repo->clone[0] = strdup(event->tags[i].value);
            repo->clone_len = 1;
        } else if (strcmp(event->tags[i].key, "relays") == 0) {
            repo->relays = malloc(sizeof(char*));
            repo->relays[0] = strdup(event->tags[i].value);
            repo->relays_len = 1;
        } else if (strcmp(event->tags[i].key, "r") == 0) {
            repo->earliest_unique_commit_id = strdup(event->tags[i].value);
        } else if (strcmp(event->tags[i].key, "maintainers") == 0) {
            repo->maintainers = malloc(sizeof(char*));
            repo->maintainers[0] = strdup(event->tags[i].value);
            repo->maintainers_len = 1;
        }
    }

    return repo;
}

void nostr_free_patch(nostr_patch_t *patch) {
    if (!patch) return;
    if (patch->tags) free(patch->tags);
    if (patch->repository.public_key) free(patch->repository.public_key);
    if (patch->repository.identifier) free(patch->repository.identifier);
    if (patch->repository.relays) free(patch->repository.relays);
    free(patch);
}

void nostr_free_repository(nostr_repository_t *repo) {
    if (!repo) return;
    if (repo->id) free(repo->id);
    if (repo->name) free(repo->name);
    if (repo->description) free(repo->description);
    if (repo->web) free(repo->web);
    if (repo->clone) free(repo->clone);
    if (repo->relays) free(repo->relays);
    if (repo->earliest_unique_commit_id) free(repo->earliest_unique_commit_id);
    if (repo->maintainers) free(repo->maintainers);
    free(repo);
}
