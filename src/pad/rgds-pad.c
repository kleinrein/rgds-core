/* rgds-pad: the RG DS's buttons, sticks and rumble as one game pad (docs/OWN_BSP_PLAN.md, Phase C step 2).
 *
 * RG DS OS's kernel runs the mainline input drivers, which describe the controls as separate devices:
 * gpio-keys-control (the buttons), adc-joystick (the four stick channels behind the analog mux, raw) and
 * pwm-vibrator (the motor). Everything above the kernel expects one pad, the way ROCKNIX's single-ADC joypad driver
 * presented it: the emulators open one controller, PortMaster's mappings name that pad's GUID, the 3DS engine opens it
 * by name. So this daemon reads the three devices and presents one virtual pad through uinput with that driver's
 * identity (name, IDs, buttons, stick ranges and arithmetic, rumble), and takes the three out of /dev/input so nothing
 * sees the controls twice. It opens them through its own device nodes made from sysfs (in /dev/rgds-pad), so a
 * restart still finds them,
 * and puts their /dev/input nodes back when it is stopped.
 *
 * Exit status 3: nothing to do (no mainline buttons and sticks, or a kernel joypad already there, as on the RG DS
 * Plus, whose tree still uses ROCKNIX's driver). rcS then stops restarting it. It writes /run/rgds/pad-ready once the
 * pad exists.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "rgds_pad_axes.h"

#define NAME "retrogame_joypad"
#define PHYS "rocknix-singleadc-joypad/input0"
/* on /dev (devtmpfs), not /run, which is mounted nodev; nothing scans this folder for controllers */
#define NODES "/dev/rgds-pad"
#define READY "/run/rgds/pad-ready"
#define BITS(n) (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define TEST(bit, array) ((array[(bit) / (8 * sizeof(long))] >> ((bit) % (8 * sizeof(long)))) & 1)

struct source {
    const char* name;
    int fd;
    char event[256]; /* "eventN" */
    unsigned dev;    /* major:minor, to put the /dev/input node back */
    int hidden;
    char js[256]; /* its joydev node ("jsN"), if any */
    unsigned jsdev;
    int js_hidden;
};

static struct source keys = {.name = "gpio-keys-control", .fd = -1}, sticks = {.name = "adc-joystick", .fd = -1},
                     motor = {.name = "pwm-vibrator", .fd = -1};
static volatile sig_atomic_t stopping;
static int debug;

/* The four mux states as adc-joystick reports them, and the pad axis each becomes: rgds_pad_axes.h. */
static const int* channel_code = rgds_pad_state_code;
static struct rgds_pad_axis axis[4];
static int raw[4];

static int ff_map[FF_MAX_EFFECTS > 64 ? 64 : FF_MAX_EFFECTS];

static void say(const char* fmt, const char* arg) {
    fprintf(stderr, "rgds-pad: ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
}

static int read_line(const char* path, char* out, size_t size) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)size, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}

/* A kernel input device named NAME, as on the RG DS Plus (ROCKNIX's driver): then there is nothing to merge. */
static int kernel_joypad(void) {
    DIR* d = opendir("/sys/class/input");
    struct dirent* e;
    char path[512], name[128], real[PATH_MAX];
    int found = 0;
    if (!d) return 0;
    while (!found && (e = readdir(d))) {
        if (strncmp(e->d_name, "input", 5)) continue;
        snprintf(path, sizeof path, "/sys/class/input/%s/name", e->d_name);
        if (read_line(path, name, sizeof name) || strcmp(name, NAME)) continue;
        snprintf(path, sizeof path, "/sys/class/input/%s", e->d_name);
        if (realpath(path, real) && !strstr(real, "/virtual/")) found = 1;
    }
    closedir(d);
    return found;
}

/* The joydev node ("jsN") of an input device, from sysfs: /sys/class/input/eventN/device/jsN. */
static void find_js(const char* event, char* js, size_t size, unsigned* dev) {
    char path[1024], line[32];
    unsigned major_n, minor_n;
    DIR* d;
    struct dirent* e;
    js[0] = 0;
    snprintf(path, sizeof path, "/sys/class/input/%s/device", event);
    if (!(d = opendir(path))) return;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "js", 2)) continue;
        snprintf(path, sizeof path, "/sys/class/input/%s/device/%s/dev", event, e->d_name);
        if (read_line(path, line, sizeof line) == 0 && sscanf(line, "%u:%u", &major_n, &minor_n) == 2) {
            snprintf(js, size, "%s", e->d_name);
            *dev = (unsigned)makedev(major_n, minor_n);
        }
        break;
    }
    closedir(d);
}

/* Old programs open /dev/input/js0 for the pad, which ROCKNIX's driver was: our pad's joydev node takes the lowest
 * jsN name the sources freed, as an alias node (the kernel's own name for it goes). */
