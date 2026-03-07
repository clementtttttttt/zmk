/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/endpoints.h>

#include <zephyr/drivers/gpio.h>

static int hid_listener_keycode_pressed(const struct zmk_keycode_state_changed *ev) {
    int err, explicit_mods_changed, implicit_mods_changed;

    if (!is_mod(ev->usage_page, ev->keycode) &&
        zmk_hid_is_pressed(ZMK_HID_USAGE(ev->usage_page, ev->keycode))) {
        LOG_DBG("unregistering usage_page 0x%02X keycode 0x%02X since it was already pressed",
                ev->usage_page, ev->keycode);
        err = zmk_hid_release(ZMK_HID_USAGE(ev->usage_page, ev->keycode));
        if (err < 0) {
            LOG_DBG("Unable to pre-release keycode (%d)", err);
            return err;
        }
        err = zmk_endpoints_send_report(ev->usage_page);
        if (err < 0) {
            LOG_ERR("Failed to send key report for pre-releasing keycode (%d)", err);
        }
    }

    LOG_DBG("usage_page 0x%02X keycode 0x%02X implicit_mods 0x%02X explicit_mods 0x%02X",
            ev->usage_page, ev->keycode, ev->implicit_modifiers, ev->explicit_modifiers);
    err = zmk_hid_press(ZMK_HID_USAGE(ev->usage_page, ev->keycode));
    if (err < 0) {
        LOG_DBG("Unable to press keycode");
        return err;
    }
    explicit_mods_changed = zmk_hid_register_mods(ev->explicit_modifiers);
    implicit_mods_changed = zmk_hid_implicit_modifiers_press(ev->implicit_modifiers);
    if (ev->usage_page != HID_USAGE_KEY &&
        (explicit_mods_changed > 0 || implicit_mods_changed > 0)) {
        err = zmk_endpoints_send_report(HID_USAGE_KEY);
        if (err < 0) {
            LOG_ERR("Failed to send key report for changed mofifiers for consumer page event (%d)",
                    err);
        }
    }

    return zmk_endpoints_send_report(ev->usage_page);
}

static int hid_listener_keycode_released(const struct zmk_keycode_state_changed *ev) {
    int err, explicit_mods_changed, implicit_mods_changed;

    LOG_DBG("usage_page 0x%02X keycode 0x%02X implicit_mods 0x%02X explicit_mods 0x%02X",
            ev->usage_page, ev->keycode, ev->implicit_modifiers, ev->explicit_modifiers);
    err = zmk_hid_release(ZMK_HID_USAGE(ev->usage_page, ev->keycode));
    if (err < 0) {
        LOG_DBG("Unable to release keycode");
        return err;
    }

#if IS_ENABLED(CONFIG_ZMK_HID_SEPARATE_MOD_RELEASE_REPORT)

    // send report of normal key release early to fix the issue
    // of some programs recognizing the implicit_mod release before the actual key release
    err = zmk_endpoints_send_report(ev->usage_page);
    if (err < 0) {
        LOG_ERR("Failed to send key report for the released keycode (%d)", err);
    }

#endif // IS_ENABLED(CONFIG_ZMK_HID_SEPARATE_MOD_RELEASE_REPORT)

    explicit_mods_changed = zmk_hid_unregister_mods(ev->explicit_modifiers);
    // There is a minor issue with this code.
    // If LC(A) is pressed, then LS(B), then LC(A) is released, the shift for B will be released
    // prematurely. This causes if LS(B) to repeat like Bbbbbbbb when pressed for a long time.
    // Solving this would require keeping track of which key's implicit modifiers are currently
    // active and only releasing modifiers at that time.
    implicit_mods_changed = zmk_hid_implicit_modifiers_release();

    if (ev->usage_page != HID_USAGE_KEY &&
        (explicit_mods_changed > 0 || implicit_mods_changed > 0)) {
        err = zmk_endpoints_send_report(HID_USAGE_KEY);
        if (err < 0) {
            LOG_ERR("Failed to send key report for changed mofifiers for consumer page event (%d)",
                    err);
        }
    }
    return zmk_endpoints_send_report(ev->usage_page);
}


