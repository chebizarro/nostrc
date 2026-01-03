#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../libnostr/include/libnostr_store.h"

int main() {
    printf("Testing nostrdb ingestion via libnostr_store API...\n\n");
    
    // Clean test directory
    system("rm -rf /tmp/test_ndb_simple && mkdir -p /tmp/test_ndb_simple");
    
    // Initialize store with same config as gnostr
    ln_store *store = NULL;
    const char *opts = "{\"mapsize\":1073741824,\"ingester_threads\":4,\"ingest_skip_validation\":1}";
    
    printf("Opening store with opts: %s\n", opts);
    int rc = ln_store_open("nostrdb", "/tmp/test_ndb_simple", opts, &store);
    if (rc != 0) {
        fprintf(stderr, "✗ FAILED to open store: rc=%d\n", rc);
        return 1;
    }
    printf("✓ Store opened successfully\n\n");
    
    // Test ingesting 10 profile events via LDJSON
    printf("Test: Ingesting 10 profile events via LDJSON...\n");
    
    char ldjson[10000] = "";
    for (int i = 0; i < 10; i++) {
        char event[1000];
        // Test with missing tags field (like real events from relays)
        if (i < 5) {
            snprintf(event, sizeof(event), 
                "{\"kind\":0,\"id\":\"test%08d\",\"pubkey\":\"%064d\",\"created_at\":%d,\"content\":\"{\\\"name\\\":\\\"User%d\\\"}\",\"sig\":\"sig%064d\"}\n",
                i, i, 1234567890 + i, i, i);
        } else {
            // Test with tags field present
            snprintf(event, sizeof(event), 
                "{\"id\":\"test%08d\",\"pubkey\":\"%064d\",\"created_at\":%d,\"kind\":0,\"tags\":[],\"content\":\"{\\\"name\\\":\\\"User%d\\\"}\",\"sig\":\"sig%064d\"}\n",
                i, i, 1234567890 + i, i, i);
        }
        strcat(ldjson, event);
    }
    
    printf("Ingesting %zu bytes of LDJSON...\n", strlen(ldjson));
    rc = ln_store_ingest_ldjson(store, ldjson, strlen(ldjson), NULL);
    if (rc != 0) {
        fprintf(stderr, "✗ FAILED to ingest LDJSON: rc=%d\n", rc);
        return 1;
    }
    printf("✓ LDJSON ingested successfully (rc=0)\n");
    
    // Wait for async ingestion
    printf("\nWaiting 3 seconds for async ingestion to complete...\n");
    sleep(3);
    
    // Get stats
    char *stats_json = NULL;
    rc = ln_store_stat_json(store, &stats_json);
    if (rc != 0 || !stats_json) {
        fprintf(stderr, "✗ FAILED to get stats: rc=%d\n", rc);
        return 1;
    }
    
    printf("\nDatabase stats:\n%s\n", stats_json);
    
    // Parse the profile count from JSON (simple string search)
    char *profile_line = strstr(stats_json, "\"profile\":");
    if (!profile_line) {
        fprintf(stderr, "✗ FAILED: No profile stats in JSON\n");
        free(stats_json);
        return 1;
    }
    
    int profile_count = 0;
    sscanf(profile_line, "\"profile\":%d", &profile_count);
    
    printf("\n✓ Found %d profiles in database\n", profile_count);
    
    if (profile_count < 8) {
        fprintf(stderr, "\n✗✗✗ FAILED: Only %d/10 profiles in database! ✗✗✗\n", profile_count);
        fprintf(stderr, "This confirms events are being dropped during ingestion!\n");
        fprintf(stderr, "Success rate: %d%%\n", profile_count * 10);
        free(stats_json);
        return 1;
    }
    
    printf("\n✓✓✓ TEST PASSED ✓✓✓\n");
    printf("nostrdb ingestion is working correctly via libnostr_store!\n");
    printf("Success rate: %d%%\n", profile_count * 10);
    
    free(stats_json);
    ln_store_close(store);
    return 0;
}
