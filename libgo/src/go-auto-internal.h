/*
 * go-auto-internal.h — Auto-cleanup registrations for all libgo types.
 *
 * INTERNAL ONLY — include this in .c implementation files, never in
 * public headers.  Provides go_autoptr / go_auto for all libgo types.
 *
 * Usage:
 *   #include "go-auto-internal.h"
 *
 *   void example(void) {
 *       go_autoptr(GoChannel) ch = go_channel_new(sizeof(int), 8);
 *       go_autoptr(Error) err = NULL;
 *       go_auto(GoWaitGroup) wg;
 *       go_wait_group_init(&wg);
 *       // all automatically cleaned up at scope exit
 *   }
 */
#ifndef GO_AUTO_INTERNAL_H
#define GO_AUTO_INTERNAL_H

/* Include the types we need — these pull in refptr.h → go_auto.h */
#include "channel.h"
#include "context.h"
#include "hash_map.h"
#include "error.h"
#include "ticker.h"
#include "wait_group.h"
#include "int_array.h"
#include "string_array.h"

/* ── Heap-allocated types (use go_autoptr) ──────────────────────────── */

/* GoChannel: ref-counted */
GO_DEFINE_AUTOPTR_CLEANUP_FUNC(GoChannel, go_channel_unref)

/* GoContext: ref-counted */
GO_DEFINE_AUTOPTR_CLEANUP_FUNC(GoContext, go_context_unref)

/* GoHashMap: use destroy */
GO_DEFINE_AUTOPTR_CLEANUP_FUNC(GoHashMap, go_hash_map_destroy)

/* Error: use free_error */
GO_DEFINE_AUTOPTR_CLEANUP_FUNC(Error, free_error)

/* Ticker: use stop_ticker */
GO_DEFINE_AUTOPTR_CLEANUP_FUNC(Ticker, stop_ticker)

/* StringArray: heap-allocated */
GO_DEFINE_AUTOPTR_CLEANUP_FUNC(StringArray, string_array_free)

/* ── Stack-allocated types (use go_auto) ────────────────────────────── */

/* GoWaitGroup: stack-allocated */
GO_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GoWaitGroup, go_wait_group_destroy)

/* IntArray: stack-allocated */
GO_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(IntArray, int_array_free)

/* GoRefPtr is already registered in refptr.h */

#endif /* GO_AUTO_INTERNAL_H */
