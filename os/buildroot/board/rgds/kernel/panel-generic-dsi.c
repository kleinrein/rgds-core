// SPDX-License-Identifier: GPL-2.0
/*
 * Generic MIPI-DSI panel driver
 * Copyright (C) 2024 ROCKNIX
 *
 * based on
 *
 * Rockteck jh057n00900 5.5" MIPI-DSI panel driver
 * Copyright (C) Purism SPC 2019
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/gpio/consumer.h>
#include <linux/hex.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/version.h>

#include <video/display_timing.h>
#include <video/mipi_display.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0))
#include <drm/drm_of.h>
#endif
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define DRIVER_NAME "panel-generic-dsi"

static char* descfile = "";
module_param(descfile, charp, 0660);
MODULE_PARM_DESC(descfile, "Panel description filename in firmware dir");

/*
 * RG DS OS: the two panels used to cost 700 ms of the boot, prepared one after the other inside the first modeset
 * (power, reset and the init delay, the DCS sequence, then a 120 ms wait after sleep-out before the backlight).
 * With early_power the power and reset part runs from probe on a worker, long before DRM binds, and the wait after
 * sleep-out becomes a deadline for the backlight instead of a sleep, so the second panel's sequence overlaps it.
 */
static bool early_power = true;
module_param(early_power, bool, 0444);
MODULE_PARM_DESC(early_power, "Power and reset the panel at probe, and gate only the backlight on the sleep-out delay");

struct generic_panel_delays {
    int prepare;
    int reset;
    int init;
    int enable;
    int ready;
};

struct generic_panel_size {
    int width;
    int height;
};

struct generic_panel_mode {
    int clock;
    int horizontal[5];
    int vertical[5];
    int is_default;
    struct generic_panel_mode* prev;
};

#define DCS_PSEUDO_CMD_SEQ 0x10000
struct generic_panel_init_seq {
    int dcs;
    int len;
    int read;
    int wait;
    u8* data;
    struct generic_panel_init_seq* link;
};

struct generic_panel {
    struct device* dev;
    struct drm_panel panel;
    struct gpio_desc* enable_gpio;
    struct gpio_desc* reset_gpio;
    struct regulator* vdd;
    struct regulator* iovcc;

    struct generic_panel_delays delays;
    struct generic_panel_size size;
    struct generic_panel_mode* modes;
    struct generic_panel_init_seq* iseq;

    enum drm_panel_orientation orientation;
    bool prepared;

    /* early_power: power/reset from probe, backlight on a deadline */
    struct backlight_device* backlight;
    struct work_struct power_work;
    struct delayed_work backlight_work;
    struct mutex power_lock;
    bool powered;
    ktime_t reset_released;
    ktime_t ready_at;
};

static void generic_panel_power_off(struct generic_panel* ctx);
int load_panel_description(struct mipi_dsi_device* dsi, struct generic_panel* ctx);
int load_panel_description_line(char* data, struct mipi_dsi_device* dsi, struct generic_panel* ctx);
int load_globals(char* data, struct mipi_dsi_device* dsi, struct generic_panel* ctx);
int load_mode(char* data, struct mipi_dsi_device* dsi, struct generic_panel* ctx);
int load_init_seq(char* data, struct mipi_dsi_device* dsi, struct generic_panel* ctx);

