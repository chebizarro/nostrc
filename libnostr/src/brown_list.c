/**
 * brown_list.c - Relay brown list for persistently failing relays (nostrc-py1)
 *
 * Implementation of the "brown list" - a soft ban mechanism for relays
 * that consistently fail to connect.
 */

#include "nostr-brown-list.h"
#include "nostr/metrics.h"
#include "nostr_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>

/* Default configuration values */
#define DEFAULT_THRESHOLD       3       /* 3 consecutive failures */
#define DEFAULT_TIMEOUT_SEC     1800    /* 30 minutes */
#define MIN_THRESHOLD           1
#define MIN_TIMEOUT_SEC         60      /* 1 minute minimum */
#define MAX_ENTRIES             1000    /* Prevent unbounded growth */

/* Get current time in seconds */
static time_t now_seconds(void) {
    return time(NULL);
}

/* Find an entry by URL (caller holds mutex) */
static NostrBrownListEntry *find_entry(NostrBrownList *list, const char *url) {
    if (!list || !url) return NULL;

    for (NostrBrownListEntry *e = list->entries; e; e = e->next) {
        if (e->url && strcmp(e->url, url) == 0) {
            return e;
        }
    }
    return NULL;
}

/* Create a new entry (caller holds mutex) */
static NostrBrownListEntry *create_entry(NostrBrownList *list, const char *url) {
    if (!list || !url) return NULL;

    /* Check entry limit */
    if (list->entry_count >= MAX_ENTRIES) {
        /* Evict oldest expired entry, or oldest entry if none expired */
        time_t now = now_seconds();
        NostrBrownListEntry *oldest = NULL;
        NostrBrownListEntry **oldest_ptr = NULL;
        NostrBrownListEntry **prev_ptr = &list->entries;

        for (NostrBrownListEntry *e = list->entries; e; prev_ptr = &e->next, e = e->next) {
            /* Prefer expired entries */
            if (e->expires_at > 0 && e->expires_at <= now) {
                /* Found expired - remove it */
                *prev_ptr = e->next;
                free(e->url);
                free(e);
                list->entry_count--;
                goto create_new;
            }
            /* Track oldest for fallback */
            if (!oldest || e->last_failure_time < oldest->last_failure_time) {
                oldest = e;
                oldest_ptr = prev_ptr;
            }
        }

        /* No expired entries - evict oldest */
        if (oldest && oldest_ptr) {
            *oldest_ptr = oldest->next;
            free(oldest->url);
            free(oldest);
            list->entry_count--;
        }
    }

create_new: ;  /* Empty statement after label (C99/C11 compatible) */
    NostrBrownListEntry *entry = (NostrBrownListEntry *)calloc(1, sizeof(NostrBrownListEntry));
    if (!entry) return NULL;

    entry->url = strdup(url);
    if (!entry->url) {
        free(entry);
        return NULL;
    }

    /* Insert at head */
    entry->next = list->entries;
    list->entries = entry;
    list->entry_count++;

    return entry;
}

/* Remove an entry (caller holds mutex) - reserved for future use */
#if defined(__GNUC__)
__attribute__((unused))
#endif
static void remove_entry(NostrBrownList *list, const char *url) {
    if (!list || !url) return;

    NostrBrownListEntry **prev_ptr = &list->entries;
    for (NostrBrownListEntry *e = list->entries; e; prev_ptr = &e->next, e = e->next) {
        if (e->url && strcmp(e->url, url) == 0) {
            *prev_ptr = e->next;
            free(e->url);
            free(e);
            list->entry_count--;
            return;
        }
    }
}

/* Check if an entry is expired (caller holds mutex) */
static bool is_expired(NostrBrownListEntry *entry) {
    if (!entry || entry->expires_at == 0) return false;
    return now_seconds() >= entry->expires_at;
}

/* Handle expiry of an entry (caller holds mutex) */
static void handle_expiry(NostrBrownListEntry *entry) {
    if (!entry) return;
    entry->browned_at = 0;
    entry->expires_at = 0;
    entry->failure_count = 0;
    nostr_metric_counter_add("brown_list_expired", 1);
}

/* Auto-save if persistence is enabled */
static void maybe_save(NostrBrownList *list) {
    if (list && list->storage_path) {
        nostr_brown_list_save(list);
    }
}

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

NostrBrownList *nostr_brown_list_new(void) {
    return nostr_brown_list_new_with_config(DEFAULT_THRESHOLD, DEFAULT_TIMEOUT_SEC);
}

