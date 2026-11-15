// Copyright 2025 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bluetooth.h"
#include "iton_bt.h"

void bluetooth_init(void) {
    iton_bt_init();
}

bool bluetooth_is_connected(void) {
    return iton_bt_is_connected;
}

uint8_t bluetooth_keyboard_leds(void) {
#ifdef ITON_BT_ENABLE_LED_STATE
    return iton_bt_led_state;
#else
    return 0;
#endif
}

void bluetooth_send_keyboard(report_keyboard_t *report) {
    iton_bt_send_keyboard(report);
}

void bluetooth_send_consumer(uint16_t usage) {
    iton_bt_send_consumer(usage);
}

void bluetooth_send_system(uint16_t data) {
    iton_bt_send_system(data);
}
