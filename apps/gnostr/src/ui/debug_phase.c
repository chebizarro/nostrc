/* debug_phase.c - Debug phase isolation globals */

#include <pthread.h>

/* Main thread ID for ownership checks */
pthread_t g_main_thread_id = 0;