int load_globals(char* data, struct mipi_dsi_device* dsi, struct generic_panel* ctx) {
    struct device* dev = &dsi->dev;
    char *param, *val;
    while (*data) {
        data = next_arg(data, &param, &val);
        if (!val) continue;
        if (strcmp(param, "delays") == 0) {
            int delays[] = {0, 5, 1, 25, 120, 50};
            get_options(val, 6, delays);
            ctx->delays.prepare = delays[1];
            ctx->delays.reset = delays[2];
            ctx->delays.init = delays[3];
            ctx->delays.enable = delays[4];
            ctx->delays.ready = delays[5];
        } else if (strcmp(param, "size") == 0) {
            int size[] = {0, -1, -1};
            get_options(val, 3, size);
            ctx->size.width = size[1];
            ctx->size.height = size[2];
        } else if (strcmp(param, "format") == 0) {
            if (strcmp(val, "rgb888") == 0) {
                dsi->format = MIPI_DSI_FMT_RGB888;
            } else if (strcmp(val, "rgb666") == 0) {
                dsi->format = MIPI_DSI_FMT_RGB666;
            } else if (strcmp(val, "rgb666_packed") == 0) {
                dsi->format = MIPI_DSI_FMT_RGB666_PACKED;
            } else if (strcmp(val, "rgb565") == 0) {
                dsi->format = MIPI_DSI_FMT_RGB565;
            } else {
                dev_info(dev, "bad format %s\n", val);
            }
        } else if (strcmp(param, "lanes") == 0) {
            if (get_option(&val, &dsi->lanes) == 0) {
                dev_info(dev, "bad lanes %s\n", val);
            }
        } else if (strcmp(param, "flags") == 0) {
            int flags;
            if (get_option(&val, &flags) == 0) {
                dev_info(dev, "bad flags %s\n", val);
            } else {
                dsi->mode_flags = flags;
            }
        } else {
            dev_info(dev, "unknown param %s\n", param);
            return -1;
        }
    }
    return 0;
}

int load_mode(char* data, struct mipi_dsi_device* dsi, struct generic_panel* ctx) {
    struct device* dev = &dsi->dev;
    struct generic_panel_mode* mode;
    char *param, *val;

    mode = devm_kzalloc(dev, sizeof(*mode), GFP_KERNEL);

    while (*data) {
        data = next_arg(data, &param, &val);
        if (!val) continue;
        if (strcmp(param, "clock") == 0) {
            if (get_option(&val, &mode->clock) == 0) {
                dev_info(dev, "bad clock %s\n", val);
            }
        } else if (strcmp(param, "horizontal") == 0) {
            get_options(val, 5, &mode->horizontal[0]);
        } else if (strcmp(param, "vertical") == 0) {
            get_options(val, 5, &mode->vertical[0]);
        } else if (strcmp(param, "default") == 0) {
            get_option(&val, &mode->is_default);
        } else {
            dev_info(dev, "Mode unhandled %s = %s\n", param, val);
        }
    }

    if ((mode->clock > 5000) && (mode->vertical[1] > 0) && (mode->horizontal[1] > 0)) {
        mode->prev = ctx->modes;
        ctx->modes = mode;
        dev_dbg(dev, "Mode %d %d %d  %p -> %p\n", mode->clock, mode->vertical[1], mode->horizontal[1], mode,
                mode->prev);
        return 0;
    }
    return -1;
}

int load_init_seq(char* data, struct mipi_dsi_device* dsi, struct generic_panel* ctx) {
    struct device* dev = &dsi->dev;
    struct generic_panel_init_seq* item;
    char *param, *val;

    item = devm_kzalloc(dev, sizeof(*item), GFP_KERNEL);
    item->dcs = -1;
    item->len = -1;
    item->read = 0;
    item->wait = 0;

    while (*data) {
        data = next_arg(data, &param, &val);
        if (!val) continue;
        if (strcmp(param, "dcs") == 0) {
            item->dcs = simple_strtoul(val, NULL, 16) & 0xFF;
            // dev_info(dev, "Init dcs %02x\n", item->dcs);
        } else if (strcmp(param, "data") == 0) {
            item->len = (strlen(val)) >> 1;
            item->data = devm_kzalloc(dev, item->len, GFP_KERNEL);
            if (hex2bin(item->data, val, item->len) != 0) {
                dev_info(dev, "bad data %s\n", val);
                return -1;
            }
            // for (int i=0; i < (item->len); i++) { dev_info(dev, " %02x", item->data[i]);};
            // dev_info(dev, "\n");
        } else if (strcmp(param, "seq") == 0) {
            item->dcs = DCS_PSEUDO_CMD_SEQ;
            item->len = (strlen(val)) >> 1;
            item->data = devm_kzalloc(dev, item->len, GFP_KERNEL);
            if (hex2bin(item->data, val, item->len) != 0) {
                dev_info(dev, "bad seq %s\n", val);
                return -1;
            }
            dev_dbg(dev, "loaded seq len=%d %s\n", item->len, val);
        } else if (strcmp(param, "read") == 0) {
            item->read = simple_strtoul(val, NULL, 16);
        } else if (strcmp(param, "wait") == 0) {
            item->wait = simple_strtoul(val, NULL, 16);
        } else {
            dev_info(dev, "Init unhandled %s = %s\n", param, val);
        }
    }

    if (item->dcs >= 0) {
        item->link = ctx->iseq;
        ctx->iseq = item;
        return 0;
    }

    return -1;
}

