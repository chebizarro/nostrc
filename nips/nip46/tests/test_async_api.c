/* nostrc-5wj9: Test async RPC API surface.
 * Validates the async interface compiles and handles NULL/error cases correctly.
 * Does NOT test actual relay communication (that requires integration tests). */

#include "nostr/nip46/nip46_client.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

static int callback_called = 0;
static char *last_error = NULL;

static void test_callback(NostrNip46Session *session,
                          const char *result_json,
                          const char *error_msg,
                          void *user_data) {
    (void)session;
    free(last_error);
    last_error = error_msg ? strdup(error_msg) : NULL;
    free((void *)result_json);
    free((void *)error_msg);
    callback_called = 1;
    if (user_data) {
        int *flag = (int *)user_data;
        *flag = 1;
    }
}

static void test_timeout_config(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    assert(s != NULL);

    /* Default timeout */
    uint32_t t = nostr_nip46_client_get_timeout(s);
    assert(t == NOSTR_NIP46_DEFAULT_TIMEOUT_MS);

    /* Set custom timeout */
    nostr_nip46_client_set_timeout(s, 5000);
    assert(nostr_nip46_client_get_timeout(s) == 5000);

    /* Reset to default */
    nostr_nip46_client_set_timeout(s, 0);
    assert(nostr_nip46_client_get_timeout(s) == NOSTR_NIP46_DEFAULT_TIMEOUT_MS);

    /* NULL session safety */
    nostr_nip46_client_set_timeout(NULL, 1000);
    assert(nostr_nip46_client_get_timeout(NULL) == NOSTR_NIP46_DEFAULT_TIMEOUT_MS);

    nostr_nip46_session_free(s);
    printf("PASS: test_timeout_config\n");
}

static void test_async_null_session(void) {
    int flag = 0;
    callback_called = 0;

    /* sign_event_async with NULL session should invoke callback with error */
    nostr_nip46_client_sign_event_async(NULL, "{}", test_callback, &flag);
    assert(callback_called == 1);
    assert(last_error != NULL);

    callback_called = 0;
    nostr_nip46_client_connect_rpc_async(NULL, NULL, NULL, test_callback, &flag);
    assert(callback_called == 1);

    callback_called = 0;
    nostr_nip46_client_get_public_key_rpc_async(NULL, test_callback, &flag);
    assert(callback_called == 1);

    printf("PASS: test_async_null_session\n");
}

static void test_async_null_event_json(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    assert(s != NULL);

    callback_called = 0;
    nostr_nip46_client_sign_event_async(s, NULL, test_callback, NULL);
    assert(callback_called == 1);
    assert(last_error != NULL);

    nostr_nip46_session_free(s);
    printf("PASS: test_async_null_event_json\n");
}

static void test_cancel_all_empty(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    assert(s != NULL);

    /* cancel_all on a session with no pending requests should be safe */
    nostr_nip46_client_cancel_all(s);
    nostr_nip46_client_cancel_all(NULL); /* NULL safety */

    nostr_nip46_session_free(s);
    printf("PASS: test_cancel_all_empty\n");
}

static void test_session_state_machine(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    assert(s != NULL);

    /* New session starts disconnected */
    NostrNip46State state = nostr_nip46_client_get_state_public(s);
    assert(state == NOSTR_NIP46_STATE_DISCONNECTED);

    /* NULL session returns disconnected */
    assert(nostr_nip46_client_get_state_public(NULL) == NOSTR_NIP46_STATE_DISCONNECTED);

    nostr_nip46_session_free(s);
    printf("PASS: test_session_state_machine\n");
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned completed;
} AsyncCompletion;

static void owned_failure(NostrNip46Session *session, const char *result,
                          const char *error, void *data) {
    (void)session;
    AsyncCompletion *completion = data;
    assert(result == NULL);
    assert(error != NULL);
    /* Both worker errors and shutdown/queue rejection errors are caller-owned. */
    free((void *)result);
    free((void *)error);
    pthread_mutex_lock(&completion->mutex);
    completion->completed++;
    pthread_cond_signal(&completion->cond);
    pthread_mutex_unlock(&completion->mutex);
}

static void test_async_owner_release(void) {
    AsyncCompletion completion = {
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0
    };
    NostrNip46Session *session = nostr_nip46_client_new();
    assert(session);
    enum { REQUESTS = 100 };
    for (int i = 0; i < REQUESTS; i++)
        nostr_nip46_client_sign_event_async(session, "{}", owned_failure, &completion);
    nostr_nip46_session_free(session);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 10;
    pthread_mutex_lock(&completion.mutex);
    while (completion.completed < REQUESTS)
        assert(pthread_cond_timedwait(&completion.cond, &completion.mutex, &deadline) == 0);
    assert(completion.completed == REQUESTS);
    pthread_mutex_unlock(&completion.mutex);
    pthread_cond_destroy(&completion.cond);
    pthread_mutex_destroy(&completion.mutex);
}

int main(void) {
    test_timeout_config();
    test_async_null_session();
    test_async_null_event_json();
    test_cancel_all_empty();
    test_session_state_machine();
    test_async_owner_release();
    free(last_error);
    printf("\nAll async API tests passed.\n");
    return 0;
}
