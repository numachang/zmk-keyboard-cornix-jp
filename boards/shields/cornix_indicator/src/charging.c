/*
 * Copyright (c) 2026 numachang
 * SPDX-License-Identifier: MIT
 *
 * Stage B4 + #16 (both halves): charging indicator on pixel[0] (inner).
 *
 * Each half has its own USB-C port. zmk_usb_is_powered() is true iff
 * VBUS is present on this half. The official Cornix RMK firmware
 * (analysed by uf2 strings + literal scan, see #2 discussion) reads
 * POWER.USBREGSTATUS for charging detection rather than a charge-IC
 * STAT GPIO; ZMK's helper does the same so we don't need extra DT.
 *
 *   USB powered, battery < FULL_THRESHOLD  -> sin-wave breathing pulse
 *                                             on pixel[0] (#16)
 *   USB powered, battery >= FULL_THRESHOLD -> green 3 s steady, then off
 *                                             (shown once per full-charge)
 *   USB not powered                        -> indicator off
 *
 * Priority: while the peer-lost slow blink is active we skip the
 * breathing update for that tick so the blue peer-lost indication
 * remains visible. Other one-shot events (battery low red, BT-search
 * blink etc.) interleave through the per-pixel msgq (#18) and are
 * rendered to completion before breathing resumes naturally on the
 * next 33 ms tick.
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

#define CHARGING_FULL_THRESHOLD 95
#define FULL_SHOW_MS            3000

/* Breathing animation parameters. BREATH_PERIOD_MS / BREATH_UPDATE_MS
 * is the LUT length; BREATH_PEAK is the maximum green intensity at the
 * top of the curve. */
#define BREATH_PERIOD_MS 2000
#define BREATH_UPDATE_MS 33
#define BREATH_STEPS     60
#define BREATH_PEAK      32

/* (1 - cos(2π*i/BREATH_STEPS)) / 2 * BREATH_PEAK, generated:
 *   python -c "import math; print(','.join(str(round((1-math.cos(2*math.pi*i/60))/2*32)) for i in range(60)))"
 * Smooth 0 -> 32 -> 0 over one full period (no plateau). */
static const uint8_t BREATH_LUT[BREATH_STEPS] = {
     0,  0,  0,  1,  1,  2,  3,  4,  5,  7,
     8,  9, 11, 13, 14, 16, 18, 19, 21, 23,
    24, 25, 27, 28, 29, 30, 31, 31, 32, 32,
    32, 32, 32, 31, 31, 30, 29, 28, 27, 25,
    24, 23, 21, 19, 18, 16, 14, 13, 11,  9,
     8,  7,  5,  4,  3,  2,  1,  1,  0,  0,
};

static const struct led_rgb COL_GREEN_FULL = {.g = CORNIX_RGB_LEVEL};

static bool usb_powered;
static uint8_t latest_battery; /* 0 = unknown */
static bool full_already_shown;

static struct k_work_delayable breathing_work;
static bool breathing_active;
static uint8_t breath_idx;

static void breathing_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!breathing_active) {
        return;
    }

    /* Skip our own update if peer-lost blue is currently armed on this
     * same pixel — the user's peer-loss notification is more important
     * than the smooth charging fade for that tick. We still advance
     * the LUT index so the phase stays continuous after peer-lost ends. */
    if (!cornix_rgb_peer_blink_active()) {
        struct led_rgb c = {.g = BREATH_LUT[breath_idx]};
        cornix_rgb_set(CORNIX_RGB_PIX_INNER, c);
    }
    breath_idx = (breath_idx + 1) % BREATH_STEPS;

    k_work_reschedule(&breathing_work, K_MSEC(BREATH_UPDATE_MS));
}

static void start_breathing(void) {
    if (breathing_active) {
        return;
    }
    LOG_INF("charging: breathing start");
    breathing_active = true;
    breath_idx = 0;
    k_work_reschedule(&breathing_work, K_NO_WAIT);
}

static void stop_breathing(void) {
    if (!breathing_active) {
        return;
    }
    LOG_INF("charging: breathing stop");
    breathing_active = false;
    k_work_cancel_delayable(&breathing_work);
    /* The last breath tick may have left the LED at a non-zero value;
     * explicitly clear it so the strip isn't left glowing dim. */
    cornix_rgb_set(CORNIX_RGB_PIX_INNER, (struct led_rgb){0});
}

static int on_usb_changed(const zmk_event_t *eh) {
    ARG_UNUSED(eh);

    bool now_powered = zmk_usb_is_powered();
    if (now_powered == usb_powered) {
        return 0;
    }
    usb_powered = now_powered;
    LOG_INF("charging: usb_powered=%d", usb_powered);

    if (!usb_powered) {
        stop_breathing();
        full_already_shown = false;
        return 0;
    }

    /* Just plugged in. */
    if (latest_battery > 0 && latest_battery >= CHARGING_FULL_THRESHOLD) {
        /* Already full at plug-in: skip the breathing animation, just
         * acknowledge with the steady "fully charged" pulse. */
        LOG_INF("charging: plug-in at %u%% (>= full), steady green",
                latest_battery);
        cornix_rgb_show_once(CORNIX_RGB_PIX_INNER, COL_GREEN_FULL, FULL_SHOW_MS);
        full_already_shown = true;
    } else {
        start_breathing();
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
    uint8_t prev = latest_battery;
    latest_battery = ev->state_of_charge;

    if (!usb_powered) {
        return 0;
    }

    /* Crossing the FULL_THRESHOLD upward while breathing: stop the
     * breath and emit the steady "fully charged" pulse once. */
    if (breathing_active &&
        latest_battery >= CHARGING_FULL_THRESHOLD &&
        !full_already_shown) {
        LOG_INF("charging: reached %u%% (>= full), steady green",
                latest_battery);
        stop_breathing();
        cornix_rgb_show_once(CORNIX_RGB_PIX_INNER, COL_GREEN_FULL, FULL_SHOW_MS);
        full_already_shown = true;
        return 0;
    }

    /* Crossing the threshold downward while we already showed "full":
     * revert to breathing so the indicator follows reality (heavy
     * load while still on USB). Without this the user would only see
     * the green pulse return after a physical unplug. */
    if (full_already_shown &&
        latest_battery < CHARGING_FULL_THRESHOLD &&
        prev >= CHARGING_FULL_THRESHOLD) {
        LOG_INF("charging: fell back below %u%% (now %u%%), resume breathing",
                CHARGING_FULL_THRESHOLD, latest_battery);
        full_already_shown = false;
        start_breathing();
    }
    return 0;
}

ZMK_LISTENER(cornix_rgb_charging_bat, on_battery_for_charging);
ZMK_SUBSCRIPTION(cornix_rgb_charging_bat, zmk_battery_state_changed);

static int charging_init(void) {
    k_work_init_delayable(&breathing_work, breathing_handler);

    /* If USB is already powered at boot (e.g. plugged in before
     * power-on), start the indicator without waiting for the
     * conn-state event. latest_battery is still 0 here so we fall
     * straight into breathing; the full-detect will fire when the
     * first battery_state_changed comes in. */
    if (zmk_usb_is_powered()) {
        usb_powered = true;
        start_breathing();
    }
    return 0;
}
SYS_INIT(charging_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
