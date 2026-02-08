/**
 * @file mock_relay.c
 * @brief In-process mock relay implementation for unit tests
 *
 * This implements a mock Nostr relay that runs in-process without network I/O.
 * It intercepts messages from the NostrRelay's send_channel and responds through
 * the recv_channel, simulating a real relay.
 */

#include "nostr/testing/mock_relay.h"
#include "nostr-connection.h"
#include "go.h"
#include "select.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <nsync.h>
#include <unistd.h>

/* Internal subscription tracking */
typedef struct MockSubscription {
    char *sub_id;
    NostrFilters *filters;
    struct MockSubscription *next;
} MockSubscription;

/* Internal event storage */
typedef struct EventNode {
    NostrEvent *event;
    struct EventNode *next;
} EventNode;

/* Mock relay internal state */
struct NostrMockRelay {
    /* Configuration */
    NostrMockRelayConfig config;

    /* Connection state */
    NostrRelay *relay;
    GoChannel *send_channel;  /* Read messages from client (relay sends to us) */
    GoChannel *recv_channel;  /* Send responses to client (relay receives from us) */

    /* Shutdown coordination */
    GoChannel *shutdown_chan;
    volatile int shutdown;
    volatile int running;

    /* Seeded events store */
    EventNode *seeded_events;
    size_t seeded_count;
    nsync_mu seeded_mu;

    /* Captured published events */
    EventNode *published_events;
    size_t published_count;
    nsync_mu published_mu;
    GoChannel *publish_notify;  /* Notifies await_publish callers */

    /* Active subscriptions */
    MockSubscription *subscriptions;
    nsync_mu subs_mu;

    /* Fault injection */
    NostrMockFaultType fault_type;
    int fault_after_n;
    int operation_count;
    nsync_mu fault_mu;

    /* Statistics */
    NostrMockRelayStats stats;
    nsync_mu stats_mu;
};

/* Forward declarations */
static void *mock_relay_loop(void *arg);
static void handle_req(NostrMockRelay *mock, const char *sub_id, NostrFilters *filters);
static void handle_event(NostrMockRelay *mock, NostrEvent *event);
static void handle_close(NostrMockRelay *mock, const char *sub_id);
static int send_response(NostrMockRelay *mock, const char *json);
static void free_event_list(EventNode *head);
static void free_subscription_list(MockSubscription *head);
static bool check_fault(NostrMockRelay *mock);

/* === Configuration === */

NostrMockRelayConfig nostr_mock_relay_config_default(void) {
    NostrMockRelayConfig config = {
        .response_delay_ms = 0,
        .max_events_per_req = -1,
        .auto_eose = true,
        .validate_signatures = false,
        .simulate_auth = false,
        .auth_challenge = NULL
    };
    return config;
}

/* === Lifecycle === */

NostrMockRelay *nostr_mock_relay_new(const NostrMockRelayConfig *config) {
    NostrMockRelay *mock = calloc(1, sizeof(NostrMockRelay));
    if (!mock) return NULL;

    if (config) {
        mock->config = *config;
    } else {
        mock->config = nostr_mock_relay_config_default();
    }

    mock->shutdown_chan = go_channel_create(1);
    mock->publish_notify = go_channel_create(16);

    nsync_mu_init(&mock->seeded_mu);
    nsync_mu_init(&mock->published_mu);
    nsync_mu_init(&mock->subs_mu);
    nsync_mu_init(&mock->fault_mu);
    nsync_mu_init(&mock->stats_mu);

    return mock;
}

void nostr_mock_relay_free(NostrMockRelay *mock) {
    if (!mock) return;

    nostr_mock_relay_stop(mock);
    nostr_mock_relay_detach(mock);

    /* Free seeded events */
    nsync_mu_lock(&mock->seeded_mu);
    free_event_list(mock->seeded_events);
    mock->seeded_events = NULL;
    nsync_mu_unlock(&mock->seeded_mu);

    /* Free published events */
    nsync_mu_lock(&mock->published_mu);
    free_event_list(mock->published_events);
    mock->published_events = NULL;
    nsync_mu_unlock(&mock->published_mu);

    /* Free subscriptions */
    nsync_mu_lock(&mock->subs_mu);
    free_subscription_list(mock->subscriptions);
    mock->subscriptions = NULL;
    nsync_mu_unlock(&mock->subs_mu);

    if (mock->shutdown_chan) {
        go_channel_close(mock->shutdown_chan);
        go_channel_free(mock->shutdown_chan);
    }
    if (mock->publish_notify) {
        go_channel_close(mock->publish_notify);
        go_channel_free(mock->publish_notify);
    }

    free(mock);
}

