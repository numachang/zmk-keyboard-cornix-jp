/*
 * Copyright (c) 2026 numachang
 * SPDX-License-Identifier: MIT
 *
 * Stage B6: shared LED strip worker + EXT_POWER auto-off.
 *
 * Owns the led_strip device and the only thread that touches it. Pulls
 * cornix_rgb_msg off a message queue and renders the requested blink on
 * the requested pixel. Per-half listeners (central.c / peripheral.c /
 * charging.c) translate ZMK events into messages and enqueue them.
 *
 * EXT_POWER is gated for power saving (#15): WS2812 idle current is
 * non-negligible, so we cut its supply when no message has arrived for
 * EXT_POWER_IDLE_TIMEOUT_MS. The next message restores power, waits a
 * short settle, and renders.
 *
 * The idle timeout must exceed the longest continuous-blink period
 * (B3 peer-lost = 1 s, B4 charging = 2 s, B5 BT search = 1 s) so that
 * those workers' own self-rescheduling keeps the LED powered for the
 * duration of the indication. 15 s leaves comfortable headroom.
 *
 * EXT_POWER rail assumption: this fork's Cornix wiring puts the WS2812
 * strip alone on the zmk,ext-power-generic rail (P0.13 left / P0.24
 * right). If a future shield adds another consumer (e.g. nice!view) to
 * the same rail it would also be cut at idle — bring it up on its own
 * rail or wrap it in a separate gating mechanism.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/ext_power.h>

#include <cornix_rgb_indicator/widget.h>

LOG_MODULE_REGISTER(cornix_rgb, CONFIG_ZMK_LOG_LEVEL);

#define EXT_POWER_IDLE_TIMEOUT_MS 15000
#define EXT_POWER_SETTLE_MS       10

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

K_MSGQ_DEFINE(cornix_rgb_msgq, sizeof(struct cornix_rgb_msg), 16, 4);

static struct led_rgb pixels[STRIP_NUM_PIXELS];

void cornix_rgb_queue(const struct cornix_rgb_msg *msg) {
    if (k_msgq_put(&cornix_rgb_msgq, msg, K_NO_WAIT) != 0) {
        LOG_WRN("cornix_rgb: msgq full, dropping msg for pixel %u",
                msg->pixel_index);
    }
}

void cornix_rgb_show_once(uint8_t idx, struct led_rgb color, uint16_t on_ms) {
    struct cornix_rgb_msg msg = {
        .pixel_index = idx,
        .color = color,
        .blink_count = 1,
        .on_ms = on_ms,
        .off_ms = 0,
    };
    cornix_rgb_queue(&msg);
}

void cornix_rgb_blink(uint8_t idx, struct led_rgb color, uint8_t count,
                      uint16_t on_ms, uint16_t off_ms) {
    struct cornix_rgb_msg msg = {
        .pixel_index = idx,
        .color = color,
        .blink_count = count ? count : 1,
        .on_ms = on_ms,
        .off_ms = off_ms,
    };
    cornix_rgb_queue(&msg);
}

static void write_pixel(uint8_t idx, struct led_rgb color) {
    if (idx >= STRIP_NUM_PIXELS) {
        return;
    }
    pixels[idx] = color;
    led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
}

#if HAVE_EXT_POWER
static struct k_work_delayable ext_power_off_work;

static void ext_power_off_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!device_is_ready(ext_power)) {
        return;
    }
    int rc = ext_power_disable(ext_power);
    LOG_INF("cornix_rgb: idle timeout, ext_power OFF (rc=%d)", rc);
}

static int ext_power_off_init(void) {
    k_work_init_delayable(&ext_power_off_work, ext_power_off_handler);
    return 0;
}
SYS_INIT(ext_power_off_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static void ensure_ext_power_on(void) {
    if (!device_is_ready(ext_power)) {
        return;
    }
    int state = ext_power_get(ext_power);
    if (state == 0) {
        ext_power_enable(ext_power);
        /* Give the strip a moment to come back from cold before the
         * first led_strip_update_rgb of the new burst. */
        k_sleep(K_MSEC(EXT_POWER_SETTLE_MS));
        LOG_INF("cornix_rgb: ext_power ON");
    }
}
#endif

static void cornix_rgb_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    if (!device_is_ready(strip)) {
        LOG_ERR("cornix_rgb: led_strip device not ready");
        return;
    }

    LOG_INF("cornix_rgb worker started, %d pixel(s)", STRIP_NUM_PIXELS);

    /* EXT_POWER is left OFF at boot. The first message will turn it on
     * via ensure_ext_power_on(); after EXT_POWER_IDLE_TIMEOUT_MS of
     * silence the off-work will turn it off again. */

    while (true) {
        struct cornix_rgb_msg msg;
        k_msgq_get(&cornix_rgb_msgq, &msg, K_FOREVER);

#if HAVE_EXT_POWER
        k_work_cancel_delayable(&ext_power_off_work);
        ensure_ext_power_on();
#endif

        for (uint8_t i = 0; i < msg.blink_count; i++) {
            write_pixel(msg.pixel_index, msg.color);
            k_sleep(K_MSEC(msg.on_ms));

            write_pixel(msg.pixel_index, (struct led_rgb){0});
            if (i + 1 < msg.blink_count) {
                k_sleep(K_MSEC(msg.off_ms));
            }
        }

#if HAVE_EXT_POWER
        /* Re-arm the off timer only when the queue has gone quiet.
         * Continuous-blink workers (B3/B4/B5) self-reschedule faster
         * than the timeout, so the timer keeps getting cancelled while
         * an indication is in progress. */
        k_work_reschedule(&ext_power_off_work,
                          K_MSEC(EXT_POWER_IDLE_TIMEOUT_MS));
#endif
    }
}

K_THREAD_DEFINE(cornix_rgb_tid, 1024, cornix_rgb_thread_fn,
                NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT)
/* central.c / peripheral.c both guard themselves on CONFIG_ZMK_SPLIT.
 * Provide a no-op fallback so charging.c (which calls this
 * unconditionally) still links on a non-split build. Cornix always
 * builds as split, but keeping the symmetry costs nothing. */
bool cornix_rgb_peer_blink_active(void) {
    return false;
}
#endif
