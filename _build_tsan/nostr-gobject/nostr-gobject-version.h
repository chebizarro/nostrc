#pragma once

/**
 * SECTION:nostr-gobject-version
 * @short_description: nostr-gobject version checking
 *
 * nostr-gobject provides macros to check the version of the library
 * at compile-time.
 */

/**
 * NOSTR_GOBJECT_MAJOR_VERSION:
 *
 * nostr-gobject major version component (e.g. 1 if the version is 1.2.3)
 */
#define NOSTR_GOBJECT_MAJOR_VERSION (1)

/**
 * NOSTR_GOBJECT_MINOR_VERSION:
 *
 * nostr-gobject minor version component (e.g. 2 if the version is 1.2.3)
 */
#define NOSTR_GOBJECT_MINOR_VERSION (0)

/**
 * NOSTR_GOBJECT_MICRO_VERSION:
 *
 * nostr-gobject micro version component (e.g. 3 if the version is 1.2.3)
 */
#define NOSTR_GOBJECT_MICRO_VERSION (0)

/**
 * NOSTR_GOBJECT_VERSION:
 *
 * nostr-gobject version as a string (e.g. "1.2.3")
 */
#define NOSTR_GOBJECT_VERSION "1.0.0"

/**
 * NOSTR_GOBJECT_CHECK_VERSION:
 * @major: required major version
 * @minor: required minor version
 * @micro: required micro version
 *
 * Compile-time version checking. Evaluates to %TRUE if the version
 * of nostr-gobject is greater than or equal to the required one.
 */
#define NOSTR_GOBJECT_CHECK_VERSION(major,minor,micro) \
  (NOSTR_GOBJECT_MAJOR_VERSION > (major) || \
   (NOSTR_GOBJECT_MAJOR_VERSION == (major) && NOSTR_GOBJECT_MINOR_VERSION > (minor)) || \
   (NOSTR_GOBJECT_MAJOR_VERSION == (major) && NOSTR_GOBJECT_MINOR_VERSION == (minor) && \
    NOSTR_GOBJECT_MICRO_VERSION >= (micro)))
