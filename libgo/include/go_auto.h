/*
 * go_auto.h — GLib-style auto memory management for libgo/libnostr.
 *
 * Provides __attribute__((cleanup)) macros for safe, scope-based resource
 * management in C11.  This is an INTERNAL header for implementation files
 * only — do NOT expose these macros in public API headers.
 *
 * Requires GCC ≥ 4.9 or Clang ≥ 3.7 for __attribute__((cleanup)).
 *
 * Naming convention follows GLib:
 *   go_auto(Type)            — stack value type with cleanup function
 *   go_autoptr(Type)         — heap pointer type with free/unref function
 *   go_autofree              — any malloc'd pointer, freed with free()
 *   go_autostr               — alias for go_autofree char *
 *   go_steal_pointer(pp)     — transfer ownership (set *pp to NULL, return old)
 *   go_clear_pointer(pp, fn) — NULL-safe clear: call fn(*pp), set *pp = NULL
 *
 * Type registration macros (place in headers AFTER the type definition):
 *   GO_DEFINE_AUTOPTR_CLEANUP_FUNC(Type, free_func)
 *   GO_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Type, clear_func)
 *   GO_DEFINE_AUTO_CLEANUP_FREE_FUNC(Type, free_func, none_value)
 */
#ifndef GO_AUTO_H
#define GO_AUTO_H

#include <stdlib.h>  /* free() */
#include <stddef.h>  /* NULL */

/* ── Compiler support check ─────────────────────────────────────────── */
#if !defined(__GNUC__) && !defined(__clang__)
  /* No cleanup attribute — macros degrade to plain declarations.
   * This avoids hard compilation errors on MSVC, but obviously
   * no auto-cleanup occurs. */
  #define _GO_CLEANUP(func)
  #warning "go_auto.h: __attribute__((cleanup)) not supported, auto-cleanup disabled"
#else
  #define _GO_CLEANUP(func) __attribute__((cleanup(func)))
#endif

/* ── go_autofree: auto-free any malloc'd pointer ────────────────────── */
static inline void _go_autofree_cleanup(void *pp) {
    void *p = *(void **)pp;
    if (p) free(p);
}

/**
 * go_autofree:
 *
 * Prefix for a local variable whose backing pointer is freed when it
 * goes out of scope.  Works for any malloc'd allocation.
 *
 * Example:
 *   go_autofree char *buf = malloc(128);
 *   go_autofree uint8_t *data = calloc(n, sizeof(uint8_t));
 */
#define go_autofree _GO_CLEANUP(_go_autofree_cleanup)

/* ── go_autostr: convenience alias for go_autofree char * ───────────── */
/* Backward-compatible with the old refptr.h definition.
 * Redefine using the new foundation.                                     */
#ifndef go_autostr
  static inline void _go_autostr_cleanup(char **sp) {
      if (*sp) { free(*sp); *sp = NULL; }
  }
  #define go_autostr _GO_CLEANUP(_go_autostr_cleanup) char *
#endif

/* ── go_steal_pointer: transfer ownership ───────────────────────────── */
/**
 * go_steal_pointer:
 * @pp: (inout): address of a pointer variable
 *
 * Reads the pointer at *pp, sets *pp to NULL, and returns the original
 * value.  Use this to transfer ownership out of an auto-cleanup scope
 * without triggering the cleanup function.
 *
 * Exactly equivalent to g_steal_pointer().
 *
 * Example:
 *   go_autofree char *s = strdup("hello");
 *   return go_steal_pointer(&s);   // caller owns the string now
 */
static inline void *
_go_steal_pointer(void *pp)
{
    void **p = (void **)pp;
    void *ref = *p;
    *p = NULL;
    return ref;
}
/* Type-safe macro version (mirrors g_steal_pointer) */
#define go_steal_pointer(pp) \
    ((__typeof__(*(pp))) _go_steal_pointer(pp))

/* ── go_clear_pointer: NULL-safe destroy-and-clear ──────────────────── */
/**
 * go_clear_pointer:
 * @pp: (inout): address of a pointer variable
 * @destroy: function to call on the pointer (e.g., free, nostr_event_free)
 *
 * If *pp is non-NULL, calls destroy(*pp) and sets *pp to NULL.
 * Safe to call multiple times.
 *
 * Exactly equivalent to g_clear_pointer().
 */
#define go_clear_pointer(pp, destroy) \
    do { \
        __typeof__(*(pp)) _cp_tmp = *(pp); \
        *(pp) = NULL; \
        if (_cp_tmp) (destroy)(_cp_tmp); \
    } while (0)