NostrBrownList *nostr_brown_list_new_with_config(int threshold, int timeout_seconds) {
    NostrBrownList *list = (NostrBrownList *)calloc(1, sizeof(NostrBrownList));
    if (!list) return NULL;

    list->threshold = (threshold >= MIN_THRESHOLD) ? threshold : DEFAULT_THRESHOLD;
    list->timeout_seconds = (timeout_seconds >= MIN_TIMEOUT_SEC) ? timeout_seconds : DEFAULT_TIMEOUT_SEC;

    /* Initialize mutex */
    pthread_mutex_t *mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (!mutex) {
        free(list);
        return NULL;
    }
    pthread_mutex_init(mutex, NULL);
    list->mutex = mutex;

    nostr_metric_counter_add("brown_list_created", 1);

    return list;
}

void nostr_brown_list_free(NostrBrownList *list) {
    if (!list) return;

    /* Maybe save before freeing */
    maybe_save(list);

    /* Free all entries */
    NostrBrownListEntry *entry = list->entries;
    while (entry) {
        NostrBrownListEntry *next = entry->next;
        free(entry->url);
        free(entry);
        entry = next;
    }

    /* Free mutex */
    if (list->mutex) {
        pthread_mutex_destroy((pthread_mutex_t *)list->mutex);
        free(list->mutex);
    }

    /* Free storage path */
    if (list->storage_path) {
        free(list->storage_path);
    }

    free(list);
}

/* ========================================================================
 * Configuration
 * ======================================================================== */

void nostr_brown_list_set_threshold(NostrBrownList *list, int threshold) {
    if (!list || !list->mutex) return;
    pthread_mutex_lock((pthread_mutex_t *)list->mutex);
    list->threshold = (threshold >= MIN_THRESHOLD) ? threshold : MIN_THRESHOLD;
    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
}

int nostr_brown_list_get_threshold(NostrBrownList *list) {
    if (!list) return DEFAULT_THRESHOLD;
    pthread_mutex_lock((pthread_mutex_t *)list->mutex);
    int result = list->threshold;
    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
    return result;
}

void nostr_brown_list_set_timeout(NostrBrownList *list, int timeout_seconds) {
    if (!list || !list->mutex) return;
    pthread_mutex_lock((pthread_mutex_t *)list->mutex);
    list->timeout_seconds = (timeout_seconds >= MIN_TIMEOUT_SEC) ? timeout_seconds : MIN_TIMEOUT_SEC;
    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
}

int nostr_brown_list_get_timeout(NostrBrownList *list) {
    if (!list) return DEFAULT_TIMEOUT_SEC;
    pthread_mutex_lock((pthread_mutex_t *)list->mutex);
    int result = list->timeout_seconds;
    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
    return result;
}

/* ========================================================================
 * Recording failures and successes
 * ======================================================================== */

bool nostr_brown_list_record_failure(NostrBrownList *list, const char *url) {
    if (!list || !url || !list->mutex) return false;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    /* Find or create entry */
    NostrBrownListEntry *entry = find_entry(list, url);
    if (!entry) {
        entry = create_entry(list, url);
        if (!entry) {
            pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
            return false;
        }
    }

    /* Check if already expired and needs reset */
    if (is_expired(entry)) {
        handle_expiry(entry);
    }

    /* If already browned and not expired, just return */
    if (entry->browned_at > 0) {
        pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
        return true;
    }

    /* Increment failure count */
    entry->failure_count++;
    entry->last_failure_time = now_seconds();

    nostr_metric_counter_add("brown_list_failure_recorded", 1);

    bool newly_browned = false;

    /* Check if we should brown-list */
    if (entry->failure_count >= list->threshold) {
        /* Only brown-list if network is healthy (at least one relay connected)
         * OR if we've never seen any successful connection */
        bool network_healthy = (list->connected_count > 0) ||
                              (list->last_any_success > 0 &&
                               (now_seconds() - list->last_any_success) < 300);

        if (network_healthy) {
            entry->browned_at = now_seconds();
            entry->expires_at = entry->browned_at + list->timeout_seconds;
            newly_browned = true;

            nostr_metric_counter_add("brown_list_browned", 1);

            fprintf(stderr, "[BROWN_LIST] Relay browned: %s (failures=%d, timeout=%ds)\n",
                    url, entry->failure_count, list->timeout_seconds);
        }
    }

    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);

    if (newly_browned) {
        maybe_save(list);
    }

    return newly_browned;
}

