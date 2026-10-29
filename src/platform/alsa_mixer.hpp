#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef __linux__
#include <fcntl.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace rgds {
// Saved RG DS level (0-100) to the RK817 DAC percentage used by runtime/audio.py.
inline int dac_percent_for_level(int level) { return level <= 0 ? 0 : 30 + int(std::lround(70.0 * (level - 1) / 99)); }

// Reapply the DAC level without spawning a helper. Opening the first playback
// sink resets the RK817 DAC, so a reopened menu sink restores it before any
// sample is heard. Rounding mirrors `amixer sset DAC N%`.
inline bool write_mixer_percent(int percent, const char* name = "DAC Playback Volume",
                                const char* device = "/dev/snd/controlC0") {
#ifdef __linux__
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    snd_ctl_elem_info info{};
    info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    std::strncpy(reinterpret_cast<char*>(info.id.name), name, sizeof(info.id.name) - 1);
    bool ok = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0 && info.type == SNDRV_CTL_ELEM_TYPE_INTEGER &&
              info.count > 0 && info.count <= 128;
    if (ok) {
        long min = info.value.integer.min, max = info.value.integer.max;
        long raw = long(std::ceil(percent * (max - min) * 0.01 + min));
        snd_ctl_elem_value value{};
        value.id = info.id;
        for (unsigned i = 0; i < info.count; i++) value.value.integer.value[i] = raw;
        ok = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) >= 0;
    }
    close(fd);
    return ok;
#else
    (void)percent;
    (void)name;
    (void)device;
    return true;
#endif
}

// The saved level on the card that plays now. RG DS OS names it in /run/rgds/audio-mixer ("<card> rk817|aw88166",
// /usr/libexec/rgds/audio); without the file the RK817 is card 0. The RG DS Plus's AW88166 speakers keep the level
// runtime/audio.py gave them (an attenuation, and no reset when a sink opens), so there is nothing to restore.
inline bool write_saved_level(int level, const char* route = "/run/rgds/audio-mixer") {
    int card = 0;
    char kind[16] = "rk817";
    if (FILE* f = std::fopen(route, "r")) {
        if (std::fscanf(f, "%d %15s", &card, kind) != 2 || card < 0) {
            card = 0;
            std::strcpy(kind, "rk817");
        }
        std::fclose(f);
    }
    if (std::strcmp(kind, "aw88166") == 0) return true;
    char device[32];
    std::snprintf(device, sizeof device, "/dev/snd/controlC%d", card);
    const int percent = dac_percent_for_level(level);
    return write_mixer_percent(percent, "DAC Playback Volume", device) ||
           write_mixer_percent(percent, "Master Playback Volume", device);
}
}
