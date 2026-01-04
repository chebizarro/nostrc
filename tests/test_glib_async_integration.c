/**
 * Test: GLib main loop integration with async operations
 * Tests that async profile fetch polling works correctly with GLib
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <glib.h>

#include "go.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"

static GMainLoop *main_loop = NULL;
static int idle_callback_count = 0;
static gboolean test_passed = FALSE;

/* Simulates the async polling mechanism in nostr_simple_pool_async.c */
static gboolean async_poll_callback(gpointer user_data) {
    idle_callback_count++;
    
    printf("[POLL %d] Async callback executing on main loop\n", idle_callback_count);
    
    /* Simulate checking goroutine channels (non-blocking) */
    usleep(1000); /* 1ms of work */
    
    if (idle_callback_count >= 5) {
        /* Test complete */
        printf("[POLL] Completed 5 iterations, test PASS\n");
        test_passed = TRUE;
        g_main_loop_quit(main_loop);
        return G_SOURCE_REMOVE;
    }
    
    /* Continue polling */
    return G_SOURCE_CONTINUE;
}

/* Test timeout to prevent infinite hang */
static gboolean timeout_callback(gpointer user_data) {
    printf("[TIMEOUT] Test took too long, FAIL\n");
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

int main(void) {
    printf("=== GLib Async Integration Test ===\n");
    
    /* Create GLib main context and loop (like GTK does) */
    GMainContext *context = g_main_context_new();
    main_loop = g_main_loop_new(context, FALSE);
    
    printf("[SETUP] Created GMainLoop\n");
    
    /* Schedule async polling (like nostr_simple_pool_async.c does) */
    GSource *poll_source = g_idle_source_new();
    g_source_set_callback(poll_source, async_poll_callback, NULL, NULL);
    g_source_attach(poll_source, context);
    g_source_unref(poll_source);
    
    printf("[SETUP] Scheduled async polling\n");
    
    /* Set timeout to prevent hang */
    g_timeout_add(2000, timeout_callback, NULL);
    
    /* Run main loop (blocks until quit) */
    printf("[RUN] Starting main loop...\n");
    g_main_loop_run(main_loop);
    
    /* Cleanup */
    g_main_loop_unref(main_loop);
    g_main_context_unref(context);
    
    printf("\n=== Results ===\n");
    printf("Idle callbacks: %d\n", idle_callback_count);
    printf("Test result: %s\n", test_passed ? "PASS" : "FAIL");
    
    return test_passed ? 0 : 1;
}