/* === Integration === */

int nostr_mock_relay_attach(NostrMockRelay *mock, NostrRelay *relay) {
    if (!mock || !relay) return -1;
    if (!relay->connection) return -1;

    mock->relay = relay;
    mock->send_channel = relay->connection->send_channel;
    mock->recv_channel = relay->connection->recv_channel;

    return 0;
}

void nostr_mock_relay_detach(NostrMockRelay *mock) {
    if (!mock) return;

    nostr_mock_relay_stop(mock);

    mock->relay = NULL;
    mock->send_channel = NULL;
    mock->recv_channel = NULL;
}

int nostr_mock_relay_start(NostrMockRelay *mock) {
    if (!mock || !mock->send_channel || !mock->recv_channel) return -1;
    if (mock->running) return 0;  /* Already running */

    mock->shutdown = 0;
    mock->running = 1;

    if (go(mock_relay_loop, mock) != 0) {
        mock->running = 0;
        return -1;
    }

    return 0;
}

void nostr_mock_relay_stop(NostrMockRelay *mock) {
    if (!mock || !mock->running) return;

    mock->shutdown = 1;

    /* Signal shutdown via channel */
    if (mock->shutdown_chan && !go_channel_is_closed(mock->shutdown_chan)) {
        int signal = 1;
        go_channel_try_send(mock->shutdown_chan, &signal);
    }

    /* Give the loop time to exit */
    for (int i = 0; i < 100 && mock->running; i++) {
        usleep(10000);  /* 10ms */
    }
}

/* === Event Seeding === */

int nostr_mock_relay_seed_event(NostrMockRelay *mock, NostrEvent *event) {
    if (!mock || !event) return -1;

    EventNode *node = calloc(1, sizeof(EventNode));
    if (!node) return -1;

    node->event = nostr_event_copy(event);
    if (!node->event) {
        free(node);
        return -1;
    }

    nsync_mu_lock(&mock->seeded_mu);
    node->next = mock->seeded_events;
    mock->seeded_events = node;
    mock->seeded_count++;
    nsync_mu_unlock(&mock->seeded_mu);

    nsync_mu_lock(&mock->stats_mu);
    mock->stats.events_seeded++;
    nsync_mu_unlock(&mock->stats_mu);

    return 0;
}

