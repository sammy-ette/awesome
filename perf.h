#ifndef AWESOME_PERF_H
#define AWESOME_PERF_H

#include <time.h>

#define TIMER_START(ts) do { clock_gettime(CLOCK_THREAD_CPUTIME_ID, &(ts)); } while (0)
#define TIMER_SET(ts, result) do {                                      \
        struct timespec __prev = (ts);                                  \
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &(ts));                  \
        result = ((ts).tv_sec - __prev.tv_sec) * 1000000000 + ((ts).tv_nsec - __prev.tv_nsec); \
    } while (0)   

#endif
