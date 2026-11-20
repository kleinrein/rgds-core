// vblank_phase: how far apart the two panels' vblanks are (RG DS OS, no DRM master needed).
// Build: zig cc -target aarch64-linux-musl -O2 -static -o vblank_phase vblank_phase.c
// Run:   ./vblank_phase [/dev/dri/card0] [seconds]  -> per second: each CRTC's period and the offset of CRTC 1's
//        vblank after CRTC 0's, in ms (0 = in step, ~8.3 = half a refresh apart).
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct wait_vblank_request {
    uint32_t type;
    uint32_t sequence;
    unsigned long signal;
};
struct wait_vblank_reply {
    uint32_t type;
    uint32_t sequence;
    long tval_sec;
    long tval_usec;
};
union wait_vblank {
    struct wait_vblank_request request;
    struct wait_vblank_reply reply;
};
#define DRM_IOCTL_WAIT_VBLANK _IOWR('d', 0x3a, union wait_vblank)
#define VBLANK_RELATIVE 0x1
#define VBLANK_HIGH_CRTC_SHIFT 1

static double wait_one(int fd, int crtc, unsigned* seq) {
    union wait_vblank v;
    memset(&v, 0, sizeof v);
    v.request.type = VBLANK_RELATIVE | ((unsigned)crtc << VBLANK_HIGH_CRTC_SHIFT);
    v.request.sequence = 1;
    if (ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &v) < 0) {
        perror("WAIT_VBLANK");
        return -1;
    }
    *seq = v.reply.sequence;
    return v.reply.tval_sec * 1000.0 + v.reply.tval_usec / 1000.0;
}

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "/dev/dri/card0";
    int seconds = argc > 2 ? atoi(argv[2]) : 5;
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(dev);
        return 1;
    }
    for (int s = 0; s < seconds; ++s) {
        double t0[61], t1[61];
        unsigned q;
        for (int i = 0; i < 61; ++i) t0[i] = wait_one(fd, 0, &q);
        for (int i = 0; i < 61; ++i) t1[i] = wait_one(fd, 1, &q);
        double p0 = (t0[60] - t0[0]) / 60, p1 = (t1[60] - t1[0]) / 60;
        // CRTC 1's vblank relative to CRTC 0's grid: extrapolate CRTC 0's last vblank forward by its period
        double off = t1[60] - t0[60];
        while (off > p0) off -= p0;
        while (off < 0) off += p0;
        printf("second %d: crtc0 %.4f ms (%.3f Hz)  crtc1 %.4f ms (%.3f Hz)  crtc1 after crtc0: %.2f ms\n", s, p0,
               1000 / p0, p1, 1000 / p1, off);
        fflush(stdout);
    }
    return 0;
}
