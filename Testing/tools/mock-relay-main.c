/**
 * @file mock-relay-main.c
 * @brief CLI tool for running a standalone mock Nostr relay server
 *
 * Usage:
 *   ./mock-relay [options]
 *
 * Options:
 *   --port PORT      Port to listen on (default: 0 = auto-assign)
 *   --bind ADDR      Address to bind to (default: 127.0.0.1)
 *   --seed FILE      JSONL file to seed events from
 *   --name NAME      Relay name for NIP-11
 *   --desc DESC      Relay description for NIP-11
 *   --delay MS       Response delay in milliseconds (default: 0)
 *   --max-events N   Max events per REQ (default: -1 = unlimited)
 *   --validate-sig   Enable signature validation
 *   --no-eose        Disable automatic EOSE
 *   --help           Show this help
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include "nostr/testing/mock_relay_server.h"

static volatile int g_should_exit = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_should_exit = 1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Standalone mock Nostr relay server for integration testing.\n"
        "\n"
        "Options:\n"
        "  --port PORT      Port to listen on (default: 0 = auto-assign)\n"
        "  --bind ADDR      Address to bind to (default: 127.0.0.1)\n"
        "  --seed FILE      JSONL file to seed events from\n"
        "  --name NAME      Relay name for NIP-11 (default: MockRelay)\n"
        "  --desc DESC      Relay description for NIP-11\n"
        "  --delay MS       Response delay in milliseconds (default: 0)\n"
        "  --max-events N   Max events per REQ (default: -1 = unlimited)\n"
        "  --validate-sig   Enable signature validation\n"
        "  --no-eose        Disable automatic EOSE after subscriptions\n"
        "  --help           Show this help\n"
        "\n"
        "Examples:\n"
        "  # Start on random port, print URL\n"
        "  %s\n"
        "\n"
        "  # Start on specific port with seeded events\n"
        "  %s --port 7777 --seed fixtures/events.jsonl\n"
        "\n"
        "  # Start with artificial latency for testing timeouts\n"
        "  %s --port 7777 --delay 500\n"
        "\n"
        "Output:\n"
        "  On successful start, prints the WebSocket URL to stdout:\n"
        "    ws://127.0.0.1:7777\n"
        "\n"
        "  Statistics are printed periodically to stderr.\n"
        "\n",
        prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    NostrMockRelayServerConfig config = nostr_mock_server_config_default();

    /* Mutable copies for option values */
    char *seed_file = NULL;
    char *relay_name = NULL;
    char *relay_desc = NULL;
    char *bind_addr = NULL;

    static struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"bind", required_argument, 0, 'b'},
        {"seed", required_argument, 0, 's'},
        {"name", required_argument, 0, 'n'},
        {"desc", required_argument, 0, 'd'},
        {"delay", required_argument, 0, 'l'},
        {"max-events", required_argument, 0, 'm'},
        {"validate-sig", no_argument, 0, 'v'},
        {"no-eose", no_argument, 0, 'e'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "p:b:s:n:d:l:m:veh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'p':
                config.port = (uint16_t)atoi(optarg);
                break;
            case 'b':
                bind_addr = optarg;
                break;
            case 's':
                seed_file = optarg;
                break;
            case 'n':
                relay_name = optarg;
                break;
            case 'd':
                relay_desc = optarg;
                break;
            case 'l':
                config.response_delay_ms = atoi(optarg);
                break;
            case 'm':
                config.max_events_per_req = atoi(optarg);
                break;
            case 'v':
                config.validate_signatures = true;
                break;
            case 'e':
                config.auto_eose = false;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* Apply string options */
    config.bind_addr = bind_addr;
    config.seed_file = seed_file;
    config.relay_name = relay_name;
    config.relay_desc = relay_desc;

    /* Create and start server */
    NostrMockRelayServer *server = nostr_mock_server_new(&config);
    if (!server) {
        fprintf(stderr, "Error: Failed to create mock relay server\n");
        return 1;
    }

    if (nostr_mock_server_start(server) != 0) {
        fprintf(stderr, "Error: Failed to start mock relay server\n");
        nostr_mock_server_free(server);
        return 1;
    }

    /* Print URL to stdout for scripting */
    printf("%s\n", nostr_mock_server_get_url(server));
    fflush(stdout);

    /* Print startup info to stderr */
    fprintf(stderr, "Mock relay started:\n");
    fprintf(stderr, "  URL: %s\n", nostr_mock_server_get_url(server));
    fprintf(stderr, "  Port: %u\n", nostr_mock_server_get_port(server));
    if (seed_file) {
        fprintf(stderr, "  Seeded events: %zu\n", nostr_mock_server_get_seeded_count(server));
    }
    fprintf(stderr, "\nPress Ctrl+C to stop...\n\n");

    /* Set up signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Main loop - print stats periodically */
    while (!g_should_exit) {
        sleep(5);
        if (g_should_exit) break;

        NostrMockRelayStats stats;
        nostr_mock_server_get_stats(server, &stats);

        fprintf(stderr, "[stats] connections: %zu (total: %zu), "
                "subs: %zu, events matched: %zu, published: %zu\n",
                stats.connections_current, stats.connections_total,
                stats.subscriptions_received, stats.events_matched,
                stats.events_published);
    }

    fprintf(stderr, "\nShutting down...\n");

    /* Final stats */
    NostrMockRelayStats final_stats;
    nostr_mock_server_get_stats(server, &final_stats);
    fprintf(stderr, "\nFinal statistics:\n");
    fprintf(stderr, "  Total connections: %zu\n", final_stats.connections_total);
    fprintf(stderr, "  Subscriptions received: %zu\n", final_stats.subscriptions_received);
    fprintf(stderr, "  Events matched: %zu\n", final_stats.events_matched);
    fprintf(stderr, "  Events published: %zu\n", final_stats.events_published);
    fprintf(stderr, "  CLOSE received: %zu\n", final_stats.close_received);

    /* Cleanup */
    nostr_mock_server_free(server);

    fprintf(stderr, "Done.\n");
    return 0;
}
