// rgds_prof.so: a sampling profiler that rides along with a process (LD_PRELOAD), for a handheld with no perf.
//
//   RGDS_PROF=/path/out LD_PRELOAD=/path/rgds_prof.so program ...
//
// Every millisecond of CPU time the process uses (ITIMER_PROF), the thread that was running records its thread id,
// program counter and return address (x30: the caller of a leaf function). At exit, or on SIGUSR2, it writes
// out.samples (milliseconds since start, tid, pc and lr in hex) and out.maps (a copy of /proc/self/maps) and out.threads (tid name); turning
// addresses into function names happens on a machine with the unstripped libraries (rgds_prof_report.py).
// RGDS_PROF_START=N skips the first N seconds (boot), RGDS_PROF_HZ sets the rate (default 1000).
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define MAX_SAMPLES (4u << 20)

struct sample {
    uint32_t tid, ms;
    uint64_t pc, lr;
};

static struct sample *samples;
static atomic_uint used;
static char out[512];
static atomic_int written;
static double start_after;
static struct timespec began;

// Every ioctl counted by request code, with the time it took (out.ioctls: request hex, calls, total ms), so kernel
// time in a GPU driver can be split into submits, waits, buffer creation and so on.
#define IOCTL_SLOTS 128
static struct {
    _Atomic unsigned long request;
    atomic_ulong calls, ns;
} ioctls[IOCTL_SLOTS];

int ioctl(int fd, unsigned long request, ...) {
    static int (*real)(int, unsigned long, ...);
    if (!real) real = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (!samples) return real(fd, request, arg);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int result = real(fd, request, arg);
    clock_gettime(CLOCK_MONOTONIC, &b);
    unsigned long slot = (request * 2654435761u) % IOCTL_SLOTS;
    for (int i = 0; i < IOCTL_SLOTS; ++i, slot = (slot + 1) % IOCTL_SLOTS) {
        unsigned long expected = 0;
        if (atomic_load(&ioctls[slot].request) == request ||
            atomic_compare_exchange_strong(&ioctls[slot].request, &expected, request)) {
            atomic_fetch_add(&ioctls[slot].calls, 1);
            atomic_fetch_add(&ioctls[slot].ns, (unsigned long)((b.tv_sec - a.tv_sec) * 1000000000L + b.tv_nsec - a.tv_nsec));
            break;
        }
    }
    return result;
}

static void on_prof(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const double seconds = now.tv_sec - began.tv_sec + (now.tv_nsec - began.tv_nsec) / 1e9;
    if (seconds < start_after) return;
    unsigned at = atomic_fetch_add_explicit(&used, 1, memory_order_relaxed);
    if (at >= MAX_SAMPLES) return;
    const ucontext_t *uc = context;
    samples[at].tid = (uint32_t)syscall(SYS_gettid);
    samples[at].ms = (uint32_t)(seconds * 1000);
    samples[at].pc = uc->uc_mcontext.pc;
    samples[at].lr = uc->uc_mcontext.regs[30];
}

static void copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "r"), *o = fopen(to, "w");
    char buffer[4096];
    size_t n;
    while (in && o && (n = fread(buffer, 1, sizeof buffer, in)) > 0) fwrite(buffer, 1, n, o);
    if (in) fclose(in);
    if (o) fclose(o);
}

static void dump(void) {
    if (!samples || atomic_exchange(&written, 1)) return;
    struct itimerval off = {0};
    setitimer(ITIMER_PROF, &off, NULL);
    char path[600];
    snprintf(path, sizeof path, "%s.maps", out);
    copy_file("/proc/self/maps", path);
    snprintf(path, sizeof path, "%s.threads", out);
    FILE *f = fopen(path, "w");
    DIR *tasks = opendir("/proc/self/task");
    for (struct dirent *e; f && tasks && (e = readdir(tasks));) {
        if (e->d_name[0] == '.') continue;
        char comm_path[300], comm[64] = "";
        snprintf(comm_path, sizeof comm_path, "/proc/self/task/%s/comm", e->d_name);
        FILE *c = fopen(comm_path, "r");
        if (c) {
            if (fgets(comm, sizeof comm, c)) comm[strcspn(comm, "\n")] = 0;
            fclose(c);
        }
        fprintf(f, "%s %s\n", e->d_name, comm);
    }
    if (tasks) closedir(tasks);
    if (f) fclose(f);
    snprintf(path, sizeof path, "%s.ioctls", out);
    f = fopen(path, "w");
    for (int i = 0; f && i < IOCTL_SLOTS; ++i)
        if (ioctls[i].request)
            fprintf(f, "%lx %lu %.1f\n", (unsigned long)ioctls[i].request, (unsigned long)ioctls[i].calls,
                    ioctls[i].ns / 1e6);
    if (f) fclose(f);
    snprintf(path, sizeof path, "%s.samples", out);
    f = fopen(path, "w");
    unsigned n = atomic_load(&used);
    if (n > MAX_SAMPLES) n = MAX_SAMPLES;
    for (unsigned i = 0; f && i < n; ++i)
        fprintf(f, "%u %u %llx %llx\n", samples[i].ms, samples[i].tid, (unsigned long long)samples[i].pc,
                (unsigned long long)samples[i].lr);
    if (f) fclose(f);
    fprintf(stderr, "rgds_prof: %u samples to %s\n", n, path);
}

static void on_usr2(int sig) {
    (void)sig;
    dump();
}

__attribute__((constructor)) static void start(void) {
    const char *target = getenv("RGDS_PROF");
    if (!target || !*target) return;
    snprintf(out, sizeof out, "%s", target);
    unsetenv("LD_PRELOAD"); // not into the children (the runtime's helpers)
    samples = calloc(MAX_SAMPLES, sizeof *samples);
    if (!samples) return;
    const char *after = getenv("RGDS_PROF_START");
    start_after = after ? atof(after) : 0;
    clock_gettime(CLOCK_MONOTONIC, &began);
    const char *hz_env = getenv("RGDS_PROF_HZ");
    int hz = hz_env ? atoi(hz_env) : 1000;
    if (hz < 1) hz = 1000;
    struct sigaction prof = {0};
    prof.sa_sigaction = on_prof;
    prof.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &prof, NULL);
    signal(SIGUSR2, on_usr2);
    atexit(dump);
    struct itimerval every = {{0, 1000000 / hz}, {0, 1000000 / hz}};
    setitimer(ITIMER_PROF, &every, NULL);
}

__attribute__((destructor)) static void stop(void) { dump(); }
