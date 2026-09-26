/* debian/porthome-featuretest.h — Phase 5 packaging force-include.
 * Some upstream porthome + libgo files use PATH_MAX / CLOCK_MONOTONIC /
 * CLOCK_REALTIME without including <limits.h> / <time.h> and without a
 * feature-test macro; the debian hardening set (D_FORTIFY_SOURCE=3 +
 * -std=c11) hides them without the macro. Force _GNU_SOURCE + include
 * the two headers here.
 *
 * Guarded so files that #define _GNU_SOURCE themselves do not trip
 * -Werror -Wcpp. Empty replacement so a bare `#define _GNU_SOURCE` is
 * treated as identical.
 *
 * Follow-up (nostrc-h10m.3): add the missing includes + feature-test
 * macros upstream and drop this workaround. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <limits.h>
#include <time.h>
/* Wave 4 packaging (#22a): gnome/nostr-dav/src/nd-ical.c declares its own
 * bare `#define _DEFAULT_SOURCE` (empty) which then collides with the `1`
 * value features.h derived from _GNU_SOURCE above.  <features.h>'s work
 * is finished by the time <limits.h> / <time.h> return (all __USE_* bits
 * are already resolved), so an #undef here is safe — subsequent
 * translation-unit #defines can set _DEFAULT_SOURCE to whatever value
 * they want without a -Werror macro-redefinition warning.
 */
#undef _DEFAULT_SOURCE