static char pad_js_alias[64];
static void name_pad_js(void) {
    DIR* d = opendir("/sys/class/input");
    struct dirent* e;
    char path[512], name[128], real[PATH_MAX], js[256] = "", node[300];
    unsigned dev = 0;
    int lowest = -1;
    struct source* all[3] = {&keys, &sticks, &motor};
    if (!d) return;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "js", 2)) continue;
        snprintf(path, sizeof path, "/sys/class/input/%s/device/name", e->d_name);
        if (read_line(path, name, sizeof name) || strcmp(name, NAME)) continue;
        snprintf(path, sizeof path, "/sys/class/input/%s", e->d_name);
        if (!realpath(path, real) || !strstr(real, "/virtual/")) continue;
        snprintf(path, sizeof path, "/sys/class/input/%s/dev", e->d_name);
        unsigned major_n, minor_n;
        if (read_line(path, name, sizeof name) == 0 && sscanf(name, "%u:%u", &major_n, &minor_n) == 2) {
            snprintf(js, sizeof js, "%s", e->d_name);
            dev = (unsigned)makedev(major_n, minor_n);
        }
        break;
    }
    closedir(d);
    if (!js[0]) return;
    for (int i = 0; i < 3; i++)
        if (all[i]->js_hidden) {
            int n = atoi(all[i]->js + 2);
            if (lowest < 0 || n < lowest) lowest = n;
        }
    if (lowest < 0 || lowest >= atoi(js + 2)) return;
    snprintf(pad_js_alias, sizeof pad_js_alias, "/dev/input/js%d", lowest);
    if (mknod(pad_js_alias, S_IFCHR | 0660, dev) == 0) {
        snprintf(node, sizeof node, "/dev/input/%s", js);
        unlink(node);
    } else {
        pad_js_alias[0] = 0;
    }
}

/* Open a source through a node of our own (made from its sysfs dev number), so it is found even after its
 * /dev/input node has been taken away by an earlier run. */
static int open_source(struct source* s) {
    DIR* d = opendir("/sys/class/input");
    struct dirent* e;
    char path[512], name[128], dev[32], node[300];
    unsigned major_n, minor_n;
    if (!d) return -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        snprintf(path, sizeof path, "/sys/class/input/%s/device/name", e->d_name);
        if (read_line(path, name, sizeof name) || strcmp(name, s->name)) continue;
        snprintf(path, sizeof path, "/sys/class/input/%s/dev", e->d_name);
        if (read_line(path, dev, sizeof dev) || sscanf(dev, "%u:%u", &major_n, &minor_n) != 2) continue;
        snprintf(s->event, sizeof s->event, "%s", e->d_name);
        s->dev = (unsigned)makedev(major_n, minor_n);
        find_js(e->d_name, s->js, sizeof s->js, &s->jsdev);
        snprintf(node, sizeof node, NODES "/%s", e->d_name);
        unlink(node);
        if (mknod(node, S_IFCHR | 0600, s->dev) == 0) s->fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        break;
    }
    closedir(d);
    return s->fd;
}

/* Take a source out of /dev/input, so SDL, the launcher and the menus see only the merged pad. */
static void hide(struct source* s) {
    char node[300];
    if (s->fd < 0) return;
    snprintf(node, sizeof node, "/dev/input/%s", s->event);
    if (unlink(node) == 0 || errno == ENOENT) s->hidden = 1;
    if (s->js[0]) {
        snprintf(node, sizeof node, "/dev/input/%s", s->js);
        if (unlink(node) == 0 || errno == ENOENT) s->js_hidden = 1;
    }
}

static void unhide(struct source* s) {
    char node[300];
    if (!s->hidden) return;
    snprintf(node, sizeof node, "/dev/input/%s", s->event);
    if (mknod(node, S_IFCHR | 0660, s->dev) == 0 || errno == EEXIST) s->hidden = 0;
    if (s->js_hidden) {
        snprintf(node, sizeof node, "/dev/input/%s", s->js);
        unlink(node); /* our pad's alias, if it took this name */
        if (mknod(node, S_IFCHR | 0660, s->jsdev) == 0) s->js_hidden = 0;
    }
}

static void emit(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev && debug) perror("rgds-pad: write");
}