void nostr_brown_list_record_success(NostrBrownList *list, const char *url) {
    if (!list || !url || !list->mutex) return;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    /* Update network health tracking */
    list->connected_count++;
    list->last_any_success = now_seconds();

    /* Find and reset entry */
    NostrBrownListEntry *entry = find_entry(list, url);
    if (entry) {
        bool was_browned = (entry->browned_at > 0);

        entry->failure_count = 0;
        entry->last_failure_time = 0;
        entry->browned_at = 0;
        entry->expires_at = 0;

        if (was_browned) {
            nostr_metric_counter_add("brown_list_recovered", 1);
            fprintf(stderr, "[BROWN_LIST] Relay recovered: %s\n", url);
        }
    }

    nostr_metric_counter_add("brown_list_success_recorded", 1);

    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);

    maybe_save(list);
}

void nostr_brown_list_update_connected_count(NostrBrownList *list, int connected) {
    if (!list || !list->mutex) return;
    pthread_mutex_lock((pthread_mutex_t *)list->mutex);
    list->connected_count = connected;
    if (connected > 0) {
        list->last_any_success = now_seconds();
    }
    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
}

/* ========================================================================
 * Querying brown list status
 * ======================================================================== */

bool nostr_brown_list_is_browned(NostrBrownList *list, const char *url) {
    if (!list || !url || !list->mutex) return false;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    NostrBrownListEntry *entry = find_entry(list, url);
    if (!entry || entry->browned_at == 0) {
        pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
        return false;
    }

    /* Check expiry */
    if (is_expired(entry)) {
        handle_expiry(entry);
        pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
        maybe_save(list);
        return false;
    }

    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
    return true;
}

bool nostr_brown_list_should_skip(NostrBrownList *list, const char *url) {
    /* For now, should_skip is the same as is_browned */
    return nostr_brown_list_is_browned(list, url);
}

int nostr_brown_list_get_failure_count(NostrBrownList *list, const char *url) {
    if (!list || !url || !list->mutex) return 0;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    NostrBrownListEntry *entry = find_entry(list, url);
    int count = entry ? entry->failure_count : 0;

    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
    return count;
}

int nostr_brown_list_get_time_remaining(NostrBrownList *list, const char *url) {
    if (!list || !url || !list->mutex) return 0;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    NostrBrownListEntry *entry = find_entry(list, url);
    if (!entry || entry->expires_at == 0) {
        pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
        return 0;
    }

    time_t now = now_seconds();
    int remaining = (entry->expires_at > now) ? (int)(entry->expires_at - now) : 0;

    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
    return remaining;
}

void nostr_brown_list_get_stats(NostrBrownList *list, NostrBrownListStats *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(NostrBrownListStats));

    if (!list || !list->mutex) return;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    time_t now = now_seconds();

    for (NostrBrownListEntry *e = list->entries; e; e = e->next) {
        stats->total_entries++;

        if (e->browned_at > 0 && e->expires_at > now) {
            stats->browned_count++;
        } else if (e->failure_count > 0) {
            stats->failing_count++;
        } else {
            stats->healthy_count++;
        }
    }

    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
}

/* ========================================================================
 * Manual management
 * ======================================================================== */

bool nostr_brown_list_clear_relay(NostrBrownList *list, const char *url) {
    if (!list || !url || !list->mutex) return false;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    NostrBrownListEntry *entry = find_entry(list, url);
    if (!entry) {
        pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
        return false;
    }

    /* Reset the entry instead of removing it */
    entry->failure_count = 0;
    entry->last_failure_time = 0;
    entry->browned_at = 0;
    entry->expires_at = 0;

    nostr_metric_counter_add("brown_list_manual_clear", 1);
    fprintf(stderr, "[BROWN_LIST] Relay manually cleared: %s\n", url);

    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);

    maybe_save(list);
    return true;
}

void nostr_brown_list_clear_all(NostrBrownList *list) {
    if (!list || !list->mutex) return;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    /* Free all entries */
    NostrBrownListEntry *entry = list->entries;
    while (entry) {
        NostrBrownListEntry *next = entry->next;
        free(entry->url);
        free(entry);
        entry = next;
    }
    list->entries = NULL;
    list->entry_count = 0;

    nostr_metric_counter_add("brown_list_clear_all", 1);
    fprintf(stderr, "[BROWN_LIST] All entries cleared\n");

    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);

    maybe_save(list);
}

int nostr_brown_list_expire_stale(NostrBrownList *list) {
    if (!list || !list->mutex) return 0;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    int expired_count = 0;
    time_t now = now_seconds();

    for (NostrBrownListEntry *e = list->entries; e; e = e->next) {
        if (e->browned_at > 0 && e->expires_at <= now) {
            handle_expiry(e);
            expired_count++;
        }
    }

    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);

    if (expired_count > 0) {
        maybe_save(list);
    }

    return expired_count;
}

