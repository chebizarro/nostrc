/* nostr-publish-macros.h - Export macro for libnostr-publish
 *
 * SPDX-License-Identifier: MIT
 *
 * The library is built with -fvisibility=hidden; only declarations
 * tagged NOSTR_PUBLISH_API are part of the libnostr-publish.so.0 ABI.
 */
#ifndef NOSTR_PUBLISH_MACROS_H
#define NOSTR_PUBLISH_MACROS_H

#if defined(NOSTR_PUBLISH_COMPILATION) && defined(__GNUC__)
#  define NOSTR_PUBLISH_API __attribute__((visibility("default")))
#else
#  define NOSTR_PUBLISH_API
#endif

#endif /* NOSTR_PUBLISH_MACROS_H */
