#include "nostr_relay_worker.h"
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-connection.h>
#include <stdlib.h>

/* GTask's shared pool is deliberately not used for transport work: its threads
 * need not exit before OpenSSL's atexit handler runs (nostrc-0cfsg.36.33). */
typedef struct {
    GThread *thread;
    GThreadFunc run;
    GDestroyNotify finish;
    gpointer data;
} RelayWork;

typedef struct {
    GWeakRef ref;
} RelayOwner;

typedef struct {
    GMainContext *context;
    GAsyncReadyCallback callback;
    gpointer data;
} TaskDelivery;

static GMutex lock;
static GCond changed;
static GQueue completed = G_QUEUE_INIT;
static GQueue deliveries = G_QUEUE_INIT;
static GHashTable *owners;
static GList *task_deliveries;
static GThread *collector;
static guint active;
static gboolean draining;
static gboolean stopped;
static gsize initialized;

static gpointer
collect_workers(gpointer unused)
{
    (void)unused;
    g_mutex_lock(&lock);
    for (;;) {
        while (g_queue_is_empty(&completed) && !stopped)
            g_cond_wait(&changed, &lock);
        if (stopped)
            break;
        RelayWork *work = g_queue_pop_head(&completed);
        g_mutex_unlock(&lock);
        g_thread_join(work->thread);
        if (work->finish)
            work->finish(work->data);
        g_free(work);
        g_mutex_lock(&lock);
        active--;
        g_cond_broadcast(&changed);
        g_main_context_wakeup(g_main_context_default());
    }
    g_mutex_unlock(&lock);
    return NULL;
}

static gpointer
run_worker(gpointer data)
{
    RelayWork *work = data;
    work->run(work->data);
    g_mutex_lock(&lock);
    g_queue_push_tail(&completed, work);
    g_cond_broadcast(&changed);
    g_mutex_unlock(&lock);
    return NULL;
}

static void
owner_free(gpointer data)
{
    RelayOwner *owner = data;
    g_weak_ref_clear(&owner->ref);
    g_free(owner);
}

static void
runtime_init(void)
{
    if (g_once_init_enter(&initialized)) {
        /* Registers OpenSSL first, then core transport shutdown. Our handler
         * therefore runs before both, including when the main loop has ended. */
        nostr_connection_runtime_init();
        owners = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, owner_free);
        collector = g_thread_new("relay-collector", collect_workers, NULL);
        if (atexit(gnostr_relay_workers_shutdown) != 0)
            g_error("Could not register relay worker shutdown");
        g_once_init_leave(&initialized, 1);
    }
}

static void
task_delivered(GObject *source, GAsyncResult *result, gpointer data)
{
    TaskDelivery *delivery = data;
    if (delivery->callback)
        delivery->callback(source, result, delivery->data);
    g_mutex_lock(&lock);
    task_deliveries = g_list_remove(task_deliveries, delivery);
    g_mutex_unlock(&lock);
    g_main_context_unref(delivery->context);
    g_free(delivery);
    g_main_context_wakeup(g_main_context_default());
}

GTask *
gnostr_relay_worker_task_new(GObject *source, GCancellable *cancel,
                              GAsyncReadyCallback callback, gpointer data)
{
    runtime_init();
    TaskDelivery *delivery = g_new0(TaskDelivery, 1);
    delivery->context = g_main_context_ref_thread_default();
    delivery->callback = callback;
    delivery->data = data;
    g_mutex_lock(&lock);
    task_deliveries = g_list_prepend(task_deliveries, delivery);
    g_mutex_unlock(&lock);
    return g_task_new(source, cancel, task_delivered, delivery);
}

gboolean
gnostr_relay_worker_register(GObject *relay)
{
    runtime_init();
    RelayOwner *owner = g_new0(RelayOwner, 1);
    g_weak_ref_init(&owner->ref, relay);
    g_mutex_lock(&lock);
    gboolean admitted = !draining;
    if (admitted)
        g_hash_table_insert(owners, relay, owner);
    g_mutex_unlock(&lock);
    if (!admitted) owner_free(owner);
    return admitted;
}