/*
 * data   is a null-terminated string of panel description
 */
int load_panel_description_line(char* data, struct mipi_dsi_device* dsi, struct generic_panel* ctx) {
    size_t pos;
    for (pos = 0; data[pos] != 0; pos++) {
        if ((data[pos] == '#') || data[pos] == '\n') {
            data[pos] = 0;
            break;
        }
    }
    if (pos < 2) return 0;

    switch (data[0]) {
    case 'G': load_globals(data + 1, dsi, ctx); break;
    case 'M': load_mode(data + 1, dsi, ctx); break;
    case 'I': load_init_seq(data + 1, dsi, ctx); break;
    default: dev_info(NULL, "Unhandled %s\n", data);
    }

    return 0;
}

int panel_description_foreach(struct mipi_dsi_device* dsi, struct generic_panel* ctx,
                              int (*handle_line)(char*, struct mipi_dsi_device*, struct generic_panel*));

int panel_description_foreach(struct mipi_dsi_device* dsi, struct generic_panel* ctx,
                              int (*handle_line)(char*, struct mipi_dsi_device*, struct generic_panel*)) {
    struct device* dev = &dsi->dev;
    const struct firmware* fw;
    int ret;

    if (strlen(descfile) > 0) {
        ret = request_firmware(&fw, descfile, dev);
        if (ret) {
            dev_err(dev, "No config file found (error=%d)\n", ret);
            return -1;
        }

        size_t pos = 0, size = fw->size;
        char* data = (char*)fw->data;

        while (pos < size) {
            while ((pos < size) && (data[pos] != '\n')) pos++;

            if (pos < size) pos++;
            data[pos - 1] = 0;

            handle_line(data, dsi, ctx);

            data = &data[pos];
            size = size - pos;
            pos = 0;
        }

        release_firmware(fw);
    } else {
        // ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
        const char* line;
        char* buf;
        size_t linescnt, i;

        linescnt = of_property_count_strings(dev->of_node, "panel_description");
        if (linescnt < 1) {
            dev_err(dev, "failed to read panel_description from device tree %ld\n", linescnt);
            return -1;
        }

        for (i = 0; i < linescnt; i++) {
            ret = of_property_read_string_index(dev->of_node, "panel_description", i, &line);
            if (ret < 0) {
                dev_err(dev, "failed to read panel_description[%ld] from device tree %d\n", i, ret);
                return -1;
            }
            buf = kstrdup(line, GFP_KERNEL);
            if (!buf) {
                dev_err(dev, "kstrdup(of desc line) failed\n");
                return -1;
            }

            dev_dbg(dev, "desc[%ld]: %s", i, line);
            handle_line(buf, dsi, ctx);
            kfree(buf);
        }
    }
    return 0;
}

int load_panel_description(struct mipi_dsi_device* dsi, struct generic_panel* ctx) {
    int ret;

    ret = panel_description_foreach(dsi, ctx, &load_panel_description_line);
    if (ret < 0) {
        return ret;
    }

    // Reverse iseq
    struct generic_panel_init_seq *rev = ctx->iseq, *fwd = NULL, *tmp = NULL;
    while (rev) {
        tmp = rev;
        rev = tmp->link;
        tmp->link = fwd;
        fwd = tmp;
    }
    ctx->iseq = fwd;

    return 0;
}

