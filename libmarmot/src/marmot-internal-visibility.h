#ifndef MARMOT_INTERNAL_VISIBILITY_H
#define MARMOT_INTERNAL_VISIBILITY_H

#include "marmot-export.h"

/* Unit and MDK-vector tests link against libmarmot's MLS internals. Keep
 * those entry points available only in test builds, not the shipped ABI. */
#ifdef MARMOT_TEST_HOOKS
# define MARMOT_TEST_API MARMOT_API
#else
# define MARMOT_TEST_API
#endif

#endif /* MARMOT_INTERNAL_VISIBILITY_H */
