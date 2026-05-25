/*
 * Copyright (c) 2026 numachang
 * SPDX-License-Identifier: MIT
 *
 * Stage B1b: boot-time probe sequence to pin down both
 *   (a) which physical LED is pixel[0] vs pixel[1] on each half, and
 *   (b) which led_rgb field maps to which physical colour channel
 *       (i.e. whether the DT color-mapping matches the hardware).
 *
 * For each pixel index in turn, push dim red, then dim green, then dim
 * blue, with short gaps between flashes and a longer gap between the
 * two indices. Total runtime ~6 s, only at boot. Per-half / event-driven
 * behaviour comes in B2+ once this mapping is confirmed.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/ext_power.h>

LOG_MODULE_REGISTER(cornix_rgb, CONFIG_ZMK_LOG_LEVEL);

#define STRIP_NODE       DT_ALIAS(status_ws2812)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

BUILD_ASSERT(DT_NODE_HAS_STATUS(STRIP_NODE, okay),
             "cornix_rgb: status-ws2812 alias must point at an okay node");

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
#define HAVE_EXT_POWER 1
static const struct device *const ext_power =
    DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#else
#define HAVE_EXT_POWER 0
#endif

#define PROBE_ON_MS    700
#define PROBE_OFF_MS   200
#define PROBE_GAP_MS   800
#define PROBE_LEVEL    0x10

static struct led_rgb pixels[STRIP_NUM_PIXELS];

static void probe_show(int idx, struct led_rgb color, const char *label) {
    memset(pixels, 0, sizeof(pixels));
    pixels[idx] = color;
    int rc = led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
    LOG_INF("probe pixel[%d] %s (rc=%d)", idx, label, rc);
    k_sleep(K_MSEC(PROBE_ON_MS));

    memset(pixels, 0, sizeof(pixels));
    led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
    k_sleep(K_MSEC(PROBE_OFF_MS));
}

static void cornix_rgb_boot_probe(void) {
#if HAVE_EXT_POWER
    if (device_is_ready(ext_power)) {
        ext_power_enable(ext_power);
    }
#endif

    for (int idx = 0; idx < STRIP_NUM_PIXELS; idx++) {
        probe_show(idx, (struct led_rgb){.r = PROBE_LEVEL}, ".r");
        probe_show(idx, (struct led_rgb){.g = PROBE_LEVEL}, ".g");
        probe_show(idx, (struct led_rgb){.b = PROBE_LEVEL}, ".b");
        if (idx + 1 < STRIP_NUM_PIXELS) {
            k_sleep(K_MSEC(PROBE_GAP_MS));
        }
    }

#if HAVE_EXT_POWER
    if (device_is_ready(ext_power)) {
        ext_power_disable(ext_power);
    }
#endif
}

static void cornix_rgb_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    if (!device_is_ready(strip)) {
        LOG_ERR("cornix_rgb: led_strip device not ready");
        return;
    }

    k_sleep(K_MSEC(500));
    cornix_rgb_boot_probe();
}

K_THREAD_DEFINE(cornix_rgb_tid, 1024, cornix_rgb_thread_fn,
                NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