static inline struct generic_panel* panel_to_generic_panel(struct drm_panel* panel) {
    return container_of(panel, struct generic_panel, panel);
}

static int generic_panel_init_sequence(struct generic_panel* ctx) {
    struct mipi_dsi_device* dsi = to_mipi_dsi_device(ctx->dev);
    struct device* dev = ctx->dev;
    int ret;

    struct generic_panel_init_seq* iseq = ctx->iseq;
    while (iseq) {
        if (iseq->read > 0) {
            u8 readbuf[8];
            if (iseq->read > 8) {
                iseq->read = 8;
            }
            ret = mipi_dsi_generic_read(dsi, iseq->data, iseq->len, &readbuf[0], iseq->read);
            if (ret < 0) {
                dev_err(ctx->dev, "failed to read: %d\n", ret);
            } else {
                for (int i = 0; i < ret; i++) {
                    dev_info(ctx->dev, "read[%d]: %02x\n", i, readbuf[i]);
                }
            }
        } else if (iseq->dcs == DCS_PSEUDO_CMD_SEQ) {
            ret = mipi_dsi_dcs_write_buffer(dsi, iseq->data, iseq->len);
            dev_dbg(dev, "iseq 0x%px len=%d -> %d\n", (void*)iseq, iseq->len, ret);
        } else {
            ret = mipi_dsi_dcs_write(dsi, iseq->dcs, iseq->data, iseq->len);
            dev_dbg(dev, "iseq %02x len=%d -> %d\n", iseq->dcs, iseq->len, ret);
        }
        if (iseq->wait) {
            msleep(iseq->wait);
        }
        iseq = iseq->link;
    }

    dev_dbg(dev, "Panel init sequence done\n");

    return 0;
}

static int generic_panel_unprepare(struct drm_panel* panel) {
    struct generic_panel* ctx = panel_to_generic_panel(panel);
    struct mipi_dsi_device* dsi = to_mipi_dsi_device(ctx->dev);
    int ret;

    if (!ctx->prepared) return 0;

    /*
     * Both DCS writes below are best-effort: a failure here (DSI bus
     * busy/timeout, which is more likely to happen right as the rest of
     * the pipeline is changing state during suspend) must not abort the
     * power-down that follows. Bailing out here used to leave the panel
     * powered on with ctx->prepared still true, which also made the next
     * prepare() a no-op - so a single failed DCS write during suspend
     * could leave the screen stuck on/corrupted until reboot instead of
     * just for that one suspend cycle.
     */
    ret = mipi_dsi_dcs_set_display_off(dsi);
    if (ret < 0) dev_err(ctx->dev, "failed to set display off: %d\n", ret);

    ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
    if (ret < 0) dev_err(ctx->dev, "failed to enter sleep mode: %d\n", ret);

    generic_panel_power_off(ctx);

    ctx->prepared = false;

    return 0;
}

/* Regulators, enable pin and the reset pulse; the panel is then in its init delay, timed from reset_released. */
static int generic_panel_power_on(struct generic_panel* ctx) {
    int ret;

    mutex_lock(&ctx->power_lock);
    if (ctx->powered) {
        mutex_unlock(&ctx->power_lock);
        return 0;
    }

    ret = regulator_enable(ctx->vdd);
    if (ret < 0) {
        dev_err(ctx->dev, "Failed to enable vdd supply: %d\n", ret);
        goto out;
    }

    ret = regulator_enable(ctx->iovcc);
    if (ret < 0) {
        dev_err(ctx->dev, "Failed to enable iovcc supply: %d\n", ret);
        regulator_disable(ctx->vdd);
        goto out;
    }

    if (ctx->enable_gpio) {
        gpiod_set_value_cansleep(ctx->enable_gpio, 1);
    }
    msleep(ctx->delays.prepare);

    gpiod_set_value_cansleep(ctx->reset_gpio, 1);
    msleep(ctx->delays.reset);
    gpiod_set_value_cansleep(ctx->reset_gpio, 0);
    ctx->reset_released = ktime_get();
    ctx->powered = true;
out:
    mutex_unlock(&ctx->power_lock);
    return ret;
}