K_THREAD_STACK_DEFINE(ps2_stack, 2048);

static struct k_thread ps2_thread;
enum kb_report_idx {
	RP_ID = 0,
	KB_MOD_KEY ,
	KB_RESERVED,
	KB_KEY_CODE1,
	KB_KEY_CODE2,
	KB_KEY_CODE3,
	KB_KEY_CODE4,
	KB_KEY_CODE5,
	KB_KEY_CODE6,
	KB_REPORT_COUNT,
};

static void clk_cycle( const struct gpio_dt_spec *d){
		k_busy_wait(5);

		gpio_pin_set_dt(d, 0);
		k_busy_wait(30);
		gpio_pin_set_dt(d, 1);
		k_busy_wait(30);

}

static void ps2_send_byte(uint8_t in){
	static const struct gpio_dt_spec PS2_CK =
		GPIO_DT_SPEC_GET(DT_NODELABEL(ps2ck), gpios );
	static const struct gpio_dt_spec PS2_DO =
		GPIO_DT_SPEC_GET(DT_NODELABEL(ps2do), gpios);

		
	gpio_pin_set_dt(&PS2_DO, 0);
	clk_cycle(&PS2_CK);
	
	char numbits = 0;
	
	for(int i=0; i<8; ++i){
			if(in & 1) ++numbits;
			gpio_pin_set_dt(&PS2_DO, in & 1);
			clk_cycle(&PS2_CK);
			in >>= 1;
	}
	
	gpio_pin_set_dt(&PS2_DO, !(numbits & 1));
	clk_cycle(&PS2_CK);
	
	gpio_pin_set_dt(&PS2_DO, 1);
	clk_cycle(&PS2_CK);
	//k_sleep(K_USEC(77* 5/));
	k_sleep(K_MSEC(5));

}

uint8_t old_mods = 0;

