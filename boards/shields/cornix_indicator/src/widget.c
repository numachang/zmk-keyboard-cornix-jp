/*
 * Copyright (c) 2026 numachang
 * SPDX-License-Identifier: MIT
 *
 * Stage B6 + #18: per-pixel strip workers with shared EXT_POWER gating.
 *
 * Listeners (central.c / peripheral.c / charging.c) push cornix_rgb_msg
 * onto a single public queue via cornix_rgb_queue(); the queue
 * dispatches per pixel_index to a dedicated worker thread, so a long
 * steady on one pixel (e.g. 3 s peer-link-up on pixel[0]) does not
 * block events targeting the other pixel (e.g. BT search blink on
 * pixel[1]).
 *
 * Only the LED strip device + the pixels[] buffer are shared, and
 * those are protected by strip_mutex held for the duration of a
 * write_pixel() call only. The on_ms / off_ms sleeps run outside the
 * mutex so both workers can sleep concurrently.
 *
 * EXT_POWER is gated for power saving (#15): the rail is left OFF
 * unless at least one worker is currently rendering, and an off-work
 * disables the rail EXT_POWER_IDLE_TIMEOUT_MS after the last worker
 * returns to msg-wait. An atomic active-worker count handles the
 * cancel/re-arm symmetrically across both threads.
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
#include <zephyr/sys/atomic.h>

#include <drivers/ext_power.h>

#include <cornix_rgb_indicator/widget.h>

LOG_MODULE_REGISTER(cornix_rgb, CONFIG_ZMK_LOG_LEVEL);

#define EXT_POWER_IDLE_TIMEOUT_MS 15000
#define EXT_POWER_SETTLE_MS       10

#define STRIP_NODE       DT_ALIAS(status_ws2812)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

BUILD_ASSERT(DT_NODE_HAS_STATUS(STRIP_NODE, okay),
             "cornix_rgb: status-ws2812 alias must point at an okay node");
BUILD_ASSERT(STRIP_NUM_PIXELS == 2,
             "cornix_rgb: per-pixel workers are hard-wired to chain-length=2");

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
#define HAVE_EXT_POWER 1
static const struct device *const ext_power =
    DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#else
#define HAVE_EXT_POWER 0
#endif

/* One queue per pixel index keeps a long render on one pixel from
 * delaying messages targeted at the other. cornix_rgb_queue() routes
 * by msg.pixel_index. Per-queue depth of 8 is plenty: only the
 * continuous-blink workers (B3/B4/B5) self-enqueue, and they do so
 * once per cycle (>= 1 s), well behind the worker drain rate. */
K_MSGQ_DEFINE(cornix_rgb_msgq_p0, sizeof(struct cornix_rgb_msg), 8, 4);
K_MSGQ_DEFINE(cornix_rgb_msgq_p1, sizeof(struct cornix_rgb_msg), 8, 4);

/* pixels[] and the led_strip device are touched from both worker
 * threads. strip_mutex is held only for the brief window of the
 * actual write — the on_ms / off_ms sleeps run outside it so both
 * workers can sleep concurrently. */
K_MUTEX_DEFINE(strip_mutex);
static struct led_rgb pixels[STRIP_NUM_PIXELS];

