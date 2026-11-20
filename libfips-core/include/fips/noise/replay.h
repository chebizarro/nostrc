#ifndef FIPS_NOISE_REPLAY_H
#define FIPS_NOISE_REPLAY_H

#include <fips/host.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIPS_REPLAY_WINDOW_SIZE 8192u
/* Bit i records highest - i. Initial highest is 0 but counter 0 is unseen.
 * Fields are read-only; caller-owned, single-writer, no allocation or locks. */
typedef struct fips_replay_window {
    uint64_t highest;
    uint64_t bitmap[FIPS_REPLAY_WINDOW_SIZE / 64u];
} fips_replay_window;

FIPS_CORE_API void fips_replay_window_init(fips_replay_window *window);
/* Non-mutating. Rejects MAX, duplicates and age >= 8192, as upstream. */
FIPS_CORE_API bool fips_replay_window_check(const fips_replay_window *window, uint64_t counter);
/* Mark ONLY after authentication and acceptance. Rechecks the window; false
 * leaves it unchanged, including too-old/MAX inputs (no unchecked bitmap index). */
FIPS_CORE_API bool fips_replay_window_mark(fips_replay_window *window, uint64_t counter);
FIPS_CORE_API uint64_t fips_replay_window_highest(const fips_replay_window *window);
FIPS_CORE_API void fips_replay_window_reset(fips_replay_window *window);

#ifdef __cplusplus
}
#endif
#endif
