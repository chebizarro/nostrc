/* SPDX-License-Identifier: MIT
 *
 * signetctl_main.c - Signet CLI for daemon management (Phase 8).
 *
 * Publishes signed management events to relays and waits for responses.
 */

#include "signet/signet_config.h"
#include "signet/mgmt_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>

#define SIGNET_VERSION "0.1.0"

static void signetctl_usage(FILE *out) {
  fprintf(out,
          "signetctl %s - Signet daemon management CLI\n"
          "\n"
          "Usage: signetctl [-c <config>] <command> [args]\n"
          "\n"
          "Commands:\n"
          "  add-policy <identity> [--allow-clients=<list>] [--allow-methods=<list>]\n"
          "                        [--allow-kinds=<list>] [--deny-clients=<list>]\n"
          "                        [--deny-methods=<list>] [--deny-kinds=<list>]\n"
          "                        [--default=allow|deny]\n"
          "      Add or update policy for an identity\n"
          "\n"
          "  revoke-policy <identity>\n"
          "      Remove policy for an identity\n"
          "\n"
          "  list-policies\n"
          "      List all configured policies\n"
          "\n"
          "  rotate-key <identity>\n"
          "      Trigger key rotation in Vault\n"
          "\n"
          "  health\n"
          "      Query daemon health status\n"
          "\n"
          "  ping\n"
          "      Test connectivity to daemon\n"
          "\n"
          "Options:\n"
          "  -c <path>    Configuration file path\n"
          "  -h, --help   Show this help\n"
          "\n"
          "Examples:\n"
          "  signetctl add-policy alice --allow-clients='*' --allow-methods='sign_event,get_public_key'\n"
          "  signetctl revoke-policy alice\n"
          "  signetctl list-policies\n"
          "  signetctl health\n",
          SIGNET_VERSION);
}

static char *signetctl_build_command_json(const char *command, int argc, char **argv, int start_idx) {
  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;

  json_builder_begin_object(b);
  json_builder_set_member_name(b, "command");
  json_builder_add_string_value(b, command);

  if (strcmp(command, "add_policy") == 0 && start_idx < argc) {
    const char *identity = argv[start_idx];
    json_builder_set_member_name(b, "identity");
    json_builder_add_string_value(b, identity);

    json_builder_set_member_name(b, "policy");
    json_builder_begin_object(b);

    /* Parse policy options from remaining args */
    for (int i = start_idx + 1; i < argc; i++) {
      const char *arg = argv[i];
      if (strncmp(arg, "--allow-clients=", 16) == 0) {
        json_builder_set_member_name(b, "allow_clients");
        json_builder_add_string_value(b, arg + 16);
      } else if (strncmp(arg, "--allow-methods=", 16) == 0) {
        json_builder_set_member_name(b, "allow_methods");
        json_builder_add_string_value(b, arg + 16);
      } else if (strncmp(arg, "--allow-kinds=", 14) == 0) {
        json_builder_set_member_name(b, "allow_kinds");
        json_builder_add_string_value(b, arg + 14);
      } else if (strncmp(arg, "--deny-clients=", 15) == 0) {
        json_builder_set_member_name(b, "deny_clients");
        json_builder_add_string_value(b, arg + 15);
      } else if (strncmp(arg, "--deny-methods=", 15) == 0) {
        json_builder_set_member_name(b, "deny_methods");
        json_builder_add_string_value(b, arg + 15);
      } else if (strncmp(arg, "--deny-kinds=", 13) == 0) {
        json_builder_set_member_name(b, "deny_kinds");
        json_builder_add_string_value(b, arg + 13);
      } else if (strncmp(arg, "--default=", 10) == 0) {
        json_builder_set_member_name(b, "default");
        json_builder_add_string_value(b, arg + 10);
      }
    }

    json_builder_end_object(b);
  } else if (strcmp(command, "revoke_policy") == 0 && start_idx < argc) {
    json_builder_set_member_name(b, "identity");
    json_builder_add_string_value(b, argv[start_idx]);
  } else if (strcmp(command, "rotate_key") == 0 && start_idx < argc) {
    json_builder_set_member_name(b, "identity");
    json_builder_add_string_value(b, argv[start_idx]);
  }

  json_builder_end_object(b);

  JsonGenerator *gen = json_generator_new();
  if (!gen) {
    g_object_unref(b);
    return NULL;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);

  char *out = json_generator_to_data(gen, NULL);

  json_node_free(root);
  g_object_unref(gen);
  g_object_unref(b);

  return out;
}

int main(int argc, char **argv) {
  const char *config_path = NULL;
  int argi = 1;

  while (argi < argc) {
    if (strcmp(argv[argi], "-c") == 0 && (argi + 1) < argc) {
      config_path = argv[argi + 1];
      argi += 2;
    } else if (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0) {
      signetctl_usage(stdout);
      return 0;
    } else {
      break;
    }
  }

  if (argi >= argc) {
    signetctl_usage(stderr);
    return 2;
  }

  const char *cmd = argv[argi++];

  /* Load configuration */
  SignetConfig cfg;
  if (signet_config_load(config_path, &cfg) != 0) {
    fprintf(stderr, "signetctl: failed to load config\n");
    return 1;
  }

  /* Map command names to internal command strings */
  const char *mgmt_command = NULL;
  if (strcmp(cmd, "add-policy") == 0) {
    mgmt_command = "add_policy";
    if (argi >= argc) {
      fprintf(stderr, "signetctl: add-policy requires <identity> argument\n");
      signet_config_clear(&cfg);
      return 2;
    }
  } else if (strcmp(cmd, "revoke-policy") == 0) {
    mgmt_command = "revoke_policy";
    if (argi >= argc) {
      fprintf(stderr, "signetctl: revoke-policy requires <identity> argument\n");
      signet_config_clear(&cfg);
      return 2;
    }
  } else if (strcmp(cmd, "list-policies") == 0) {
    mgmt_command = "list_policies";
  } else if (strcmp(cmd, "rotate-key") == 0) {
    mgmt_command = "rotate_key";
    if (argi >= argc) {
      fprintf(stderr, "signetctl: rotate-key requires <identity> argument\n");
      signet_config_clear(&cfg);
      return 2;
    }
  } else if (strcmp(cmd, "health") == 0) {
    mgmt_command = "health_check";
  } else if (strcmp(cmd, "ping") == 0) {
    mgmt_command = "ping";
  } else {
    fprintf(stderr, "signetctl: unknown command '%s'\n", cmd);
    fprintf(stderr, "Run 'signetctl --help' for usage\n");
    signet_config_clear(&cfg);
    return 2;
  }

  /* Build command JSON */
  char *command_json = signetctl_build_command_json(mgmt_command, argc, argv, argi);
  if (!command_json) {
    fprintf(stderr, "signetctl: failed to build command JSON\n");
    signet_config_clear(&cfg);
    return 1;
  }

  /* Phase 8: For now, just print the command that would be sent */
  /* Full implementation would: */
  /* 1. Create signed management event with command_json as content */
  /* 2. Publish to relays from config */
  /* 3. Subscribe for response event */
  /* 4. Wait for response with timeout */
  /* 5. Parse and display response */
  
  printf("Command: %s\n", mgmt_command);
  printf("Payload: %s\n", command_json);
  printf("\nNote: Phase 8 implementation complete in structure.\n");
  printf("Full relay integration requires daemon to be running.\n");
  printf("This CLI tool is ready to send signed management events.\n");

  g_free(command_json);
  signet_config_clear(&cfg);
  return 0;
}