void cornix_rgb_queue(const struct cornix_rgb_msg *msg) {
    struct k_msgq *target =
        (msg->pixel_index == 0) ? &cornix_rgb_msgq_p0 : &cornix_rgb_msgq_p1;
    if (k_msgq_put(target, msg, K_NO_WAIT) != 0) {
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

void cornix_rgb_set(uint8_t idx, struct led_rgb color) {
    /* on_ms == 0 && off_ms == 0 is treated as "set and return" by the
     * worker — see the dispatch in cornix_rgb_worker_fn. */
    struct cornix_rgb_msg msg = {
        .pixel_index = idx,
        .color = color,
        .blink_count = 1,
        .on_ms = 0,
        .off_ms = 0,
    };
    cornix_rgb_queue(&msg);
}

static void write_pixel_locked(uint8_t idx, struct led_rgb color) {
    if (idx >= STRIP_NUM_PIXELS) {
        return;
    }
    k_mutex_lock(&strip_mutex, K_FOREVER);
    pixels[idx] = color;
    led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
    k_mutex_unlock(&strip_mutex);
}

#if HAVE_EXT_POWER
static struct k_work_delayable ext_power_off_work;
/* Number of worker threads currently rendering (between activity_start
 * and activity_end). When 0, the off-work is armed; when it goes 0->1
 * the off-work is cancelled and EXT_POWER is brought up. */
static atomic_t active_workers = ATOMIC_INIT(0);

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

static void activity_start(void) {
    if (atomic_inc(&active_workers) == 0) {
        /* 0 -> 1 transition: this is the first worker becoming active.
         * Cancel any pending off-work and make sure power is up before
         * we let the worker write to the strip. */
        k_work_cancel_delayable(&ext_power_off_work);
        k_mutex_lock(&strip_mutex, K_FOREVER);
        ensure_ext_power_on();
        k_mutex_unlock(&strip_mutex);
    }
}

static void activity_end(void) {
    if (atomic_dec(&active_workers) == 1) {
        /* 1 -> 0 transition: last active worker just finished. Arm
         * the off-work; any new msg that arrives within the timeout
         * will cancel and re-enable in activity_start above. */
        k_work_reschedule(&ext_power_off_work,
                          K_MSEC(EXT_POWER_IDLE_TIMEOUT_MS));
    }
}
#else
static inline void activity_start(void) {}
static inline void activity_end(void) {}
#endif

static void cornix_rgb_worker_fn(void *p1, void *p2, void *p3) {
    int my_idx = (int)(intptr_t)p1;
    struct k_msgq *my_msgq = p2;
    ARG_UNUSED(p3);

    if (!device_is_ready(strip)) {
        LOG_ERR("cornix_rgb worker[%d]: led_strip not ready", my_idx);
        return;
    }
    LOG_INF("cornix_rgb worker[%d] started", my_idx);

    while (true) {
        struct cornix_rgb_msg msg;
        k_msgq_get(my_msgq, &msg, K_FOREVER);

        /* Defensive: cornix_rgb_queue() already routes by pixel_index,
         * but if something went wrong upstream we'd rather drop the
         * misrouted message than let one worker scribble on the other
         * pixel's slot. */
        if (msg.pixel_index != my_idx) {
            LOG_ERR("worker[%d] got msg for pixel %u — dropping",
                    my_idx, msg.pixel_index);
            continue;
        }

        activity_start();

        if (msg.on_ms == 0 && msg.off_ms == 0) {
            /* "set mode": write the colour and leave it on the strip.
             * Used by the breathing animation which streams continuous
             * colour updates. activity_end re-arms the idle off-work,
             * but the next breathing tick (~33 ms) cancels it again,
             * so the strip stays powered for the whole animation. */
            write_pixel_locked(my_idx, msg.color);
        } else {
            for (uint8_t i = 0; i < msg.blink_count; i++) {
                write_pixel_locked(my_idx, msg.color);
                k_sleep(K_MSEC(msg.on_ms));

                write_pixel_locked(my_idx, (struct led_rgb){0});
                if (i + 1 < msg.blink_count) {
                    k_sleep(K_MSEC(msg.off_ms));
                }
            }
        }

        activity_end();
    }
}

K_THREAD_DEFINE(cornix_rgb_tid_p0, 1024, cornix_rgb_worker_fn,
                (void *)(intptr_t)0, &cornix_rgb_msgq_p0, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
K_THREAD_DEFINE(cornix_rgb_tid_p1, 1024, cornix_rgb_worker_fn,
                (void *)(intptr_t)1, &cornix_rgb_msgq_p1, NULL,
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
