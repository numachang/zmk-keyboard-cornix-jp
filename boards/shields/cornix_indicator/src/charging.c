/*
 * Copyright (c) 2026 numachang
 * SPDX-License-Identifier: MIT
 *
 * Stage B4 (both halves): charging indicator on pixel[0] (inner).
 *
 * Each half has its own USB-C port. ZMK's zmk_usb_is_powered() is true
 * iff VBUS is present on this half. We do not need a charge-IC STAT GPIO
 * to follow the manual reasonably well:
 *
 *   USB powered, battery < FULL_THRESHOLD  -> green slow blink (charging)
 *   USB powered, battery >= FULL_THRESHOLD -> green 3 s steady, then off
 *                                             (shown once per full-charge)
 *   USB not powered                        -> indicator off
 *
 * Same logic runs on both central and peripheral so each half indicates
 * its own charge state regardless of which one has the cable plugged.
 * This matches the Cornix RMK manual where both halves have a "battery /
 * charging" LED on the inner LED of each side.
 *
 * Priority: when the peer-lost slow blink is active, we skip our own
 * green blink for that cycle so the blue peer-lost indication remains
 * visible. Battery-low one-shot red blinks just interleave through the
 * shared strip msgq.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

#include <cornix_rgb_indicator/widget.h>

LOG_MODULE_DECLARE(cornix_rgb, CONFIG_ZMK_LOG_LEVEL);

#define CHARGING_FULL_THRESHOLD  95
/* Charging blink is intentionally slower / lower duty than the B3 / B5
 * blue and BT-search blinks. Green is at the peak of human eye
 * sensitivity and felt "flickery" at the same 1 Hz / duty 40% cadence
 * the other indicators use. 0.5 Hz / duty 20% reads as a calm "charging"
 * sign without distracting. See discussion on #2. */
#define CHARGING_BLINK_ON_MS     400
#define CHARGING_BLINK_PERIOD_MS 2000
#define FULL_SHOW_MS             3000

static const struct led_rgb COL_GREEN = {.g = CORNIX_RGB_LEVEL};

static bool usb_powered = false;
static uint8_t latest_battery; /* 0 = unknown */
static bool full_already_shown;

static struct k_work_delayable charging_blink_work;

static void charging_blink_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!usb_powered) {
        return;
    }

    /* Once the battery is full, show the steady "fully charged" pulse
     * exactly once and then go quiet. The flag is cleared on USB
     * disconnect so unplug-replug after a full charge starts the cycle
     * over. */
    if (latest_battery > 0 && latest_battery >= CHARGING_FULL_THRESHOLD) {
        if (!full_already_shown) {
            LOG_INF("charging: fully charged (%u%%), steady green", latest_battery);
            cornix_rgb_show_once(CORNIX_RGB_PIX_INNER, COL_GREEN, FULL_SHOW_MS);
            full_already_shown = true;
        }
        return;
    }

    /* Charging: emit one slow-blink pulse, then reschedule. Skip this
     * cycle if peer-lost is currently blinking blue on the same pixel. */
    if (!cornix_rgb_peer_blink_active()) {
        cornix_rgb_blink(CORNIX_RGB_PIX_INNER, COL_GREEN, 1,
                         CHARGING_BLINK_ON_MS, 0);
    }
    k_work_reschedule(&charging_blink_work,
                      K_MSEC(CHARGING_BLINK_PERIOD_MS));
}

static void start_charging_blink(void) {
    full_already_shown = false;
    k_work_reschedule(&charging_blink_work, K_NO_WAIT);
}

static void stop_charging_blink(void) {
    k_work_cancel_delayable(&charging_blink_work);
}

static int on_usb_changed(const zmk_event_t *eh) {
    ARG_UNUSED(eh);

    bool now_powered = zmk_usb_is_powered();
    if (now_powered == usb_powered) {
        return 0;
    }
    usb_powered = now_powered;
    LOG_INF("charging: usb_powered=%d", usb_powered);

    if (usb_powered) {
        start_charging_blink();
    } else {
        stop_charging_blink();
        full_already_shown = false;
    }
    return 0;
}

ZMK_LISTENER(cornix_rgb_charging_usb, on_usb_changed);
ZMK_SUBSCRIPTION(cornix_rgb_charging_usb, zmk_usb_conn_state_changed);

static int on_battery_for_charging(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev == NULL || ev->state_of_charge == 0) {
        return 0;
    }
    latest_battery = ev->state_of_charge;

    /* Crossing the full threshold upward: nothing to do — the next
     * scheduled charging_blink_handler fires the steady "full" pulse.
     *
     * Crossing the threshold downward while we are still on USB power
     * AND have already shown "full" once: revert to charging mode so
     * the indicator follows reality (e.g. heavy load drew the cell
     * back below 95% even though USB is still attached). Without this,
     * the green pulse would only resume after a physical unplug. */
    if (usb_powered && full_already_shown &&
        ev->state_of_charge < CHARGING_FULL_THRESHOLD) {
        LOG_INF("charging: fell back below %u%% (now %u%%), resume blink",
                CHARGING_FULL_THRESHOLD, ev->state_of_charge);
        full_already_shown = false;
        k_work_reschedule(&charging_blink_work, K_NO_WAIT);
    }
    return 0;
}

ZMK_LISTENER(cornix_rgb_charging_bat, on_battery_for_charging);
ZMK_SUBSCRIPTION(cornix_rgb_charging_bat, zmk_battery_state_changed);

static int charging_init(void) {
    k_work_init_delayable(&charging_blink_work, charging_blink_handler);

    /* If USB is already powered at boot (e.g. plugged in before power-on),
     * start the indicator without waiting for the conn-state event. */
    if (zmk_usb_is_powered()) {
        usb_powered = true;
        start_charging_blink();
    }
    return 0;
}
SYS_INIT(charging_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