static void ps2_write_report(const struct zmk_keycode_state_changed *in){
		
		uint16_t  ps2_mapping_table[] = {
			0x1c,
			0x32,
			0x21,
			0x23,
			0x24,
			0x2b,
			0x34,
			0x33,
			0x43,
			0x3b,
			0x42,
			0x4b,
			0x3a,
			0x31,
			0x44,
			0x4d,
			0x15,
			0x2d,
			0x1b,
			0x2c,
			0x3c,
			0x2a,
			0x1d,
			0x22,
			0x35,
			0x1a,
			0x16,
			0x1e,
			0x26,
			0x25,
			0x2e,
			0x36,
			0x3d,
			0x3e,
			0x46,
			0x45,
			0x5a,
			0x76,
			0x66,
			0xd,
			0x29,
			0x4e,
			0x55,
			0x54,
			0x5b,
			0x5d,
			0x5d,
			0x4c,
			0x52,
			0x0e,
			0x41,
			0x49,
			0x4a,
			0x58,
			0x05,
			0x6,
			0x4,
			0xc,
			0x3,
			0xb,
			0x83,
			0x0a,
			0x01,
			0x09,
			0x78,
			0x07,
			0x17c,
			0x7e,
			0x17e, //pause or ctrl pause
			0x170,
			0x16c,
			0x17d,
			0x171,
			0x179,
			0x17a,
			0x174,
			0x16b,
			0x172,
			0x175,
			0x77,
			0x14a,
			0x7c,
			0x7b,
			0x79,
			0x15a,
			0x69,
			0x72,
			0x7a,
			0x6b,
			0x73,
			0x74,
			0x6c,
			0x75,
			0x7d,
			0x70,
			0x71,
			0x61,
			0x12f,
			0x137,
			0xf,
			0x8,
			0x10,
			0x18,
			0x20,
			0x28,
			0x30,
			0x38,
			0x40,
			0x48,
			0x50,
			0x57,
			0x5f,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0xF0CC,
			0x6d,
			0xf0cc,
			0x51,
			0x13,
			0x6a,
			0x64,
			0x67,
			0x27,
			0xf0cc,
			0xf0cc,
			0xf0cc,
			0xf2,
			0xf1,
			0x63,
			0x62,
			0x5f,
};

	uint16_t ps2_mapping_table_2[]= {
			0x14,
			0x12,
			0x11,
			0x11f,
			0x114,
			0x59,
			0x111,
			0x127,
			0xf0cc,
			0x14d,
			0x115,
			0x13b,
			0x134,
			0x123,
			0xf0cc,
			0xf0cc,
			0x132,
			0x121,
			0xf0cc,
			0xf0cc,
			0xf0cc,
			0xf0cc,
			0x150,
			0x148,
			0x12b,
			0x140,
			0x110,
			0x13a,
			0x138,
			0x130,
			0x128,
			0x120,
			0xf0cc,
			0x47,
			0x4f,
			0x65,
			0x68,
			0x6e,
			0x147,
			0x14f,
			0x165,
			0x167,
			0x168,
			0x16a
			
		};
		
		uint16_t mod_mappings[]={
			0x14,
			0x12,
			0x11,
			0xF0CC,
			0x11d,
			0x159,
			0x138,
			0xF0CC
		};
		
	/*	if(in->explicit_modifiers != old_mods){
							uint8_t mods = in->explicit_modifiers;

			for(int i=0;i<8; ++i){
				if((old_mods & 1) && !(mods & 1)){
					if(mod_mappings[i] == 0xf0cc) continue;
					if(mod_mappings[i] & 0x100) ps2_send_byte(0xe0);
					ps2_send_byte(0xf0);
					ps2_send_byte(mod_mappings[i] & 0xff);
				}
				if(!(old_mods & 1) && (mods & 1)){
					if(mod_mappings[i] == 0xf0cc) continue;
					if(mod_mappings[i] & 0x100) ps2_send_byte(0xe0);
					ps2_send_byte(mod_mappings[i] & 0xff);
				}
				old_mods>>=1;
				mods >>=1;
			}
			old_mods = 		in->explicit_modifiers;
		}
		*/
		if(((in->keycode >= 0xe0) && ((in->keycode - 0xe0) < sizeof(ps2_mapping_table_2)/sizeof(uint16_t)))||(in->keycode >= 4) && ((in->keycode - 4) < sizeof(ps2_mapping_table)/sizeof(uint16_t))){
			uint16_t newcode;
			if(in->keycode >= 0xe0) newcode = ps2_mapping_table_2[in->keycode - 0xe0];
			 else newcode = ps2_mapping_table[in->keycode - 4];
			if(newcode == 0xf0cc) return;
			if(newcode & 0x100){
				ps2_send_byte(0xe0);
			}
			
			if(!in->state) ps2_send_byte(0xf0);
			
			ps2_send_byte(newcode);
		}

}


void ps2_start_send(const struct zmk_keycode_state_changed *in){
			k_thread_join(&ps2_thread, K_MSEC(100));

		k_tid_t blink_tid = k_thread_create(&ps2_thread,          // Thread struct
		                              ps2_stack,            // Stack
                               K_THREAD_STACK_SIZEOF(ps2_stack),
                               ps2_write_report,     // Entry point
                            in,                  // arg_1‎
                               NULL,                   // arg_2‎
                            NULL,                   // arg_3‎
                            7,                      // Priority‎
                                                            0,                      // Options‎
                               K_NO_WAIT);             // Delay
	

}
 struct zmk_keycode_state_changed *ev[16];
int hid_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev) {
		
		
        if (ev->state) {
            hid_listener_keycode_pressed(ev);
        } else {
            hid_listener_keycode_released(ev);
        }
		//ps2_start_send(ev);
		ps2_write_report(ev);
    }
    return 0;
}

ZMK_LISTENER(hid_listener, hid_listener);
ZMK_SUBSCRIPTION(hid_listener, zmk_keycode_state_changed);
