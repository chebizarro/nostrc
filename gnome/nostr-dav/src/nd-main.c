/* nd-main.c - nostr-dav entry point
 *
 * SPDX-License-Identifier: MIT
 *
 * Localhost CalDAV/CardDAV/WebDAV bridge daemon that translates between
 * the DAV wire protocol and Nostr events. Designed to be activated by
 * systemd --user or D-Bus.
 *
 * Usage:
 *   nostr-dav [--address=127.0.0.1] [--port=7680]
 *
 * Or via environment:
 *   NOSTR_DAV_ADDRESS=127.0.0.1 NOSTR_DAV_PORT=7680 nostr-dav
 */

#include "nd-application.h"

#include <glib.h>
#include <locale.h>

int
main(int argc, char *argv[])
{
  setlocale(LC_ALL, "");

  g_autoptr(NdApplication) app = nd_application_new();

  return g_application_run(G_APPLICATION(app), argc, argv);
}