/* ── Type-specific auto-cleanup registration ────────────────────────── */

/**
 * GO_DEFINE_AUTOPTR_CLEANUP_FUNC:
 * @TypeName: the type (e.g., NostrEvent, GoChannel)
 * @func: the free/unref function (e.g., nostr_event_free, go_channel_unref)
 *
 * Defines the cleanup helper needed by go_autoptr(TypeName).
 * Place this in the header file AFTER the type's free function declaration.
 *
 * NOTE: This is for INTERNAL use only. Do NOT put go_autoptr in
 * public function signatures.
 *
 * Example:
 *   // In nostr-event-internal.h:
 *   GO_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)
 *
 *   // In implementation:
 *   go_autoptr(NostrEvent) event = nostr_event_new();
 *   // event is automatically freed at scope exit
 */
#define GO_DEFINE_AUTOPTR_CLEANUP_FUNC(TypeName, func) \
    static inline void _go_autoptr_cleanup_##TypeName(TypeName **pp) { \
        if (*pp) { (func)(*pp); *pp = NULL; } \
    }

/**
 * go_autoptr:
 * @TypeName: the type (must have GO_DEFINE_AUTOPTR_CLEANUP_FUNC defined)
 *
 * Declares a local pointer variable that is automatically freed when it
 * goes out of scope using the registered cleanup function.
 *
 * Example:
 *   go_autoptr(NostrEvent) event = nostr_event_new();
 *   if (error) return;  // event freed automatically
 */
#undef go_autoptr  /* Remove the old refptr.h definition */
#define go_autoptr(TypeName) \
    _GO_CLEANUP(_go_autoptr_cleanup_##TypeName) TypeName *

/**
 * GO_DEFINE_AUTO_CLEANUP_CLEAR_FUNC:
 * @TypeName: a stack-allocated value type (e.g., GoWaitGroup, IntArray)
 * @func: the clear/destroy function that takes TypeName* (e.g., int_array_free)
 *
 * Used with go_auto(TypeName) for stack-allocated value types.
 *
 * Example:
 *   GO_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(IntArray, int_array_free)
 *
 *   go_auto(IntArray) arr;
 *   int_array_init(&arr);
 *   // arr is automatically cleared at scope exit
 */
#define GO_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(TypeName, func) \
    static inline void _go_auto_cleanup_##TypeName(TypeName *p) { \
        (func)(p); \
    }

/**
 * go_auto:
 * @TypeName: the type (must have GO_DEFINE_AUTO_CLEANUP_CLEAR_FUNC defined)
 *
 * Declares a stack-allocated value that is automatically cleared when it
 * goes out of scope.
 */
#define go_auto(TypeName) \
    _GO_CLEANUP(_go_auto_cleanup_##TypeName) TypeName

/**
 * GO_DEFINE_AUTO_CLEANUP_FREE_FUNC:
 * @TypeName: the type (e.g., GoChannel)
 * @func: the free function
 * @none: the "no value" sentinel (usually NULL or 0)
 *
 * Like GO_DEFINE_AUTOPTR_CLEANUP_FUNC but for types where the variable
 * holds the value directly (not a pointer to it) and "none" may not be NULL.
 */
#define GO_DEFINE_AUTO_CLEANUP_FREE_FUNC(TypeName, func, none) \
    static inline void _go_auto_cleanup_free_##TypeName(TypeName *p) { \
        if (*p != (none)) (func)(*p); \
    }

/* ── Built-in cleanup definition for GoRefPtr ───────────────────────── */
/* GoRefPtr is defined in refptr.h (which includes this file), so we can
 * safely define its cleanup here since GoRefPtr is declared before us. */
/* Note: GoRefPtr cleanup is registered in refptr.h after the struct def. */

/* ── Cleanup definitions for other libgo types ──────────────────────── */
/* These are defined in go-auto-internal.h (internal to libgo .c files)
 * because the types are not yet available when go_auto.h is included
 * from refptr.h.  Include go-auto-internal.h in your .c files to get:
 *   go_autoptr(GoChannel), go_autoptr(GoContext), go_autoptr(GoHashMap),
 *   go_autoptr(Error), go_autoptr(Ticker), go_autoptr(StringArray),
 *   go_auto(GoWaitGroup), go_auto(IntArray)
 */

/* ── Backward compatibility ─────────────────────────────────────────── */

/* The old refptr.h cleanup_refptr macro is now superseded by the above.
 * Keep the CLEANUP(func) generic macro for one-off custom cleanups. */
#ifndef CLEANUP
  #define CLEANUP(func) _GO_CLEANUP(func)
#endif

#endif /* GO_AUTO_H */
