// main.cpp -- New entry point for Everytag firmware (C++ state machine)

#include "accel_data.hpp"
#include "beacon_config.hpp"
#include "beacon_state.hpp"
#include "zephyr_hardware.hpp"
#include "zephyr_nvs.hpp"

#include "myboards.h"

extern "C" {
#include "gatt_glue.h"
#include "watchdog.h"
#ifdef USE_BUTTON
#include <zephyr/drivers/gpio.h>
#endif
#ifdef HAS_LED1_SWITCH
#include <zephyr/drivers/gpio.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
}

// GPIO specs needed for button and LED1 gate initialization
#ifdef USE_BUTTON
static const struct gpio_dt_spec sw0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
#endif
#ifdef HAS_LED1_SWITCH
#define LED1_NODE DT_NODELABEL(led1)
static const struct gpio_dt_spec gate_spec = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
#endif

int main(void) {
    // All objects are static to avoid stack overflow (main stack is 1024 bytes)
    static beacon::ZephyrNvsStorage nvs;
    static beacon::SettingsManager settings(nvs);
    static beacon::MovementTracker accel;
    static beacon::ZephyrHardware hw(settings);
    static beacon::StateMachine sm(hw, settings, accel);

    my_wdt_init();
    printk("Beacon starting (C++)\n");

#ifdef USE_BUTTON
    // Configure button pin
    int err = gpio_pin_configure_dt(&sw0, GPIO_INPUT);
    if (err < 0) {
        printk("Could not configure sw0 GPIO (%d)\n", err);
        return 0;
    }
#endif

#ifdef HAS_LED1_SWITCH
    // Turn off mosfet
    gpio_pin_configure_dt(&gate_spec, GPIO_OUTPUT_ACTIVE);
#endif

    // Initialize NVS and load settings (continue with defaults on failure)
    if (nvs.init() != 0) {
        printk("Warning: NVS init failed, using defaults\n");
    }
    if (settings.load() != 0) {
        printk("Warning: settings load failed, using defaults\n");
    }

    // Set up GATT glue pointers
    glue_init(&settings, &sm);

    // Run the state machine
    sm.initialize();

    while (true) {
        // Sync GATT connection state into state machine before each tick
        glue_sync_gatt_state(&sm);
        sm.tick();
    }

    return 0;
}