static void generic_panel_power_off(struct generic_panel* ctx) {
    mutex_lock(&ctx->power_lock);
    if (ctx->powered) {
        if (ctx->enable_gpio) {
            gpiod_set_value_cansleep(ctx->enable_gpio, 0);
        }
        gpiod_set_value_cansleep(ctx->reset_gpio, 1);
        regulator_disable(ctx->iovcc);
        regulator_disable(ctx->vdd);
        ctx->powered = false;
    }
    mutex_unlock(&ctx->power_lock);
}

static void generic_panel_power_work(struct work_struct* work) {
    struct generic_panel* ctx = container_of(work, struct generic_panel, power_work);

    generic_panel_power_on(ctx);
}

/* Sleep out the rest of a delay that started at `since`. */
static void generic_panel_sleep_until(ktime_t since, int delay_ms) {
    s64 left = delay_ms - ktime_ms_delta(ktime_get(), since);

    if (left > 0) msleep((unsigned int)left);
}

static void generic_panel_backlight_work(struct work_struct* work) {
    struct generic_panel* ctx = container_of(to_delayed_work(work), struct generic_panel, backlight_work);

    backlight_enable(ctx->backlight);
}

static int generic_panel_prepare(struct drm_panel* panel) {
    struct generic_panel* ctx = panel_to_generic_panel(panel);
    struct mipi_dsi_device* dsi = to_mipi_dsi_device(ctx->dev);
    int ret;

    if (ctx->prepared) {
        return 0;
    }

    flush_work(&ctx->power_work);
    ret = generic_panel_power_on(ctx);
    if (ret < 0) return ret;

    generic_panel_sleep_until(ctx->reset_released, ctx->delays.init);

    ret = generic_panel_init_sequence(ctx);
    if (ret < 0) {
        dev_err(ctx->dev, "Panel init sequence failed: %d\n", ret);
        goto power_off;
    }

    ret = mipi_dsi_dcs_set_display_on(dsi);
    if (ret < 0) {
        dev_err(ctx->dev, "Failed to set display on: %d\n", ret);
        goto power_off;
    }

    ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
    if (ret < 0) {
        dev_err(ctx->dev, "Failed to exit sleep mode: %d\n", ret);
        goto power_off;
    }

    ctx->ready_at = ktime_add_ms(ktime_get(), ctx->delays.enable);
    if (!ctx->backlight) msleep(ctx->delays.enable);

    // msleep(ctx->delays.ready);

    ctx->prepared = true;

    return 0;

power_off:
    generic_panel_power_off(ctx);
    return ret;
}

static int generic_panel_enable(struct drm_panel* panel) {
    struct generic_panel* ctx = panel_to_generic_panel(panel);
    s64 left;

    if (!ctx->backlight) return 0;
    left = ktime_ms_delta(ctx->ready_at, ktime_get());
    if (left < 0) left = 0;
    schedule_delayed_work(&ctx->backlight_work, msecs_to_jiffies((unsigned int)left));
    return 0;
}

static int generic_panel_disable(struct drm_panel* panel) {
    struct generic_panel* ctx = panel_to_generic_panel(panel);

    if (!ctx->backlight) return 0;
    cancel_delayed_work_sync(&ctx->backlight_work);
    backlight_disable(ctx->backlight);
    return 0;
}

/* drm_display_mode template without clock as it is variable */
static const struct drm_display_mode mode_template = {};