/* ========================================================================
 * Iteration
 * ======================================================================== */

NostrBrownListIterator *nostr_brown_list_iterator_new(NostrBrownList *list, bool only_browned) {
    if (!list) return NULL;

    NostrBrownListIterator *iter = (NostrBrownListIterator *)calloc(1, sizeof(NostrBrownListIterator));
    if (!iter) return NULL;

    iter->list = list;
    iter->current = NULL;  /* Will start from head on first next() */
    iter->only_browned = only_browned;

    return iter;
}

bool nostr_brown_list_iterator_next(NostrBrownListIterator *iter,
                                     const char **url,
                                     int *failure_count,
                                     int *time_remaining) {
    if (!iter || !iter->list || !url) return false;

    pthread_mutex_lock((pthread_mutex_t *)iter->list->mutex);

    time_t now = now_seconds();

    /* Advance to next entry */
    NostrBrownListEntry *e = iter->current ? iter->current->next : iter->list->entries;

    while (e) {
        /* Check if we should include this entry */
        bool is_browned = (e->browned_at > 0 && e->expires_at > now);

        if (!iter->only_browned || is_browned) {
            /* Found a matching entry */
            iter->current = e;
            *url = e->url;

            if (failure_count) {
                *failure_count = e->failure_count;
            }

            if (time_remaining) {
                if (e->expires_at > now) {
                    *time_remaining = (int)(e->expires_at - now);
                } else {
                    *time_remaining = 0;
                }
            }

            pthread_mutex_unlock((pthread_mutex_t *)iter->list->mutex);
            return true;
        }

        e = e->next;
    }

    /* No more entries */
    iter->current = NULL;
    pthread_mutex_unlock((pthread_mutex_t *)iter->list->mutex);
    return false;
}

void nostr_brown_list_iterator_free(NostrBrownListIterator *iter) {
    free(iter);
}

/* ========================================================================
 * Persistence
 * ======================================================================== */

bool nostr_brown_list_set_storage_path(NostrBrownList *list, const char *path) {
    if (!list || !list->mutex) return false;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    /* Free existing path */
    if (list->storage_path) {
        free(list->storage_path);
        list->storage_path = NULL;
    }

    /* Set new path */
    if (path) {
        list->storage_path = strdup(path);
        if (!list->storage_path) {
            pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
            return false;
        }
    }

    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);

    /* Try to load existing data */
    if (path) {
        nostr_brown_list_load(list);
    }

    return true;
}

bool nostr_brown_list_save(NostrBrownList *list) {
    if (!list || !list->mutex || !list->storage_path) return false;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    FILE *f = fopen(list->storage_path, "w");
    if (!f) {
        pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
        return false;
    }

    /* Simple line-based format:
     * URL<TAB>failure_count<TAB>last_failure<TAB>browned_at<TAB>expires_at
     */
    for (NostrBrownListEntry *e = list->entries; e; e = e->next) {
        if (e->url) {
            fprintf(f, "%s\t%d\t%ld\t%ld\t%ld\n",
                    e->url,
                    e->failure_count,
                    (long)e->last_failure_time,
                    (long)e->browned_at,
                    (long)e->expires_at);
        }
    }

    fclose(f);
    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);

    return true;
}

bool nostr_brown_list_load(NostrBrownList *list) {
    if (!list || !list->mutex || !list->storage_path) return false;

    pthread_mutex_lock((pthread_mutex_t *)list->mutex);

    FILE *f = fopen(list->storage_path, "r");
    if (!f) {
        /* File doesn't exist - not an error */
        pthread_mutex_unlock((pthread_mutex_t *)list->mutex);
        return true;
    }

    char line[2048];
    time_t now = now_seconds();

    while (fgets(line, sizeof(line), f)) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        /* Parse line */
        char url[1024];
        int failure_count;
        long last_failure, browned_at, expires_at;

        if (sscanf(line, "%1023[^\t]\t%d\t%ld\t%ld\t%ld",
                   url, &failure_count, &last_failure, &browned_at, &expires_at) == 5) {

            /* Skip expired entries */
            if (browned_at > 0 && expires_at <= now) {
                continue;
            }

            /* Create entry */
            NostrBrownListEntry *entry = find_entry(list, url);
            if (!entry) {
                entry = create_entry(list, url);
            }
            if (entry) {
                entry->failure_count = failure_count;
                entry->last_failure_time = (time_t)last_failure;
                entry->browned_at = (time_t)browned_at;
                entry->expires_at = (time_t)expires_at;
            }
        }
    }

    fclose(f);
    pthread_mutex_unlock((pthread_mutex_t *)list->mutex);

    return true;
}
