#include <fips/noise/replay.h>
#include <string.h>

void fips_replay_window_init(fips_replay_window *window)
{ memset(window, 0, sizeof(*window)); }

bool fips_replay_window_check(const fips_replay_window *window, uint64_t counter)
{
    if (counter == UINT64_MAX) return false;
    if (counter > window->highest) return true;
    uint64_t age = window->highest - counter;
    if (age >= FIPS_REPLAY_WINDOW_SIZE) return false;
    return (window->bitmap[age / 64u] & (UINT64_C(1) << (age % 64u))) == 0;
}

bool fips_replay_window_mark(fips_replay_window *window, uint64_t counter)
{
    if (!fips_replay_window_check(window, counter)) return false;
    if (counter > window->highest) {
        uint64_t shift = counter - window->highest;
        if (shift >= FIPS_REPLAY_WINDOW_SIZE) {
            memset(window->bitmap, 0, sizeof(window->bitmap));
        } else {
            size_t words = (size_t)shift / 64u;
            unsigned bits = (unsigned)(shift % 64u);
            /* High to low: neither word shifts nor cross-word carries overwrite
             * a source before it is read. No shift by 64 (undefined in C). */
            for (size_t i = FIPS_REPLAY_WINDOW_SIZE / 64u; i-- > 0;) {
                uint64_t value = i >= words ? window->bitmap[i - words] : 0;
                value <<= bits;
                if (bits && i > words)
                    value |= window->bitmap[i - words - 1] >> (64u - bits);
                window->bitmap[i] = value;
            }
        }
        window->highest = counter;
    }
    uint64_t age = window->highest - counter;
    window->bitmap[age / 64u] |= UINT64_C(1) << (age % 64u);
    return true;
}

uint64_t fips_replay_window_highest(const fips_replay_window *window)
{ return window->highest; }

void fips_replay_window_reset(fips_replay_window *window)
{ fips_replay_window_init(window); }
