// Time since SoC reset by the ARM architected counter (it runs from reset; the kernel's clock starts later), minus
// the kernel's uptime: what the boot ROM, the SPL and U-Boot took before the kernel. Prints both, in seconds.
#include <stdio.h>
#include <stdint.h>
#include <time.h>
int main(void) {
    uint64_t cnt, frq;
    struct timespec up;
    asm volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    clock_gettime(CLOCK_BOOTTIME, &up);
    double since_reset = (double)cnt / (double)frq, uptime = up.tv_sec + up.tv_nsec / 1e9;
    printf("since reset %.3f s, kernel uptime %.3f s, before the kernel %.3f s\n", since_reset, uptime, since_reset - uptime);
    return 0;
}