static int generic_panel_get_modes(struct drm_panel* panel, struct drm_connector* connector) {
    struct generic_panel* ctx = panel_to_generic_panel(panel);
    struct drm_display_mode mode_tmp;
    struct drm_display_mode* mode;
    struct generic_panel_mode* genmode = ctx->modes;

    while (genmode) {
        dev_dbg(ctx->dev, "gen mode %d %dx%d\n", genmode->clock, genmode->horizontal[1], genmode->vertical[1]);

        mode_tmp = mode_template;

        mode_tmp.clock = genmode->clock;

        mode_tmp.hdisplay = genmode->horizontal[1];
        mode_tmp.hsync_start = mode_tmp.hdisplay + genmode->horizontal[2];
        mode_tmp.hsync_end = mode_tmp.hsync_start + genmode->horizontal[3];
        mode_tmp.htotal = mode_tmp.hsync_end + genmode->horizontal[4];

        mode_tmp.vdisplay = genmode->vertical[1];
        mode_tmp.vsync_start = mode_tmp.vdisplay + genmode->vertical[2];
        mode_tmp.vsync_end = mode_tmp.vsync_start + genmode->vertical[3];
        mode_tmp.vtotal = mode_tmp.vsync_end + genmode->vertical[4];

        mode_tmp.width_mm = ctx->size.width;
        mode_tmp.height_mm = ctx->size.height;

        // All tested panels (R36s orig, new-3, new-4; rgb10s) either require no sync or work OK with it.
        // So for now just always set no sync drm flags
        // TODO: pass drm flags in mode line if some future panel requires that
        mode_tmp.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;

        mode = drm_mode_duplicate(connector->dev, &mode_tmp);
        if (!mode) {
            dev_err(ctx->dev, "Failed to add mode %u\n", drm_mode_vrefresh(mode));
            return -ENOMEM;
        }
        drm_mode_set_name(mode);

        mode->type = DRM_MODE_TYPE_DRIVER;
        if (genmode->is_default) {
            mode->type |= DRM_MODE_TYPE_PREFERRED;
        };

        drm_mode_probed_add(connector, mode);

        genmode = genmode->prev;
    }

    connector->display_info.width_mm = ctx->size.width;
    connector->display_info.height_mm = ctx->size.height;

    /*
     * TODO: Remove once all drm drivers call
     * drm_connector_set_orientation_from_panel()
     */
    drm_connector_set_panel_orientation(connector, ctx->orientation);

    return 1;
}

static enum drm_panel_orientation generic_panel_get_orientation(struct drm_panel* panel) {
    struct generic_panel* ctx = panel_to_generic_panel(panel);

    return ctx->orientation;
}

static const struct drm_panel_funcs generic_panel_funcs = {
    .unprepare = generic_panel_unprepare,
    .prepare = generic_panel_prepare,
    .enable = generic_panel_enable,
    .disable = generic_panel_disable,
    .get_modes = generic_panel_get_modes,
    .get_orientation = generic_panel_get_orientation,
};

static int generic_panel_probe(struct mipi_dsi_device* dsi) {
    struct device* dev = &dsi->dev;
    struct generic_panel* ctx;
    int ret;

    // Some defaults
    dsi->lanes = 1;
    dsi->format = MIPI_DSI_FMT_RGB888;
    dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_LPM |
                      MIPI_DSI_MODE_NO_EOT_PACKET | MIPI_DSI_CLOCK_NON_CONTINUOUS;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(7, 2, 0))
    ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);

    mipi_dsi_set_drvdata(dsi, ctx);

    ctx->dev = dev;

    ret = load_panel_description(dsi, ctx);
    if (ret < 0) {
        dev_err(dev, "Failed to load panel description\n");
        return ret;
    }

    mipi_dsi_set_drvdata(dsi, ctx);

    dev_info(dev, "lanes %d, format %d, mode %lx\n", dsi->lanes, dsi->format, dsi->mode_flags);

    drm_panel_init(&ctx->panel, &dsi->dev, &generic_panel_funcs, DRM_MODE_CONNECTOR_DSI);
#else
    ctx = devm_drm_panel_alloc(&dsi->dev, __typeof(*ctx), panel, &generic_panel_funcs, DRM_MODE_CONNECTOR_DSI);

    if (IS_ERR(ctx)) return PTR_ERR(ctx);

    ret = load_panel_description(dsi, ctx);
    if (ret < 0) {
        dev_err(dev, "Failed to load panel description\n");
        return ret;
    }

    mipi_dsi_set_drvdata(dsi, ctx);

    ctx->dev = dev;
    dev_info(dev, "lanes %d, format %d, mode %lx\n", dsi->lanes, dsi->format, dsi->mode_flags);