void
gnostr_relay_worker_unregister(GObject *relay)
{
    g_mutex_lock(&lock);
    g_hash_table_remove(owners, relay);
    g_mutex_unlock(&lock);
}

void
gnostr_relay_worker_deliver(GSource *source)
{
    g_mutex_lock(&lock);
    for (GList *l = deliveries.head, *next; l; l = next) {
        next = l->next;
        if (g_source_is_destroyed(l->data)) {
            g_source_unref(l->data);
            g_queue_delete_link(&deliveries, l);
        }
    }
    if (!draining) {
        g_queue_push_tail(&deliveries, g_source_ref(source));
        g_source_attach(source, g_main_context_default());
    } else {
        g_source_destroy(source);
    }
    g_mutex_unlock(&lock);
    g_source_unref(source);
}

gboolean
gnostr_relay_worker_enter(void)
{
    runtime_init();
    g_mutex_lock(&lock);
    gboolean admitted = !draining;
    if (admitted) active++;
    g_mutex_unlock(&lock);
    return admitted;
}

void
gnostr_relay_worker_leave(void)
{
    g_mutex_lock(&lock);
    active--;
    g_cond_broadcast(&changed);
    g_mutex_unlock(&lock);
    g_main_context_wakeup(g_main_context_default());
}

void
gnostr_relay_worker_run(GThreadFunc run, GDestroyNotify finish, gpointer data)
{
    runtime_init();
    RelayWork *work = g_new0(RelayWork, 1);
    work->run = run;
    work->finish = finish;
    work->data = data;
    g_mutex_lock(&lock);
    g_assert(!stopped); /* cleanup remains admissible while draining */
    active++;
    work->thread = g_thread_new("relay-worker", run_worker, work);
    g_mutex_unlock(&lock);
}

void
gnostr_relay_workers_shutdown(void)
{
    if (!initialized)
        return;
    g_autoptr(GPtrArray) snapshot = g_ptr_array_new_with_free_func(g_object_unref);
    g_mutex_lock(&lock);
    if (draining) {
        g_mutex_unlock(&lock);
        return;
    }
    draining = TRUE;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, owners);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        GObject *relay = g_weak_ref_get(&((RelayOwner *)value)->ref);
        if (relay)
            g_ptr_array_add(snapshot, relay);
    }
    g_mutex_unlock(&lock);
    for (guint i = 0; i < snapshot->len; i++)
        g_object_run_dispose(g_ptr_array_index(snapshot, i));
    g_ptr_array_set_size(snapshot, 0);

    /* A joined worker is not a delivered GTask. Keep its captured contexts
     * alive/dispatching until callbacks release their source/user-data refs.
     * Production calls this BEFORE freeing callback owners. No polling: worker
     * and callback retirement wake the default context's blocking iteration. */
    for (;;) {
        g_autoptr(GPtrArray) contexts = g_ptr_array_new_with_free_func((GDestroyNotify)g_main_context_unref);
        g_mutex_lock(&lock);
        gboolean done = active == 0 && task_deliveries == NULL;
        for (GList *l = task_deliveries; l; l = l->next)
            g_ptr_array_add(contexts, g_main_context_ref(((TaskDelivery *)l->data)->context));
        g_mutex_unlock(&lock);
        if (done) break;
        gboolean dispatched = FALSE;
        for (guint i = 0; i < contexts->len; i++)
            dispatched |= g_main_context_iteration(g_ptr_array_index(contexts, i), FALSE);
        if (!dispatched)
            g_main_context_iteration(g_main_context_default(), TRUE);
    }
    g_mutex_lock(&lock);
    stopped = TRUE;
    g_cond_broadcast(&changed);
    g_mutex_unlock(&lock);
    g_thread_join(collector);
    collector = NULL;
    while (!g_queue_is_empty(&deliveries)) {
        GSource *source = g_queue_pop_head(&deliveries);
        g_source_destroy(source);
        g_source_unref(source);
    }
    nostr_connection_shutdown();
}
