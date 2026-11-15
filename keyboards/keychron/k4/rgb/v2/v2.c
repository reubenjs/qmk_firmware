// Copyright 2025 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#include "quantum.h"
#include "gpio.h"
#include "timer.h"
#include "iton_bt.h"
#include "connection.h"


#define BT_PROFILE_RESET_HOLD_MS 3000
#define BT_PAIRING_REPEAT_MS     100
// Number-row LEDs used for profile pairing status: 1/2/3 are indices 20-22.
#define BT_LED_BASE_INDEX        20
#define BT_LED_CONNECTED_MS      2500
#define BT_BATT_QUERY_MS         60000

// dip_switch_init() (and the first dip_switch_update_kb() calls it triggers)
// runs before bluetooth_init() in quantum/keyboard.c, so the initial mode
// must not talk to the BT module until after iton_bt_init() has run.
static bool     bt_ready         = false;
static bool     bt_mode          = false;
static uint8_t  bt_profile       = 0;
static uint16_t bt_held_keycode  = 0;
static uint16_t bt_held_timer    = 0;
static uint16_t bt_pairing_timer = 0;
static bool     bt_mode_sent     = false;
static uint16_t bt_mode_timer    = 0;
static uint8_t  bt_led_state     = 0; // 0 idle, 1 pairing flash, 2 connected hold
static uint16_t bt_led_timer     = 0;
static uint8_t  bt_batt_level    = 0;
static uint16_t bt_batt_timer    = 0;
static bool     bt_batt_due      = false;


// DIP index 0 = cable/BT selector (active = cable), index 1 = win/mac (unused).
static void k4v2_apply_dip_mode(bool cable_active) {
    bt_mode = !cable_active;
    if (bt_mode) {
        iton_bt_switch_profile(bt_profile);
        iton_bt_enable_sleep();
        connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
    } else {
        iton_bt_mode_usb();
        connection_set_host_noeeprom(CONNECTION_HOST_USB);
    }
}

bool dip_switch_update_kb(uint8_t index, bool active) {
    if (!dip_switch_update_user(index, active)) {
        return false;
    }

    if (index == 0 && bt_ready) {
        k4v2_apply_dip_mode(active);
    }

    return true;
}

void keyboard_post_init_kb(void) {
    bt_ready = true;
    // Set the host immediately but defer the iton mode command; stock delays
    // it after boot rather than talking to the module right away.
    bt_mode = gpio_read_pin(D5);
    connection_set_host_noeeprom(bt_mode ? CONNECTION_HOST_BLUETOOTH : CONNECTION_HOST_USB);
    bt_mode_timer = timer_read();
    keyboard_post_init_user();
}

bool process_record_kb(uint16_t keycode, keyrecord_t *record) {
    if (!process_record_user(keycode, record)) {
        return false;
    }

    if (!bt_mode) {
        return true;
    }

    switch (keycode) {
        case BT_PRF1:
        case BT_PRF2:
        case BT_PRF3:
            if (record->event.pressed) {
                bt_profile       = keycode - BT_PRF1;
                bt_held_keycode  = keycode;
                bt_held_timer    = timer_read();
                bt_pairing_timer = 0;
                iton_bt_switch_profile(bt_profile);
            } else {
                bt_held_keycode = 0;
            }
            return false;
    }

    return true;
}

void housekeeping_task_kb(void) {
    // Deferred boot-time mode command (see keyboard_post_init_kb).
    if (!bt_mode_sent && timer_elapsed(bt_mode_timer) >= 1000) {
        bt_mode_sent = true;
        if (bt_mode) {
            iton_bt_switch_profile(bt_profile);
            iton_bt_enable_sleep();
        } else {
            iton_bt_mode_usb();
        }
    }

    // Stock re-queues enter_pairing every tick while a profile key is held
    // past its hold timer.
    if (bt_mode && bt_held_keycode && timer_elapsed(bt_held_timer) >= BT_PROFILE_RESET_HOLD_MS && timer_elapsed(bt_pairing_timer) >= BT_PAIRING_REPEAT_MS) {
        bt_pairing_timer = timer_read();
        bt_led_state     = 1;
        bt_led_timer     = timer_read();
        iton_bt_enter_pairing();
    }

    // Battery level: query shortly after every (re)connect and periodically
    // while connected. Sends are thread-context, so callbacks only flag it.
    if (bt_mode && iton_bt_is_connected && (bt_batt_due || timer_elapsed(bt_batt_timer) >= BT_BATT_QUERY_MS)) {
        bt_batt_due   = false;
        bt_batt_timer = timer_read();
        iton_bt_query_battery_level();
    }


}

/**
 * Module notifications. These run inside the INT-line pal callback (ISR
 * context), so they may only set flags — anything that sends a frame must
 * be deferred to housekeeping_task_kb.
 */
void iton_bt_entered_pairing(void) {
    bt_led_state = 1;
    bt_led_timer = timer_read();
}

void iton_bt_enters_connection_state(void) {
    // Keep flashing while the module tries the stored host.
}

void iton_bt_connection_successful(void) {
    bt_led_state = 2;
    bt_led_timer = timer_read();
    bt_batt_due  = true;
}

void iton_bt_disconnected(void) {
    bt_led_state = 0;
}

void iton_bt_battery_level(uint8_t level) {
    bt_batt_level = level;
}

void iton_bt_battery_voltage_low(void) {
    bt_batt_level = batt_below_30;
}

void iton_bt_battery_low_power_shutdown(void) {
    bt_batt_level = batt_below_30;
}

void iton_bt_battery_exit_low_battery_mode(void) {
    bt_batt_due = true; // refresh the level at the next housekeeping pass
}

// While pairing, blank the matrix and flash the active profile's key blue.
// On connect it holds solid briefly, then control returns to the animation.
bool rgb_matrix_indicators_kb(void) {
    if (!rgb_matrix_indicators_user()) {
        return false;
    }
    if (!bt_mode) {
        return true;
    }
    if (bt_led_state) {
        if (bt_led_state == 2 && timer_elapsed(bt_led_timer) >= BT_LED_CONNECTED_MS) {
            bt_led_state = 0;
            return true;
        }
        rgb_matrix_set_color_all(0, 0, 0);
        if (bt_led_state == 2 || (timer_elapsed(bt_led_timer) % 500) < 250) {
            rgb_matrix_set_color(BT_LED_BASE_INDEX + bt_profile, 0, 0, 255);
        }
        return true;
    }
    // Persistent reminder while the battery is low: the profile key blinks
    // red for ~0.6s every 4s without disturbing the rest of the animation.
    if (bt_batt_level == batt_below_30 && (timer_read() % 4000) < 600) {
        rgb_matrix_set_color(BT_LED_BASE_INDEX + bt_profile, 255, 0, 0);
    }
    return true;
}
