/*
 * notify_unread — unread encrypted-DM count on org.nostr.NotifyDaemon1
 * (/org/nostr/NotifyDaemon1; gnome/dbus/org.nostr.NotifyDaemon1.xml,
 * nostrc-prqu.18). Count and time only: DMs stay opaque.
 *
 * Main thread only: the notification dispatch, the open-in-gnostr action
 * and the org.gnostr.Client watcher all run there.
 */
#ifndef NOSTR_NOTIFY_UNREAD_H
#define NOSTR_NOTIFY_UNREAD_H

#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

/* Export on @bus (the GApplication's connection). Idempotent. */
bool     nostr_notify_unread_export(GDBusConnection *bus, GError **error);
void     nostr_notify_unread_unexport(void);

/* A DM notification was delivered at @when (unix seconds). */
void     nostr_notify_unread_add(int64_t when);
/* The user read them (activation, gnostr in front, MarkRead). */
void     nostr_notify_unread_reset(void);

uint32_t nostr_notify_unread_count(void);
int64_t  nostr_notify_unread_last(void);

/* TRUE when @nostr_uri ("nostr:nevent1…") points at a kind-1059 gift wrap,
 * i.e. its notification was a direct message. */
bool     nostr_notify_uri_is_dm(const char *nostr_uri);

#endif /* NOSTR_NOTIFY_UNREAD_H */