#endif

    if (!ctx) return -ENOMEM;

    ctx->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
    if (IS_ERR(ctx->enable_gpio)) {
        dev_err(dev, "cannot get enable gpio\n");
        ctx->enable_gpio = NULL;
    }

    ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(ctx->reset_gpio)) {
        dev_err(dev, "cannot get reset gpio\n");
        return PTR_ERR(ctx->reset_gpio);
    }

    ctx->vdd = devm_regulator_get(dev, "vdd");
    if (IS_ERR(ctx->vdd)) {
        ret = PTR_ERR(ctx->vdd);
        if (ret != -EPROBE_DEFER) dev_err(dev, "Failed to request vdd regulator: %d\n", ret);
        return ret;
    }

    ctx->iovcc = devm_regulator_get(dev, "iovcc");
    if (IS_ERR(ctx->iovcc)) {
        ret = PTR_ERR(ctx->iovcc);
        if (ret != -EPROBE_DEFER) dev_err(dev, "Failed to request iovcc regulator: %d\n", ret);
        return ret;
    }

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0))
    ret = drm_of_get_panel_orientation(dev->of_node, &ctx->orientation);
#else
    ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
#endif
    if (ret < 0) {
        dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, ret);
        return ret;
    }

    mutex_init(&ctx->power_lock);
    INIT_WORK(&ctx->power_work, generic_panel_power_work);
    INIT_DELAYED_WORK(&ctx->backlight_work, generic_panel_backlight_work);

    if (early_power) {
        /* Our own backlight handle, so the panel core's enable() does not switch it on before the deadline. */
        ctx->backlight = devm_of_find_backlight(dev);
        if (IS_ERR(ctx->backlight)) return PTR_ERR(ctx->backlight);
    }
    if (!ctx->backlight) {
        ret = drm_panel_of_backlight(&ctx->panel);
        if (ret) return ret;
    }

    drm_panel_add(&ctx->panel);

    ret = mipi_dsi_attach(dsi);
    if (ret < 0) {
        dev_err(dev, "mipi_dsi_attach failed: %d\n", ret);
        drm_panel_remove(&ctx->panel);
        return ret;
    }

    if (early_power) schedule_work(&ctx->power_work);

    return 0;
}

static void generic_panel_shutdown(struct mipi_dsi_device* dsi) {
    struct generic_panel* ctx = mipi_dsi_get_drvdata(dsi);

    cancel_work_sync(&ctx->power_work);
    drm_panel_disable(&ctx->panel);
    drm_panel_unprepare(&ctx->panel);
    generic_panel_power_off(ctx);
}

static void generic_panel_remove(struct mipi_dsi_device* dsi) {
    struct generic_panel* ctx = mipi_dsi_get_drvdata(dsi);
    int ret;

    generic_panel_shutdown(dsi);
    cancel_work_sync(&ctx->power_work);
    cancel_delayed_work_sync(&ctx->backlight_work);
    generic_panel_power_off(ctx);

    ret = mipi_dsi_detach(dsi);
    if (ret < 0) dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

    drm_panel_remove(&ctx->panel);
}

static const struct of_device_id generic_panel_of_match[] = {{.compatible = "rocknix,generic-dsi"}, {/* sentinel */}};
MODULE_DEVICE_TABLE(of, generic_panel_of_match);

static struct mipi_dsi_driver generic_panel_driver = {
    .driver =
        {
            .name = DRIVER_NAME,
            .of_match_table = generic_panel_of_match,
        },
    .probe = generic_panel_probe,
    .remove = generic_panel_remove,
    .shutdown = generic_panel_shutdown,
};
module_mipi_dsi_driver(generic_panel_driver);

MODULE_AUTHOR("Danil Zagoskin <z@gosk.in>");
MODULE_DESCRIPTION("DRM driver for generic MIPI DSI panel");
MODULE_LICENSE("GPL v2");
