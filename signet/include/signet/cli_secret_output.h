/* SPDX-License-Identifier: MIT
 * Atomic owner-only output for CLI-delivered secret bytes.
 */

#ifndef SIGNET_CLI_SECRET_OUTPUT_H
#define SIGNET_CLI_SECRET_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

int signet_cli_write_secret_bytes(const char *path,
                                  const uint8_t *secret,
                                  size_t len);

#endif /* SIGNET_CLI_SECRET_OUTPUT_H */
