#define _POSIX_C_SOURCE 200809L

#include "donation.h"

#include <stdint.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static uint64_t donation_seed(void) {
    uint64_t seed = (uint64_t)time(NULL);
    seed ^= (uint64_t)(monotonic_seconds() * 1000000.0);
#if defined(_WIN32)
    seed ^= (uint64_t)GetCurrentProcessId() << 32;
#else
    seed ^= (uint64_t)getpid() << 32;
#endif
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return seed * UINT64_C(2685821657736338717);
}

int donation_level_valid(int level) {
    return level >= 0 && level < DONATION_CYCLE_MINUTES;
}

double donation_phase_seconds(int level, int donating) {
    int minutes = donating ? level : DONATION_CYCLE_MINUTES - level;
    return (double)minutes * 60.0;
}

double donation_initial_user_seconds(int level) {
    uint64_t seed = donation_seed();
    double unit = (double)(seed >> 11) * (1.0 / 9007199254740992.0);
    return donation_phase_seconds(level, 0) * (0.5 + unit);
}
