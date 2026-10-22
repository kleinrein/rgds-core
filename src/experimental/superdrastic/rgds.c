/* RGDS's file protocol for the optional SuperDrastic backend. No display hooks,
 * shell commands, clock policy, or engine memory patches live here. */
#define _GNU_SOURCE
#include "rgds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char last_token[80];
static int menu_open, speed, wanted_volume = -1, wanted_brightness = -1;
static unsigned setting_sequence;
static long long polled;

int rgds_sd_device_matches(const char* path, const char* name) {
    /* glibc's checked realpath requires PATH_MAX for a caller-owned buffer.
     * Let libc allocate the canonical path; short fixed buffers abort at boot. */
    char* resolved = realpath(path, NULL);
    int matches = resolved && strstr(resolved, name) != NULL;
    free(resolved);
    return matches;
}

static long long milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}
static void publish(const char* variable, const char* text, int durable) {
    const char* path = getenv(variable);
    if (!path || !*path || strlen(path) > 480) return;
    char pending[512];
    snprintf(pending, sizeof pending, "%s.pending", path);
    FILE* f = fopen(pending, "w");
    if (!f) return;
    int bad = fputs(text, f) < 0 || fflush(f);
    if (!bad && durable) bad = fsync(fileno(f));
    if (fclose(f)) bad = 1;
    if (!bad) rename(pending, path);
    else unlink(pending);
}
void rgds_sd_menu_state(int open) {
    menu_open = open;
    publish("RGDS_SD_MENU_STATUS", open ? "open=1\n" : "open=0\n", 0);
}
void rgds_sd_exit(const char* state) {
    char text[96];
    snprintf(text, sizeof text, "state=%s\n", state);
    publish("RGDS_SD_EXIT", text, 1);
}
void rgds_sd_settings(int volume, int brightness) {
    if (volume >= 0) wanted_volume = volume > 100 ? 100 : volume;
    if (brightness >= 0) wanted_brightness = brightness < 5 ? 5 : brightness > 100 ? 100 : brightness;
    char text[128];
    snprintf(text, sizeof text, "sequence=%u\nvolume=%d\nbrightness=%d\n", ++setting_sequence, wanted_volume,
             wanted_brightness);
    publish("RGDS_SD_SETTINGS", text, 0);
}
int rgds_sd_value(const char* key, int fallback) {
    const char* path = getenv("RGDS_SD_SYSTEM");
    if (!path) return fallback;
    FILE* f = fopen(path, "r");
    if (!f) return fallback;
    char line[128], name[64];
    int value;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%63[^=]=%d", name, &value) == 2 && !strcmp(name, key)) {
            fallback = value;
            break;
        }
    fclose(f);
    return fallback;
}
int rgds_sd_battery(int* charging) {
    *charging = rgds_sd_value("charging", 0);
    return rgds_sd_value("battery", -1);
}
int rgds_sd_poll(void* event) {
    if (rgds_shared_enabled()) return rgds_shared_poll(event);
    /* Queue synthetic controls through the native frame-held event scheduler.
     * Called only from the SDL/main thread, including the native menu loop. */
    (void)event;
    long long now = milliseconds();
    if (now - polled < 20) return 0;
    polled = now;
    const char* path = getenv("RGDS_SD_COMMAND");
    if (!path) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char token[80], action[40], extra;
    int count = fscanf(f, "%79s %39s %c", token, action, &extra);
    fclose(f);
    if (count != 2 || !strcmp(token, last_token)) return 0;
    for (const char* p = token; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || *p == '-')) return 0;
    const char* state = "unsupported";
    int toggle_menu = 0;
    if (!strcmp(action, "menu")) {
        if (!menu_open && !rgds_sd_idle()) return 0;
        state = "ok";
        toggle_menu = 1;
    } else if (!strcmp(action, "speed-on") || !strcmp(action, "speed-off") || !strcmp(action, "speed-toggle")) {
        int next = !strcmp(action, "speed-toggle") ? !speed : !strcmp(action, "speed-on");
        if (menu_open && next) state = "paused";
        else {
            if (next != speed && !rgds_sd_idle()) return 0;
            if (next != speed) {
                extern void resume_press(int);
                speed = next;
                resume_press(19);
            }
            state = speed ? "speed-on" : "speed-off";
        }
    }
    snprintf(last_token, sizeof last_token, "%s", token);
    char reply[192];
    snprintf(reply, sizeof reply, "token=%s\nstate=%s\n", token, state);
    publish("RGDS_SD_RESULT", reply, 0);
    if (toggle_menu) {
        rgds_sd_menu_toggle();
        return -1;
    }
    return 0;
}
