/* nd-main.c - nostr-dav entry point
 *
 * SPDX-License-Identifier: MIT
 *
 * Localhost CalDAV/CardDAV/WebDAV bridge daemon that translates between
 * the DAV wire protocol and Nostr events. Started by the nostr-dav.service
 * systemd user unit (D-Bus activation delegates to it).
 *
 * Usage:
 *   nostr-dav                     run the service on http://127.0.0.1:7680/
 *   nostr-dav --show-credentials  print WebDAV URL, username, and token
 *
 * The listen address and port are fixed at compile time.
 */

#include "nd-application.h"

#include <glib.h>
#include <locale.h>

int
main(int argc, char *argv[])
{
  setlocale(LC_ALL, "");

  g_autoptr(NdApplication) app = nd_application_new();

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  if (status == 0)
    status = nd_application_get_exit_status(app);
  return status;
}
