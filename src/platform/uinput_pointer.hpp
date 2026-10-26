#pragma once
// A virtual absolute mouse for PortMaster ports: the bottom panel's touches point and click
// on the top panel. Weston attaches an input device without a WL_OUTPUT udev tag to its first
// output, DSI-1, the top panel (weston.log: "associating input device ... with output DSI-1"),
// and maps an absolute pointer's whole range onto that output, so a touch at (x, y) on the
// bottom panel lands at the same place on the top one, whatever the top panel's resolution.
// udev tags absolute axes plus BTN_LEFT without INPUT_PROP_DIRECT as a mouse (as it does for
// VM tablets), so libinput reports absolute pointer motion rather than touch.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/uinput.h>
#include <sys/ioctl.h>
#endif
namespace rgds {
struct UinputPointer {
    int fd = -1, width = 640, height = 480;
    bool open(int w, int h) {
#ifdef __linux__
        if (fd >= 0) return true;
        width = w;
        height = h;
        fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return false;
        bool ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0 && ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0 &&
                  ioctl(fd, UI_SET_EVBIT, EV_SYN) == 0;
        for (int code : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) ok = ok && ioctl(fd, UI_SET_KEYBIT, code) == 0;
        for (int axis : {ABS_X, ABS_Y}) {
            ok = ok && ioctl(fd, UI_SET_ABSBIT, axis) == 0;
            uinput_abs_setup setup{};
            setup.code = uint16_t(axis);
            setup.absinfo.minimum = 0;
            setup.absinfo.maximum = (axis == ABS_X ? width : height) - 1;
            ok = ok && ioctl(fd, UI_ABS_SETUP, &setup) == 0;
        }
        uinput_setup device{};
        device.id.bustype = BUS_VIRTUAL;
        device.id.vendor = 0x5247;  // "RG"
        device.id.product = 0x4453; // "DS"
        std::strncpy(device.name, "RG DS touch mouse", UINPUT_MAX_NAME_SIZE - 1);
        ok = ok && ioctl(fd, UI_DEV_SETUP, &device) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
        if (!ok) close();
        return ok;
#else
        (void)w;
        (void)h;
        return false;
#endif
    }
    bool ready() const { return fd >= 0; }
    void move(int x, int y) {
        emit(EV_ABS_CODE, 0, x < 0 ? 0 : x >= width ? width - 1 : x);
        emit(EV_ABS_CODE, 1, y < 0 ? 0 : y >= height ? height - 1 : y);
        sync();
    }
    // button: 0 left, 1 right, 2 middle.
    void button(int which, bool down) {
        emit(EV_KEY_CODE, which, down ? 1 : 0);
        sync();
    }
    void click(int which) {
        button(which, true);
        button(which, false);
    }
    void close() {
#ifdef __linux__
        if (fd >= 0) {
            ioctl(fd, UI_DEV_DESTROY);
            ::close(fd);
        }
#endif
        fd = -1;
    }
    ~UinputPointer() { close(); }

private:
    enum Kind { EV_ABS_CODE, EV_KEY_CODE };
    void emit(Kind kind, int index, int value) {
#ifdef __linux__
        if (fd < 0) return;
        input_event event{};
        event.type = kind == EV_ABS_CODE ? EV_ABS : EV_KEY;
        event.code = uint16_t(kind == EV_ABS_CODE ? (index ? ABS_Y : ABS_X)
                                                  : (index == 1   ? BTN_RIGHT
                                                     : index == 2 ? BTN_MIDDLE
                                                                  : BTN_LEFT));
        event.value = value;
        ssize_t written = ::write(fd, &event, sizeof event);
        (void)written;
#else
        (void)kind;
        (void)index;
        (void)value;
#endif
    }
    void sync() {
#ifdef __linux__
        if (fd < 0) return;
        input_event event{};
        event.type = EV_SYN;
        event.code = SYN_REPORT;
        ssize_t written = ::write(fd, &event, sizeof event);
        (void)written;
#endif
    }
};
} // namespace rgds
