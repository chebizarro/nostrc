#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "go.h"
#include "error.h"

/*
 * Test tool to verify relay EOSE behavior per NIP-01 spec.
 * 
 * According to NIP-01, relays MUST send EOSE for ALL subscriptions,
 * regardless of filter type or event kind.
 * 
 * Usage: ./test_relay_eose <relay_url> [test_type]
 *   test_type: kind0 (default), kind1, empty
 */

typedef struct {
    const char *relay_url;
    const char *test_name;
    int received_eose;
    int received_events;
    time_t start_time;
    time_t eose_time;
} TestResult;

static void print_result(TestResult *result) {
    printf("\n=== Test Results ===\n");
    printf("Relay: %s\n", result->relay_url);
    printf("Test: %s\n", result->test_name);
    printf("Events received: %d\n", result->received_events);
    
    if (result->received_eose) {
        double elapsed = difftime(result->eose_time, result->start_time);
        printf("EOSE: ✅ RECEIVED (after %.2f seconds)\n", elapsed);
        printf("Status: COMPLIANT with NIP-01\n");
    } else {
        printf("EOSE: ❌ NOT RECEIVED (timeout after 10 seconds)\n");
        printf("Status: VIOLATES NIP-01 spec\n");
    }
    printf("===================\n\n");
}

static void test_relay_eose(const char *relay_url, const char *test_name, NostrFilters *filters) {
    printf("\n🔍 Testing relay: %s\n", relay_url);
    printf("   Test type: %s\n", test_name);
    printf("   Waiting for EOSE (max 10 seconds)...\n\n");
    
    TestResult result = {
        .relay_url = relay_url,
        .test_name = test_name,
        .received_eose = 0,
        .received_events = 0,
        .start_time = time(NULL),
        .eose_time = 0
    };
    
    // Create relay and subscription
    GoContext *ctx = go_context_background();
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(ctx, relay_url, &err);
    if (!relay) {
        fprintf(stderr, "❌ Failed to create relay: %s\n", err ? err->message : "unknown");
        if (err) free_error(err);
        return;
    }
    
    // Connect
    printf("   Connecting to relay...\n");
    if (!nostr_relay_connect(relay, &err)) {
        fprintf(stderr, "❌ Failed to connect to relay: %s\n", err ? err->message : "unknown");
        if (err) free_error(err);
        nostr_relay_free(relay);
        return;
    }
    
    // Wait for connection
    sleep(2);
    
    // Create subscription
    NostrSubscription *sub = nostr_subscription_new(relay, filters);
    if (!sub) {
        fprintf(stderr, "❌ Failed to create subscription\n");
        nostr_relay_disconnect(relay);
        nostr_relay_free(relay);
        return;
    }
    
    // Fire subscription
    printf("   Subscription sent, waiting for response...\n");
    if (!nostr_subscription_fire(sub, &err)) {
        fprintf(stderr, "❌ Failed to fire subscription: %s\n", err ? err->message : "unknown");
        if (err) free_error(err);
        nostr_subscription_free(sub);
        nostr_relay_disconnect(relay);
        nostr_relay_free(relay);
        return;
    }
    
    // Poll for events and EOSE
    GoChannel *events_ch = nostr_subscription_get_events_channel(sub);
    GoChannel *eose_ch = nostr_subscription_get_eose_channel(sub);
    
    time_t timeout = time(NULL) + 10; // 10 second timeout
    
    while (time(NULL) < timeout && !result.received_eose) {
        // Check for events
        void *msg = NULL;
        if (go_channel_try_receive(events_ch, &msg) == 0 && msg) {
            result.received_events++;
            printf("   📨 Event received (total: %d)\n", result.received_events);
            nostr_event_free((NostrEvent*)msg);
        }
        
        // Check for EOSE
        if (go_channel_try_receive(eose_ch, NULL) == 0) {
            result.received_eose = 1;
            result.eose_time = time(NULL);
            printf("   ✅ EOSE received!\n");
            break;
        }
        
        usleep(50000); // 50ms
    }
    
    // Cleanup
    nostr_subscription_close(sub, NULL);
    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    nostr_relay_disconnect(relay);
    nostr_relay_free(relay);
    
    // Print results
    print_result(&result);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <relay_url> [test_type]\n", argv[0]);
        fprintf(stderr, "  test_type: kind0 (default), kind1, empty\n");
        fprintf(stderr, "\nExample:\n");
        fprintf(stderr, "  %s wss://relay.damus.io kind0\n", argv[0]);
        return 1;
    }
    
    const char *relay_url = argv[1];
    const char *test_type = (argc > 2) ? argv[2] : "kind0";
    
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║         Nostr Relay EOSE Compliance Test (NIP-01)         ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");
    
    // Test kind-0 (profile metadata)
    if (strcmp(test_type, "kind0") == 0) {
        NostrFilters *filters = nostr_filters_new();
        NostrFilter *f = nostr_filter_new();
        nostr_filter_add_kind(f, 0);
        nostr_filter_set_limit(f, 5);
        nostr_filters_add(filters, f);
        
        test_relay_eose(relay_url, "Kind 0 (Profile Metadata)", filters);
        nostr_filters_free(filters);
    }
    // Test kind-1 (text notes)
    else if (strcmp(test_type, "kind1") == 0) {
        NostrFilters *filters = nostr_filters_new();
        NostrFilter *f = nostr_filter_new();
        nostr_filter_add_kind(f, 1);
        nostr_filter_set_limit(f, 5);
        nostr_filters_add(filters, f);
        
        test_relay_eose(relay_url, "Kind 1 (Text Notes)", filters);
        nostr_filters_free(filters);
    }
    // Test empty filter (should still get EOSE)
    else if (strcmp(test_type, "empty") == 0) {
        NostrFilters *filters = nostr_filters_new();
        NostrFilter *f = nostr_filter_new();
        nostr_filter_set_limit(f, 5);
        nostr_filters_add(filters, f);
        
        test_relay_eose(relay_url, "Empty Filter (Any Kind)", filters);
        nostr_filters_free(filters);
    }
    else {
        fprintf(stderr, "Unknown test type: %s\n", test_type);
        return 1;
    }
    
    printf("💡 Tip: Test multiple relays to compare compliance:\n");
    printf("   %s wss://relay.damus.io kind0\n", argv[0]);
    printf("   %s wss://relay.primal.net kind0\n", argv[0]);
    printf("   %s wss://nos.lol kind0\n", argv[0]);
    printf("   %s wss://relay.sharegap.net kind0\n", argv[0]);
    
    return 0;
}