static int create_pad(void) {
    unsigned long keybits[BITS(KEY_CNT)];
    struct uinput_setup setup;
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    memset(keybits, 0, sizeof keybits);
    ioctl(keys.fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int code = 0; code < KEY_CNT; code++)
        if (TEST(code, keybits)) ioctl(fd, UI_SET_KEYBIT, code);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (int i = 0; i < 4; i++) {
        struct uinput_abs_setup abs;
        memset(&abs, 0, sizeof abs);
        abs.code = (unsigned short)axis[i].code;
        abs.absinfo.minimum = -RGDS_PAD_MAX;
        abs.absinfo.maximum = RGDS_PAD_MAX;
        abs.absinfo.fuzz = RGDS_PAD_FUZZ;
        abs.absinfo.flat = RGDS_PAD_FLAT;
        ioctl(fd, UI_ABS_SETUP, &abs);
    }
    if (motor.fd >= 0) {
        /* the motor's own effect bits: ff-memless gives rumble, the periodic waveforms and gain, as it gave the
         * joypad */
        unsigned long ffbits[BITS(FF_CNT)];
        memset(ffbits, 0, sizeof ffbits);
        ioctl(motor.fd, EVIOCGBIT(EV_FF, sizeof ffbits), ffbits);
        ioctl(fd, UI_SET_EVBIT, EV_FF);
        for (int code = 0; code < FF_CNT; code++)
            if (TEST(code, ffbits)) ioctl(fd, UI_SET_FFBIT, code);
    }
    ioctl(fd, UI_SET_PHYS, PHYS);
    memset(&setup, 0, sizeof setup);
    setup.id.bustype = BUS_HOST;
    setup.id.vendor = 0x484b;
    setup.id.product = 0x1101;
    setup.id.version = 0x0100;
    snprintf(setup.name, sizeof setup.name, "%s", NAME);
    setup.ff_effects_max = motor.fd >= 0 ? (unsigned)(sizeof ff_map / sizeof ff_map[0]) : 0;
    if (setup.ff_effects_max > 16) setup.ff_effects_max = 16; /* the 16 slots ff-memless gave the joypad */
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The buttons held right now, as a fresh state (at the start, and after the kernel dropped events). */
static void sync_keys(int pad, unsigned long* held) {
    unsigned long now[BITS(KEY_CNT)];
    memset(now, 0, sizeof now);
    ioctl(keys.fd, EVIOCGKEY(sizeof now), now);
    for (int code = 0; code < KEY_CNT; code++)
        if (TEST(code, now) != TEST(code, held)) emit(pad, EV_KEY, code, (int)TEST(code, now));
    memcpy(held, now, sizeof now);
    emit(pad, EV_SYN, SYN_REPORT, 0);
}

static void set_held(unsigned long* held, int code, int down) {
    unsigned long mask = 1UL << (code % (8 * sizeof(long)));
    if (down) held[code / (8 * sizeof(long))] |= mask;
    else held[code / (8 * sizeof(long))] &= ~mask;
}

static void report_sticks(int pad) {
    for (int p = 0; p < 2; p++) {
        int a = rgds_pad_stick_states[p][0], b = rgds_pad_stick_states[p][1];
        if (!rgds_pad_stick(&axis[a], raw[a], &axis[b], raw[b])) continue;
        emit(pad, EV_ABS, axis[a].code, rgds_pad_report(&axis[a]));
        emit(pad, EV_ABS, axis[b].code, rgds_pad_report(&axis[b]));
    }
    emit(pad, EV_SYN, SYN_REPORT, 0);
}

static int channel_of(int code) {
    for (int i = 0; i < 4; i++)
        if (channel_code[i] == code) return i;
    return -1;
}

/* The rest position: the first full reading after the sticks device was opened (adc-joystick starts polling then),
 * as ROCKNIX's driver took it in its open. */
static void calibrate(void) {
    struct pollfd p = {sticks.fd, POLLIN, 0};
    struct input_event ev;
    for (int tries = 0; tries < 20; tries++) {
        if (poll(&p, 1, 50) > 0)
            while (read(sticks.fd, &ev, sizeof ev) == (ssize_t)sizeof ev);
        int ready = 1;
        for (int i = 0; i < 4; i++) {
            struct input_absinfo info;
            if (ioctl(sticks.fd, EVIOCGABS(channel_code[i]), &info) == 0) raw[i] = info.value;
            if (raw[i] == 0) ready = 0;
        }
        if (ready) break;
    }
    for (int i = 0; i < 4; i++) axis[i].cal = raw[i] * RGDS_PAD_SCALE;
}

static void ff_upload(int pad, int request) {
    struct uinput_ff_upload up;
    memset(&up, 0, sizeof up);
    up.request_id = (unsigned)request;
    if (ioctl(pad, UI_BEGIN_FF_UPLOAD, &up) < 0) return;
    int slot = up.effect.id;
    struct ff_effect effect = up.effect;
    effect.id = (slot >= 0 && slot < (int)(sizeof ff_map / sizeof ff_map[0])) ? ff_map[slot] : -1;
    if (ioctl(motor.fd, EVIOCSFF, &effect) < 0) {
        up.retval = -errno;
    } else {
        up.retval = 0;
        if (slot >= 0 && slot < (int)(sizeof ff_map / sizeof ff_map[0])) ff_map[slot] = effect.id;
    }
    ioctl(pad, UI_END_FF_UPLOAD, &up);
}

static void ff_erase(int pad, int request) {
    struct uinput_ff_erase er;
    memset(&er, 0, sizeof er);
    er.request_id = (unsigned)request;
    if (ioctl(pad, UI_BEGIN_FF_ERASE, &er) < 0) return;
    int slot = (int)er.effect_id;
    er.retval = 0;
    if (slot >= 0 && slot < (int)(sizeof ff_map / sizeof ff_map[0]) && ff_map[slot] >= 0) {
        if (ioctl(motor.fd, EVIOCRMFF, ff_map[slot]) < 0) er.retval = -errno;
        ff_map[slot] = -1;
    }
    ioctl(pad, UI_END_FF_ERASE, &er);
}

static void on_signal(int sig) {
    (void)sig;
    stopping = 1;
}

/* Off the emulation core (CPU 3, docs/OWN_OS_PLAN.md) and ahead of ordinary work, so a press is passed on at once. */
static void placement(void) {
    cpu_set_t cpus;
    struct sched_param param = {10};
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    CPU_ZERO(&cpus);
    for (long c = 0; c < (n > 3 ? n - 1 : n); c++) CPU_SET((int)c, &cpus);
    sched_setaffinity(0, sizeof cpus, &cpus);
    sched_setscheduler(0, SCHED_FIFO, &param);
}

int main(void) {
    unsigned long held[BITS(KEY_CNT)];
    struct sigaction sa;
    int pad;

    debug = getenv("RGDS_PAD_DEBUG") != NULL;
    for (size_t i = 0; i < sizeof ff_map / sizeof ff_map[0]; i++) ff_map[i] = -1;
    for (int i = 0; i < 4; i++) {
        axis[i].code = rgds_pad_state_axis[i];
        axis[i].invert = rgds_pad_state_invert[i];
    }
    if (kernel_joypad()) {
        say("a kernel %s is there already: nothing to merge", NAME);
        return 3;
    }
    mkdir("/run/rgds", 0755);
    mkdir(NODES, 0700);
    if (open_source(&keys) < 0 || open_source(&sticks) < 0) {
        say("no %s and adc-joystick: nothing to merge", keys.name);
        return 3;
    }
    open_source(&motor); /* optional: a pad without rumble still works */
    ioctl(keys.fd, EVIOCGRAB, 1);
    ioctl(sticks.fd, EVIOCGRAB, 1);
    calibrate();
    pad = create_pad();
    if (pad < 0) {
        perror("rgds-pad: uinput");
        return 1;
    }
    hide(&keys);
    hide(&sticks);
    hide(&motor);
    name_pad_js();
    memset(held, 0, sizeof held);
    sync_keys(pad, held);
    report_sticks(pad);
    placement();

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    {
        FILE* f = fopen(READY, "w");
        if (f) {
            fprintf(f, "%d\n", (int)getpid());
            fclose(f);
        }
    }

    struct pollfd fds[3] = {{keys.fd, POLLIN, 0}, {sticks.fd, POLLIN, 0}, {pad, POLLIN, 0}};
    struct input_event ev;
    while (!stopping) {
        if (poll(fds, 3, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP) || fds[1].revents & (POLLERR | POLLHUP)) break;
        if (fds[0].revents & POLLIN) {
            while (read(keys.fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type == EV_KEY && ev.code < KEY_CNT) {
                    emit(pad, EV_KEY, ev.code, ev.value);
                    set_held(held, ev.code, ev.value != 0);
                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    emit(pad, EV_SYN, SYN_REPORT, 0);
                } else if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
                    sync_keys(pad, held);
                }
            }
        }
        if (fds[1].revents & POLLIN) {
            int changed = 0;
            while (read(sticks.fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type == EV_ABS) {
                    int ch = channel_of(ev.code);
                    if (ch >= 0) {
                        raw[ch] = ev.value;
                        changed = 1;
                    }
                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT && changed) {
                    report_sticks(pad);
                    changed = 0;
                }
            }
        }
        if (fds[2].revents & POLLIN) {
            while (read(pad, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) ff_upload(pad, ev.value);
                else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) ff_erase(pad, ev.value);
                else if (ev.type == EV_FF && motor.fd >= 0) {
                    int code = ev.code;
                    if (code != FF_GAIN) {
                        if (code < 0 || code >= (int)(sizeof ff_map / sizeof ff_map[0]) || ff_map[code] < 0) continue;
                        code = ff_map[code];
                    }
                    emit(motor.fd, EV_FF, code, ev.value);
                }
            }
        }
    }

    unlink(READY);
    ioctl(pad, UI_DEV_DESTROY);
    close(pad);
    unhide(&keys);
    unhide(&sticks);
    unhide(&motor);
    return stopping ? 0 : 1;
}
