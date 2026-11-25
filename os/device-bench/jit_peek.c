// jit_peek: where one thread of a running process spends its time, when that is JIT code no symbol table names.
//
//   jit_peek PID THREAD_NAME SAMPLES INTERVAL_US OUT_PREFIX
//
// Stops the named thread SAMPLES times (ptrace interrupt, a few microseconds each), notes pc, x28, x29 and x30, and
// the 32-bit word at x28+328 (Flycast's Sh4Context.pc while x28 holds the context). Then writes OUT.pcs (one sample
// a line), and OUT.page-ADDR for the 24 busiest 4 KiB pages of code (raw bytes from /proc/PID/mem) to disassemble on
// another machine. Build: zig cc -target aarch64-linux-musl -static -O2 -o jit_peek jit_peek.c
#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

struct regs {
    uint64_t x[31], sp, pc, pstate;
};

static int cmp(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char** argv) {
    if (argc < 6) return 2;
    int pid = atoi(argv[1]), count = atoi(argv[3]), gap = atoi(argv[4]), tid = 0;
    char path[256], name[64];
    unsigned long long busiest = 0;
    snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR* dir = opendir(path);
    if (!dir) {
        perror(path);
        return 1;
    }
    for (struct dirent* e; (e = readdir(dir));) {
        snprintf(path, sizeof path, "/proc/%d/task/%s/comm", pid, e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        int match = fgets(name, sizeof name, f) && !strncmp(name, argv[2], strlen(argv[2]));
        fclose(f);
        if (!match) continue;
        // Threads a named thread starts inherit its name: take the one that has used the most CPU.
        snprintf(path, sizeof path, "/proc/%d/task/%s/schedstat", pid, e->d_name);
        unsigned long long ran = 0;
        if ((f = fopen(path, "r"))) {
            if (fscanf(f, "%llu", &ran) != 1) ran = 0;
            fclose(f);
        }
        if (ran >= busiest) {
            busiest = ran;
            tid = atoi(e->d_name);
        }
    }
    closedir(dir);
    if (!tid) {
        fprintf(stderr, "no thread named %s\n", argv[2]);
        return 1;
    }
    snprintf(path, sizeof path, "/proc/%d/mem", pid);
    int mem = open(path, O_RDONLY);
    if (ptrace(PTRACE_SEIZE, tid, 0, 0)) {
        perror("seize");
        return 1;
    }
    snprintf(path, sizeof path, "%s.pcs", argv[5]);
    FILE* out = fopen(path, "w");
    uint64_t* pages = calloc(count, sizeof *pages);
    int taken = 0;
    for (int i = 0; i < count; i++) {
        int status, stopped = 0;
        if (ptrace(PTRACE_INTERRUPT, tid, 0, 0)) break;
        // The thread's own signals (Flycast's fast-memory faults) stop it too: pass those on and keep waiting for
        // the stop this asked for.
        while (waitpid(tid, &status, __WALL) == tid && WIFSTOPPED(status)) {
            if (status >> 16 == PTRACE_EVENT_STOP) {
                stopped = 1;
                break;
            }
            ptrace(PTRACE_CONT, tid, 0, WSTOPSIG(status));
        }
        if (!stopped) break;
        struct regs r;
        struct iovec io = {&r, sizeof r};
        if (!ptrace(PTRACE_GETREGSET, tid, NT_PRSTATUS, &io)) {
            uint32_t guest = 0;
            pread(mem, &guest, 4, r.x[28] + 328);
            fprintf(out, "%llx %llx %llx %llx %x\n", (unsigned long long)r.pc, (unsigned long long)r.x[28],
                    (unsigned long long)r.x[29], (unsigned long long)r.x[30], guest);
            pages[taken++] = r.pc >> 12;
        }
        ptrace(PTRACE_CONT, tid, 0, 0);
        usleep(gap);
    }
    fclose(out);
    qsort(pages, taken, sizeof *pages, cmp);
    struct {
        uint64_t page;
        int n;
    } top[24] = {0};
    for (int i = 0; i < taken;) {
        int j = i;
        while (j < taken && pages[j] == pages[i]) j++;
        int n = j - i, slot = 0;
        for (int k = 1; k < 24; k++)
            if (top[k].n < top[slot].n) slot = k;
        if (top[slot].n < n) {
            top[slot].page = pages[i];
            top[slot].n = n;
        }
        i = j;
    }
    static unsigned char buffer[4096];
    for (int k = 0; k < 24; k++) {
        if (!top[k].n) continue;
        if (pread(mem, buffer, 4096, top[k].page << 12) != 4096) continue;
        snprintf(path, sizeof path, "%s.page-%llx", argv[5], (unsigned long long)top[k].page << 12);
        FILE* f = fopen(path, "wb");
        fwrite(buffer, 1, 4096, f);
        fclose(f);
        printf("%llx %d\n", (unsigned long long)top[k].page << 12, top[k].n);
    }
    ptrace(PTRACE_DETACH, tid, 0, 0);
    printf("%d samples of thread %d\n", taken, tid);
    return 0;
}
