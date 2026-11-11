#ifndef GNOSTR_RELAY_WORKER_H
#define GNOSTR_RELAY_WORKER_H

#include <gio/gio.h>

/* Private relay runtime. Work is registered before starting, joined by the
 * collector, and drained at terminal shutdown before libnostr/OpenSSL cleanup.
 * finish runs on the collector AFTER the worker's thread-local destructors. */
void gnostr_relay_worker_run(GThreadFunc run, GDestroyNotify finish, gpointer data);
/* Synchronous compatibility callers own their threads, but terminal drainage
 * must still wait until their core operation has returned. */
gboolean gnostr_relay_worker_enter(void);
void gnostr_relay_worker_leave(void);
/* Tracks callback retirement as well as worker completion. Shutdown dispatches
 * captured contexts while callback owners are still alive. */
GTask *gnostr_relay_worker_task_new(GObject *source, GCancellable *cancel,
                                   GAsyncReadyCallback callback, gpointer data);
/* Takes a source reference; terminal drain destroys undelivered results. */
void gnostr_relay_worker_deliver(GSource *source);
gboolean gnostr_relay_worker_register(GObject *relay);
void gnostr_relay_worker_unregister(GObject *relay);

#endif
