/*
 * Copyright (c) 2026 numachang
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/led_strip.h>

/* B1b probe confirmed: pixel[0] = inner LED, pixel[1] = outer LED on both
 * halves (chains are mirrored). Manual roles map cleanly onto these:
 *   pixel[0] (inner) -> battery + peer-loss notifications
 *   pixel[1] (outer) -> BT profile (central) or peer status (peripheral)
 */
#define CORNIX_RGB_PIX_INNER 0
#define CORNIX_RGB_PIX_OUTER 1

/* Display dim by default to keep RGB drain low. */
#define CORNIX_RGB_LEVEL 0x10

/* Peer-lost continuous blink (B3): blue, 1 Hz, duty 40%, capped at
 * ~10 seconds or until the link comes back. */
#define CORNIX_PEER_BLINK_ON_MS     400
#define CORNIX_PEER_BLINK_PERIOD_MS 1000
#define CORNIX_PEER_BLINK_MAX_COUNT 10

struct cornix_rgb_msg {
    uint8_t pixel_index;
    struct led_rgb color;
    uint8_t blink_count; /* >=1 */
    uint16_t on_ms;
    uint16_t off_ms;
};

/* Enqueue a raw message. Drops if the queue is full. */
void cornix_rgb_queue(const struct cornix_rgb_msg *msg);

/* Helper: light a single pixel for on_ms, then turn it off. */
void cornix_rgb_show_once(uint8_t idx, struct led_rgb color, uint16_t on_ms);

/* Helper: blink a single pixel `count` times. */
void cornix_rgb_blink(uint8_t idx, struct led_rgb color, uint8_t count,
                      uint16_t on_ms, uint16_t off_ms);

/* Helper: set a pixel's colour and leave it (no auto-off). Used by the
 * breathing animation in charging.c, which streams ~30 set updates per
 * second to render the sin-wave fade. Internally rendered as a msg
 * with on_ms = off_ms = 0, which the worker treats as "write and
 * return immediately". */
void cornix_rgb_set(uint8_t idx, struct led_rgb color);

/* Returns true while the per-half peer-lost slow blink is armed (i.e. peer
 * is currently absent and the worker is mid-cycle). Implemented by
 * central.c / peripheral.c; charging.c skips its own green blink while
 * peer-lost is showing so the peer indication is not visually overwritten. */
bool cornix_rgb_peer_blink_active(void);
