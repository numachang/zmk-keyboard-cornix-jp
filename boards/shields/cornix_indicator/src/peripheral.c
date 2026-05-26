/*
 * Copyright (c) 2026 numachang
 * SPDX-License-Identifier: MIT
 *
 * Stage B3 (peripheral / right half):
 *   pixel[0] (inner): red 2x blink on self battery low.
 *   pixel[1] (outer): blue 3 s steady when the link to central comes up;
 *                     while the link is lost, blue blink at 1 Hz /
 *                     duty 40% for up to 10 cycles or until reconnection.
 *
 * Charging colours come in B4.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>

#include <cornix_rgb_indicator/widget.h>

LOG_MODULE_DECLARE(cornix_rgb, CONFIG_ZMK_LOG_LEVEL);

#define BATTERY_LEVEL_LOW 20

static const struct led_rgb COL_RED  = {.r = CORNIX_RGB_LEVEL};
static const struct led_rgb COL_BLUE = {.b = CORNIX_RGB_LEVEL};

#define CENTRAL_ON_MS 3000

static struct k_work_delayable peer_blink_work;
/* Touched from the system workqueue (handler), the event manager
 * (on_central_status), and the strip worker (via
 * cornix_rgb_peer_blink_active). Atomic so races between cancel/arm
 * and a mid-tick handler do not leak "still active" past timeout. */
static atomic_t peer_blink_remaining;

static void peer_blink_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (atomic_get(&peer_blink_remaining) <= 0) {
        return;
    }
    cornix_rgb_blink(CORNIX_RGB_PIX_OUTER, COL_BLUE, 1,
                     CORNIX_PEER_BLINK_ON_MS, 0);
    atomic_val_t before = atomic_dec(&peer_blink_remaining);
    if (before > 1) {
        k_work_reschedule(&peer_blink_work,
                          K_MSEC(CORNIX_PEER_BLINK_PERIOD_MS));
    } else {
        atomic_set(&peer_blink_remaining, 0);
        LOG_INF("peripheral: central-lost blink timed out");
    }
}

static int peer_blink_init(void) {
    k_work_init_delayable(&peer_blink_work, peer_blink_handler);
    return 0;
}
SYS_INIT(peer_blink_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int on_central_status(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *ev =
        as_zmk_split_peripheral_status_changed(eh);
    if (ev == NULL) {
        return 0;
    }
    LOG_INF("peripheral: central %s",
            ev->connected ? "connected" : "disconnected");
    if (ev->connected) {
        atomic_set(&peer_blink_remaining, 0);
        k_work_cancel_delayable(&peer_blink_work);
        cornix_rgb_show_once(CORNIX_RGB_PIX_OUTER, COL_BLUE, CENTRAL_ON_MS);
    } else {
        atomic_set(&peer_blink_remaining, CORNIX_PEER_BLINK_MAX_COUNT);
        k_work_reschedule(&peer_blink_work, K_NO_WAIT);
    }
    return 0;
}

ZMK_LISTENER(cornix_rgb_periph_peer, on_central_status);
ZMK_SUBSCRIPTION(cornix_rgb_periph_peer, zmk_split_peripheral_status_changed);

bool cornix_rgb_peer_blink_active(void) {
    return atomic_get(&peer_blink_remaining) > 0;
}

static int on_battery_changed(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev == NULL || ev->state_of_charge == 0) {
        return 0;
    }
    if (ev->state_of_charge <= BATTERY_LEVEL_LOW) {
        LOG_INF("peripheral: battery low %u%%", ev->state_of_charge);
        cornix_rgb_blink(CORNIX_RGB_PIX_INNER, COL_RED, 2, 400, 400);
    }
    return 0;
}

ZMK_LISTENER(cornix_rgb_periph_bat, on_battery_changed);
ZMK_SUBSCRIPTION(cornix_rgb_periph_bat, zmk_battery_state_changed);

#endif /* CONFIG_ZMK_SPLIT && !CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
