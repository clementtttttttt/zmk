/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_DISPLAY)

#include <zmk/display.h>
#include <lvgl.h>

#endif

int main(void) {
    LOG_INF("Welcome to ZMK!\n");

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();
    settings_load();
#endif

#ifdef CONFIG_ZMK_DISPLAY
    zmk_display_init();

#if IS_ENABLED(CONFIG_ARCH_POSIX)
    // Workaround for an SDL display issue:
    // https://github.com/zephyrproject-rtos/zephyr/issues/71410
    while (1) {
        lv_task_handler();
        k_sleep(K_MSEC(10));
    }
#endif


#endif /* CONFIG_ZMK_DISPLAY */


static const struct gpio_dt_spec PS2_CK =
	GPIO_DT_SPEC_GET(DT_NODELABEL(ps2ck), gpios);
static const struct gpio_dt_spec PS2_DO =
	GPIO_DT_SPEC_GET(DT_NODELABEL(ps2do), gpios);



	   // dev = device_get_binding(DT_GPIO_LABEL(DT_NODELABEL(ps2), ps2ck));
	    gpio_pin_configure_dt(&PS2_CK, GPIO_OUTPUT_ACTIVE); //CK
		gpio_pin_configure_dt(&PS2_DO, GPIO_OUTPUT_ACTIVE); //do

		gpio_pin_set_dt(&PS2_CK, 1);
		gpio_pin_set_dt(&PS2_DO, 1);

    return 0;
}
