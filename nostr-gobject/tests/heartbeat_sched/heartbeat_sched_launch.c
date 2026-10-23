/*
 * heartbeat_sched_launch.c — record-only Darwin scheduling probe/launcher
 * (nostrc-0cfsg.11.1). NOT registered with CMake/CTest; compiled ad hoc by
 * heartbeat-sched-probe.sh. It never changes a test, a threshold or a result.
 *
 * Usage: heartbeat_sched_launch --policy inherit|fg|bg [--timer] [-- cmd args...]
 *
 *   inherit  leave the process policy exactly as inherited (the CI default)
 *   fg       child-local, unprivileged: setpriority(PRIO_DARWIN_PROCESS,0,0),
 *            i.e. clear the Darwin PROCESS background flag for THIS process
 *            only. It does not touch thread-level or QoS state; those are
 *            printed so a comparison can show if the inherited state was one
 *            this call cannot clear.
 *   bg       positive control: set PRIO_DARWIN_BG on this process only
 *
 * It prints the policy it observed (before and after), optionally measures
 * usleep()/poll() wake latency, then exec()s cmd (the policy survives exec).
 */
#include <errno.h>
#include <mach/mach.h>
#include <poll.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static double
now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Calling-thread CPU via thread_info(); clock_gettime(CLOCK_THREAD_CPUTIME_ID)
 * returned wall-clock time inside the ASan test binary on macOS 27, so it is
 * not trusted anywhere in this probe. */
static double
thread_cpu_ms(void)
{
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    mach_port_t self = mach_thread_self();
    kern_return_t kr = thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), self);
    if (kr != KERN_SUCCESS)
        return -1;
    return (info.user_time.seconds + info.system_time.seconds) * 1e3 +
           (info.user_time.microseconds + info.system_time.microseconds) / 1e3;
}

static int
read_prio(int which)
{
    errno = 0;
    int v = getpriority(which, 0);
    return (v == -1 && errno != 0) ? -errno - 1000 : v; /* <= -1000 means error */
}

static void
print_policy(const char *when)
{
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int rel = 0;
    pthread_get_qos_class_np(pthread_self(), &qos, &rel);
    printf("sched-probe: %s darwin_bg_process=%d darwin_bg_thread=%d qos=0x%x nice=%d\n",
           when, read_prio(PRIO_DARWIN_PROCESS), read_prio(PRIO_DARWIN_THREAD),
           (unsigned)qos, read_prio(PRIO_PROCESS));
}

static void
timer_probe(void)
{
    enum { N = 20, SLEEP_US = 5000 };
    double max = 0, sum = 0;
    double w0 = now_ms(), c0 = thread_cpu_ms();
    for (int i = 0; i < N; i++) {
        double a = now_ms();
        usleep(SLEEP_US);
        double d = now_ms() - a;
        sum += d;
        if (d > max)
            max = d;
    }
    printf("sched-probe: usleep(5ms) x%d mean=%.2f ms max=%.2f ms wall=%.1f ms thread_cpu=%.3f ms\n",
           N, sum / N, max, now_ms() - w0, thread_cpu_ms() - c0);
    max = sum = 0;
    w0 = now_ms();
    c0 = thread_cpu_ms();
    for (int i = 0; i < N; i++) {
        double a = now_ms();
        poll(NULL, 0, 5);
        double d = now_ms() - a;
        sum += d;
        if (d > max)
            max = d;
    }
    printf("sched-probe: poll(5ms) x%d mean=%.2f ms max=%.2f ms wall=%.1f ms thread_cpu=%.3f ms\n",
           N, sum / N, max, now_ms() - w0, thread_cpu_ms() - c0);
}

int
main(int argc, char **argv)
{
    const char *policy = "inherit";
    int timer = 0, i = 1;
    for (; i < argc && strcmp(argv[i], "--") != 0; i++) {
        if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc)
            policy = argv[++i];
        else if (strcmp(argv[i], "--timer") == 0)
            timer = 1;
        else {
            fprintf(stderr, "usage: %s --policy inherit|fg|bg [--timer] [-- cmd args...]\n", argv[0]);
            return 2;
        }
    }
    print_policy("before");
    if (strcmp(policy, "fg") == 0 || strcmp(policy, "bg") == 0) {
        int want = strcmp(policy, "bg") == 0 ? PRIO_DARWIN_BG : 0;
        errno = 0;
        int rc = setpriority(PRIO_DARWIN_PROCESS, 0, want);
        printf("sched-probe: setpriority(PRIO_DARWIN_PROCESS,0,%d) rc=%d errno=%d%s\n",
               want, rc, rc ? errno : 0, rc ? " policy NOT changed" : "");
        print_policy("after ");
    } else if (strcmp(policy, "inherit") != 0) {
        fprintf(stderr, "unknown policy %s\n", policy);
        return 2;
    }
    printf("sched-probe: requested_policy=%s\n", policy);
    if (timer)
        timer_probe();
    fflush(stdout);
    if (i < argc && strcmp(argv[i], "--") == 0 && i + 1 < argc) {
        execvp(argv[i + 1], &argv[i + 1]);
        fprintf(stderr, "sched-probe: exec %s failed: %s\n", argv[i + 1], strerror(errno));
        return 127;
    }
    return 0;
}