int nostr_mock_relay_seed_events(NostrMockRelay *mock, NostrEvent **events, size_t count) {
    if (!mock || !events) return -1;

    for (size_t i = 0; i < count; i++) {
        if (nostr_mock_relay_seed_event(mock, events[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

int nostr_mock_relay_seed_from_json(NostrMockRelay *mock, const char *json_path) {
    if (!mock || !json_path) return -1;

    /* Read file contents */
    FILE *f = fopen(json_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *json = malloc(fsize + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    if (fread(json, 1, fsize, f) != (size_t)fsize) {
        free(json);
        fclose(f);
        return -1;
    }
    json[fsize] = '\0';
    fclose(f);

    /* Parse JSON array - simple implementation */
    /* For a full implementation, use jansson or similar */
    /* This is a placeholder that counts events loaded */
    int count = 0;

    /* Simple event parsing - look for event objects */
    const char *p = json;
    while ((p = strstr(p, "\"id\"")) != NULL) {
        /* Find the start of this event object */
        const char *obj_start = p;
        while (obj_start > json && *obj_start != '{') obj_start--;

        if (*obj_start == '{') {
            /* Find the end of this event object */
            int depth = 1;
            const char *obj_end = obj_start + 1;
            while (*obj_end && depth > 0) {
                if (*obj_end == '{') depth++;
                else if (*obj_end == '}') depth--;
                obj_end++;
            }

            if (depth == 0) {
                /* Extract and parse the event */
                size_t len = obj_end - obj_start;
                char *event_json = malloc(len + 1);
                if (event_json) {
                    memcpy(event_json, obj_start, len);
                    event_json[len] = '\0';

                    NostrEvent *event = nostr_event_new();
                    if (event && nostr_event_deserialize_compact(event, event_json, NULL) == 1) {
                        nostr_mock_relay_seed_event(mock, event);
                        count++;
                    }
                    if (event) nostr_event_free(event);
                    free(event_json);
                }
            }
        }
        p++;
    }

    free(json);
    return count;
}

void nostr_mock_relay_clear_events(NostrMockRelay *mock) {
    if (!mock) return;

    nsync_mu_lock(&mock->seeded_mu);
    free_event_list(mock->seeded_events);
    mock->seeded_events = NULL;
    mock->seeded_count = 0;
    nsync_mu_unlock(&mock->seeded_mu);
}

size_t nostr_mock_relay_get_seeded_count(NostrMockRelay *mock) {
    if (!mock) return 0;

    nsync_mu_lock(&mock->seeded_mu);
    size_t count = mock->seeded_count;
    nsync_mu_unlock(&mock->seeded_mu);

    return count;
}

/* === Publication Capture === */

const NostrEvent **nostr_mock_relay_get_published(NostrMockRelay *mock, size_t *count) {
    if (!mock || !count) return NULL;

    nsync_mu_lock(&mock->published_mu);
    *count = mock->published_count;

    if (mock->published_count == 0) {
        nsync_mu_unlock(&mock->published_mu);
        return NULL;
    }

    /* Build array of event pointers */
    const NostrEvent **events = malloc(mock->published_count * sizeof(NostrEvent *));
    if (!events) {
        nsync_mu_unlock(&mock->published_mu);
        *count = 0;
        return NULL;
    }

    size_t i = 0;
    for (EventNode *node = mock->published_events; node; node = node->next) {
        events[i++] = node->event;
    }

    nsync_mu_unlock(&mock->published_mu);
    return events;
}

const NostrEvent *nostr_mock_relay_await_publish(NostrMockRelay *mock, int timeout_ms) {
    if (!mock) return NULL;

    /* Check if we already have published events */
    nsync_mu_lock(&mock->published_mu);
    if (mock->published_events) {
        const NostrEvent *event = mock->published_events->event;
        nsync_mu_unlock(&mock->published_mu);
        return event;
    }
    nsync_mu_unlock(&mock->published_mu);

    /* Wait for notification */
    if (timeout_ms == 0) {
        return NULL;
    }

    void *data = NULL;
    if (timeout_ms < 0) {
        /* Indefinite wait */
        if (go_channel_receive(mock->publish_notify, &data) == 0) {
            nsync_mu_lock(&mock->published_mu);
            const NostrEvent *event = mock->published_events ? mock->published_events->event : NULL;
            nsync_mu_unlock(&mock->published_mu);
            return event;
        }
    } else {
        /* Wait with timeout using select */
        GoSelectCase cases[1] = {
            { .op = GO_SELECT_RECEIVE, .chan = mock->publish_notify, .recv_buf = &data }
        };
        GoSelectResult result = go_select_timeout(cases, 1, timeout_ms);
        if (result.selected_case == 0 && result.ok) {
            nsync_mu_lock(&mock->published_mu);
            const NostrEvent *event = mock->published_events ? mock->published_events->event : NULL;
            nsync_mu_unlock(&mock->published_mu);
            return event;
        }
    }

    return NULL;
}

void nostr_mock_relay_clear_published(NostrMockRelay *mock) {
    if (!mock) return;

    nsync_mu_lock(&mock->published_mu);
    free_event_list(mock->published_events);
    mock->published_events = NULL;
    mock->published_count = 0;
    nsync_mu_unlock(&mock->published_mu);
}

size_t nostr_mock_relay_get_published_count(NostrMockRelay *mock) {
    if (!mock) return 0;

    nsync_mu_lock(&mock->published_mu);
    size_t count = mock->published_count;
    nsync_mu_unlock(&mock->published_mu);

    return count;
}

/* === Response Injection === */

int nostr_mock_relay_inject_notice(NostrMockRelay *mock, const char *message) {
    if (!mock || !message) return -1;

    char *json = NULL;
    if (asprintf(&json, "[\"NOTICE\",\"%s\"]", message) < 0) {
        return -1;
    }

    int result = send_response(mock, json);
    free(json);
    return result;
}

int nostr_mock_relay_inject_ok(NostrMockRelay *mock, const char *event_id, bool ok, const char *reason) {
    if (!mock || !event_id) return -1;

    char *json = NULL;
    if (reason) {
        if (asprintf(&json, "[\"OK\",\"%s\",%s,\"%s\"]", event_id, ok ? "true" : "false", reason) < 0) {
            return -1;
        }
    } else {
        if (asprintf(&json, "[\"OK\",\"%s\",%s,\"\"]", event_id, ok ? "true" : "false") < 0) {
            return -1;
        }
    }

    int result = send_response(mock, json);
    free(json);
    return result;
}

int nostr_mock_relay_inject_closed(NostrMockRelay *mock, const char *sub_id, const char *reason) {
    if (!mock || !sub_id || !reason) return -1;

    char *json = NULL;
    if (asprintf(&json, "[\"CLOSED\",\"%s\",\"%s\"]", sub_id, reason) < 0) {
        return -1;
    }

    int result = send_response(mock, json);
    free(json);
    return result;
}

int nostr_mock_relay_inject_auth(NostrMockRelay *mock, const char *challenge) {
    if (!mock || !challenge) return -1;

    char *json = NULL;
    if (asprintf(&json, "[\"AUTH\",\"%s\"]", challenge) < 0) {
        return -1;
    }

    int result = send_response(mock, json);
    free(json);
    return result;
}

int nostr_mock_relay_inject_eose(NostrMockRelay *mock, const char *sub_id) {
    if (!mock || !sub_id) return -1;

    char *json = NULL;
    if (asprintf(&json, "[\"EOSE\",\"%s\"]", sub_id) < 0) {
        return -1;
    }

    int result = send_response(mock, json);
    free(json);
    return result;
}

int nostr_mock_relay_inject_event(NostrMockRelay *mock, const char *sub_id, NostrEvent *event) {
    if (!mock || !sub_id || !event) return -1;

    char *event_json = nostr_event_serialize_compact(event);
    if (!event_json) return -1;

    char *json = NULL;
    if (asprintf(&json, "[\"EVENT\",\"%s\",%s]", sub_id, event_json) < 0) {
        free(event_json);
        return -1;
    }

    free(event_json);
    int result = send_response(mock, json);
    free(json);
    return result;
}

/* === Fault Injection === */

void nostr_mock_relay_set_fault(NostrMockRelay *mock, NostrMockFaultType fault, int after_n) {
    if (!mock) return;

    nsync_mu_lock(&mock->fault_mu);
    mock->fault_type = fault;
    mock->fault_after_n = after_n;
    mock->operation_count = 0;
    nsync_mu_unlock(&mock->fault_mu);
}

void nostr_mock_relay_clear_fault(NostrMockRelay *mock) {
    if (!mock) return;

    nsync_mu_lock(&mock->fault_mu);
    mock->fault_type = MOCK_FAULT_NONE;
    mock->fault_after_n = 0;
    mock->operation_count = 0;
    nsync_mu_unlock(&mock->fault_mu);
}

NostrMockFaultType nostr_mock_relay_get_fault(NostrMockRelay *mock) {
    if (!mock) return MOCK_FAULT_NONE;

    nsync_mu_lock(&mock->fault_mu);
    NostrMockFaultType fault = mock->fault_type;
    nsync_mu_unlock(&mock->fault_mu);

    return fault;
}

/* === Statistics === */

void nostr_mock_relay_get_stats(NostrMockRelay *mock, NostrMockRelayStats *stats) {
    if (!mock || !stats) return;

    nsync_mu_lock(&mock->stats_mu);
    *stats = mock->stats;
    nsync_mu_unlock(&mock->stats_mu);
}

void nostr_mock_relay_reset_stats(NostrMockRelay *mock) {
    if (!mock) return;

    nsync_mu_lock(&mock->stats_mu);
    memset(&mock->stats, 0, sizeof(mock->stats));
    nsync_mu_unlock(&mock->stats_mu);
}

/* === Subscription Tracking === */

size_t nostr_mock_relay_get_subscription_count(NostrMockRelay *mock) {
    if (!mock) return 0;

    nsync_mu_lock(&mock->subs_mu);
    size_t count = 0;
    for (MockSubscription *s = mock->subscriptions; s; s = s->next) {
        count++;
    }
    nsync_mu_unlock(&mock->subs_mu);

    return count;
}

bool nostr_mock_relay_has_subscription(NostrMockRelay *mock, const char *sub_id) {
    if (!mock || !sub_id) return false;

    nsync_mu_lock(&mock->subs_mu);
    bool found = false;
    for (MockSubscription *s = mock->subscriptions; s; s = s->next) {
        if (strcmp(s->sub_id, sub_id) == 0) {
            found = true;
            break;
        }
    }
    nsync_mu_unlock(&mock->subs_mu);

    return found;
}

/* === Internal Implementation === */

static void free_event_list(EventNode *head) {
    while (head) {
        EventNode *next = head->next;
        if (head->event) {
            nostr_event_free(head->event);
        }
        free(head);
        head = next;
    }
}

static void free_subscription_list(MockSubscription *head) {
    while (head) {
        MockSubscription *next = head->next;
        free(head->sub_id);
        if (head->filters) {
            nostr_filters_free(head->filters);
        }
        free(head);
        head = next;
    }
}

static int send_response(NostrMockRelay *mock, const char *json) {
    if (!mock || !mock->recv_channel || !json) return -1;

    /* Check for response delay */
    if (mock->config.response_delay_ms > 0) {
        usleep(mock->config.response_delay_ms * 1000);
    }

    /* Create a copy of the message for the channel */
    char *msg_copy = strdup(json);
    if (!msg_copy) return -1;

    if (go_channel_try_send(mock->recv_channel, msg_copy) != 0) {
        free(msg_copy);
        return -1;
    }

    return 0;
}

static bool check_fault(NostrMockRelay *mock) {
    if (!mock) return false;

    nsync_mu_lock(&mock->fault_mu);
    if (mock->fault_type == MOCK_FAULT_NONE) {
        nsync_mu_unlock(&mock->fault_mu);
        return false;
    }

    mock->operation_count++;
    bool trigger = (mock->fault_after_n == 0) || (mock->operation_count > mock->fault_after_n);

    if (trigger) {
        nsync_mu_lock(&mock->stats_mu);
        mock->stats.faults_triggered++;
        nsync_mu_unlock(&mock->stats_mu);
    }

    nsync_mu_unlock(&mock->fault_mu);
    return trigger;
}

static void handle_req(NostrMockRelay *mock, const char *sub_id, NostrFilters *filters) {
    if (!mock || !sub_id) return;

    /* Update stats */
    nsync_mu_lock(&mock->stats_mu);
    mock->stats.subscriptions_received++;
    nsync_mu_unlock(&mock->stats_mu);

    /* Check for fault injection */
    if (check_fault(mock)) {
        switch (mock->fault_type) {
            case MOCK_FAULT_DISCONNECT:
                /* Close channels to simulate disconnect */
                if (mock->recv_channel) {
                    go_channel_close(mock->recv_channel);
                }
                return;
            case MOCK_FAULT_TIMEOUT:
                /* Don't respond */
                return;
            case MOCK_FAULT_INVALID_JSON:
                send_response(mock, "{invalid json}}}");
                return;
            case MOCK_FAULT_RATE_LIMIT:
                nostr_mock_relay_inject_closed(mock, sub_id, "rate-limited:");
                return;
            case MOCK_FAULT_AUTH_REQUIRED:
                if (mock->config.auth_challenge) {
                    nostr_mock_relay_inject_auth(mock, mock->config.auth_challenge);
                } else {
                    nostr_mock_relay_inject_auth(mock, "challenge-string");
                }
                return;
            default:
                break;
        }
    }

    /* Store subscription */
    MockSubscription *sub = calloc(1, sizeof(MockSubscription));
    if (sub) {
        sub->sub_id = strdup(sub_id);
        /* Note: we don't copy filters here as they may already be freed */

        nsync_mu_lock(&mock->subs_mu);
        sub->next = mock->subscriptions;
        mock->subscriptions = sub;
        nsync_mu_unlock(&mock->subs_mu);
    }

    /* Match seeded events against filters */
    nsync_mu_lock(&mock->seeded_mu);
    int events_sent = 0;
    for (EventNode *node = mock->seeded_events; node; node = node->next) {
        /* Check limit */
        if (mock->config.max_events_per_req >= 0 && events_sent >= mock->config.max_events_per_req) {
            break;
        }

        /* Match against filters */
        bool matches = false;
        if (filters) {
            matches = nostr_filters_match(filters, node->event);
        } else {
            /* No filters = match all */
            matches = true;
        }

        if (matches) {
            nostr_mock_relay_inject_event(mock, sub_id, node->event);
            events_sent++;

            nsync_mu_lock(&mock->stats_mu);
            mock->stats.events_matched++;
            nsync_mu_unlock(&mock->stats_mu);
        }
    }
    nsync_mu_unlock(&mock->seeded_mu);

    /* Send EOSE if configured */
    if (mock->config.auto_eose) {
        nostr_mock_relay_inject_eose(mock, sub_id);
    }
}

static void handle_event(NostrMockRelay *mock, NostrEvent *event) {
    if (!mock || !event) return;

    /* Update stats */
    nsync_mu_lock(&mock->stats_mu);
    mock->stats.events_published++;
    nsync_mu_unlock(&mock->stats_mu);

    /* Validate signature if configured */
    if (mock->config.validate_signatures) {
        if (!nostr_event_check_signature(event)) {
            if (event->id) {
                nostr_mock_relay_inject_ok(mock, event->id, false, "invalid: signature verification failed");
            }
            return;
        }
    }

    /* Check for fault injection */
    if (check_fault(mock)) {
        switch (mock->fault_type) {
            case MOCK_FAULT_DISCONNECT:
                if (mock->recv_channel) {
                    go_channel_close(mock->recv_channel);
                }
                return;
            case MOCK_FAULT_TIMEOUT:
                return;
            case MOCK_FAULT_INVALID_JSON:
                send_response(mock, "{invalid json}}}");
                return;
            case MOCK_FAULT_RATE_LIMIT:
                if (event->id) {
                    nostr_mock_relay_inject_ok(mock, event->id, false, "rate-limited:");
                }
                return;
            default:
                break;
        }
    }

    /* Capture the published event */
    EventNode *node = calloc(1, sizeof(EventNode));
    if (node) {
        node->event = nostr_event_copy(event);
        if (node->event) {
            nsync_mu_lock(&mock->published_mu);
            node->next = mock->published_events;
            mock->published_events = node;
            mock->published_count++;
            nsync_mu_unlock(&mock->published_mu);

            /* Notify waiting callers */
            int notify = 1;
            go_channel_try_send(mock->publish_notify, &notify);
        } else {
            free(node);
        }
    }

    /* Send OK response */
    if (event->id) {
        nostr_mock_relay_inject_ok(mock, event->id, true, NULL);
    }
}

static void handle_close(NostrMockRelay *mock, const char *sub_id) {
    if (!mock || !sub_id) return;

    /* Update stats */
    nsync_mu_lock(&mock->stats_mu);
    mock->stats.close_received++;
    nsync_mu_unlock(&mock->stats_mu);

    /* Remove subscription */
    nsync_mu_lock(&mock->subs_mu);
    MockSubscription **pp = &mock->subscriptions;
    while (*pp) {
        if (strcmp((*pp)->sub_id, sub_id) == 0) {
            MockSubscription *to_free = *pp;
            *pp = (*pp)->next;
            free(to_free->sub_id);
            if (to_free->filters) {
                nostr_filters_free(to_free->filters);
            }
            free(to_free);
            break;
        }
        pp = &(*pp)->next;
    }
    nsync_mu_unlock(&mock->subs_mu);
}

static void *mock_relay_loop(void *arg) {
    NostrMockRelay *mock = (NostrMockRelay *)arg;
    if (!mock || !mock->send_channel) {
        mock->running = 0;
        return NULL;
    }

    while (!mock->shutdown) {
        char *msg = NULL;

        /* Use select with timeout to allow checking shutdown flag */
        GoSelectCase cases[] = {
            { .op = GO_SELECT_RECEIVE, .chan = mock->send_channel, .recv_buf = (void**)&msg },
        };

        GoSelectResult result = go_select_timeout(cases, 1, 100);  /* 100ms timeout */

        if (result.selected_case < 0) {
            /* Timeout - check shutdown and continue */
            continue;
        }

        if (!result.ok || !msg) {
            /* Channel closed or error */
            break;
        }

        /* Parse the envelope */
        NostrEnvelope *env = nostr_envelope_parse(msg);
        if (!env) {
            free(msg);
            continue;
        }

        switch (env->type) {
            case NOSTR_ENVELOPE_REQ: {
                NostrReqEnvelope *req = (NostrReqEnvelope *)env;
                handle_req(mock, req->subscription_id, req->filters);
                break;
            }
            case NOSTR_ENVELOPE_EVENT: {
                NostrEventEnvelope *event_env = (NostrEventEnvelope *)env;
                handle_event(mock, event_env->event);
                break;
            }
            case NOSTR_ENVELOPE_CLOSE: {
                NostrCloseEnvelope *close_env = (NostrCloseEnvelope *)env;
                handle_close(mock, close_env->message);
                break;
            }
            case NOSTR_ENVELOPE_AUTH: {
                /* Handle AUTH response from client - for now just log */
                break;
            }
            default:
                break;
        }

        nostr_envelope_free(env);
        free(msg);
    }

    mock->running = 0;
    return NULL;
}
