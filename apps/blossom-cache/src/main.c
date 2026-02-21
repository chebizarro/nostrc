/*
 * main.c - Entry point for blossom-cache
 *
 * SPDX-License-Identifier: MIT
 */

#include "bc-application.h"

int
main(int argc, char *argv[])
{
  g_autoptr(BcApplication) app = bc_application_new();
  return g_application_run(G_APPLICATION(app), argc, argv);
}
