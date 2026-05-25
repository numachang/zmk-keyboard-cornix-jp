/*
 * Copyright (c) 2026 numachang
 * SPDX-License-Identifier: MIT
 *
 * Stage B3 (central / left half):
 *   pixel[1] (outer): BT profile colour (BT0=green / BT1=red / BT2=blue)
 *                     on every active profile change.
 *   pixel[0] (inner): blue 3 s steady on peripheral link-up; while
 *                     peripheral is lost, blue blink at 1 Hz / duty 40%
 *                     for up to 10 cycles or until the link returns;
 *                     red 2x blink on self battery low.
 *
 * Note on peer detection: zmk_split_peripheral_status_changed is only
 * raised on the peripheral side. On central we have to register our
 * own bt_conn_cb (matching what ZMK's own split/bluetooth/central.c
 * does) and filter to info.role == BT_CONN_ROLE_CENTRAL so we ignore
 * host (BT0/1/2) connections, which are handled by the BT profile
 * listener above.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#include <zephyr/bluetooth/conn.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/battery_state_changed.h>

#include <cornix_rgb_indicator/widget.h>

LOG_MODULE_DECLARE(cornix_rgb, CONFIG_ZMK_LOG_LEVEL);

#define BATTERY_LEVEL_LOW 20
#define PEER_ON_MS        3000

static const struct led_rgb COL_RED   = {.r = CORNIX_RGB_LEVEL};
static const struct led_rgb COL_GREEN = {.g = CORNIX_RGB_LEVEL};
static const struct led_rgb COL_BLUE  = {.b = CORNIX_RGB_LEVEL};

static struct led_rgb bt_profile_color(uint8_t profile_index) {
    switch (profile_index) {
    case 0:
        return COL_GREEN;
    case 1:
        return COL_RED;
    case 2:
        return COL_BLUE;
    default:
        return COL_BLUE;
    }
}

static int on_bt_profile_changed(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    int profile = zmk_ble_active_profile_index();
    if (profile < 0) {
        return 0;
    }
    LOG_INF("central: BT profile -> %d", profile);
    cornix_rgb_show_once(CORNIX_RGB_PIX_OUTER,
                         bt_profile_color((uint8_t)profile), 600);
    return 0;
}

ZMK_LISTENER(cornix_rgb_central_bt, on_bt_profile_changed);
ZMK_SUBSCRIPTION(cornix_rgb_central_bt, zmk_ble_active_profile_changed);

static bool is_peripheral_link(struct bt_conn *conn) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0) {
        return false;
    }
    /* We are central of the link; the other side is our peripheral half. */
    return info.role == BT_CONN_ROLE_CENTRAL;
}

static struct k_work_delayable peer_blink_work;
static int peer_blink_remaining;

static void peer_blink_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (peer_blink_remaining <= 0) {
        return;
    }
    cornix_rgb_blink(CORNIX_RGB_PIX_INNER, COL_BLUE, 1,
                     CORNIX_PEER_BLINK_ON_MS, 0);
    peer_blink_remaining--;
    if (peer_blink_remaining > 0) {
        k_work_reschedule(&peer_blink_work,
                          K_MSEC(CORNIX_PEER_BLINK_PERIOD_MS));
    } else {
        LOG_INF("central: peer-lost blink timed out");
    }
}

static int peer_blink_init(void) {
    k_work_init_delayable(&peer_blink_work, peer_blink_handler);
    return 0;
}
SYS_INIT(peer_blink_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static void on_peer_connected(struct bt_conn *conn, uint8_t err) {
    if (err || !is_peripheral_link(conn)) {
        return;
    }
    LOG_INF("central: peripheral linked");
    peer_blink_remaining = 0;
    k_work_cancel_delayable(&peer_blink_work);
    cornix_rgb_show_once(CORNIX_RGB_PIX_INNER, COL_BLUE, PEER_ON_MS);
}

static void on_peer_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    if (!is_peripheral_link(conn)) {
        return;
    }
    LOG_INF("central: peripheral lost, starting blink");
    peer_blink_remaining = CORNIX_PEER_BLINK_MAX_COUNT;
    k_work_reschedule(&peer_blink_work, K_NO_WAIT);
}

BT_CONN_CB_DEFINE(cornix_rgb_central_conn_cb) = {
    .connected = on_peer_connected,
    .disconnected = on_peer_disconnected,
};

static int on_battery_changed(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev == NULL || ev->state_of_charge == 0) {
        return 0;
    }
    if (ev->state_of_charge <= BATTERY_LEVEL_LOW) {
        LOG_INF("central: battery low %u%%", ev->state_of_charge);
        cornix_rgb_blink(CORNIX_RGB_PIX_INNER, COL_RED, 2, 400, 400);
    }
    return 0;
}

ZMK_LISTENER(cornix_rgb_central_bat, on_battery_changed);
ZMK_SUBSCRIPTION(cornix_rgb_central_bat, zmk_battery_state_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
