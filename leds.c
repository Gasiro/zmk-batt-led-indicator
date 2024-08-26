#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/battery.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

// GPIO-based LED device and indices of LED inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_led)),
             "An alias for indicator-led is not found for RGBLED_WIDGET");
static const uint8_t led_idx = DT_NODE_CHILD_IDX(DT_ALIAS(indicator_led));

// flag to indicate whether the initial boot up sequence is complete
static bool initialized = false;

// blink rates as specified by different conditions
enum blink_rate_t {
    BLINK_OFF,     // LED off
    BLINK_SLOW,
    BLINK_MEDIUM,
    BLINK_FAST,
    BLINK_FRANTIC
};

// a blink work item as specified by the blink rate and duration
struct blink_item {
    enum blink_rate_t rate;
    uint16_t duration_ms;
    bool first_item;
    uint16_t sleep_ms;
};


// define message queue of blink work items, that will be processed by a separate thread
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);

static void led_do_blink(enum blink_rate_t rate) {
    switch (rate) {
        case BLINK_OFF:
            led_off(led_dev, led_idx);
            break;
        case BLINK_SLOW:
            led_on(led_dev, led_idx);
            k_sleep(K_MSEC(300));
            led_off(led_dev, led_idx);
            k_sleep(K_MSEC(300));
            break;
        case BLINK_MEDIUM:
            led_on(led_dev, led_idx);
            k_sleep(K_MSEC(150));
            led_off(led_dev, led_idx);
            k_sleep(K_MSEC(150));
            break;
        case BLINK_FAST:
            led_on(led_dev, led_idx);
            k_sleep(K_MSEC(80));
            led_off(led_dev, led_idx);
            k_sleep(K_MSEC(80));
            break;
        case BLINK_FRANTIC:
            led_on(led_dev, led_idx);
            k_sleep(K_MSEC(20));
            led_off(led_dev, led_idx);
            k_sleep(K_MSEC(20));
            break;
    }
}


#if IS_ENABLED(CONFIG_ZMK_BLE)
static void output_blink(void) {
    struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_OUTPUT_BLINK_MS};

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    uint8_t profile_index = zmk_ble_active_profile_index();
    if (zmk_ble_active_profile_is_connected()) {
        LOG_INF("Profile %d connected, blinking off", profile_index);
        blink.rate = BLINK_OFF;
    } else if (zmk_ble_active_profile_is_open()) {
        LOG_INF("Profile %d open, blinking fast", profile_index);
        blink.rate = BLINK_FAST;
    } else {
        LOG_INF("Profile %d not connected, blinking slow", profile_index);
        blink.rate = BLINK_SLOW;
    }
#else
    if (zmk_split_bt_peripheral_is_connected()) {
        LOG_INF("Peripheral connected, blinking off");
        blink.rate = BLINK_SLOW;
    } else {
        LOG_INF("Peripheral not connected, blinking fast");
        blink.rate = BLINK_FAST;
    }
#endif

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}

static int led_output_listener_cb(const zmk_event_t *eh) {
    if (initialized) {
        output_blink();
    }
    return 0;
}

ZMK_LISTENER(led_output_listener, led_output_listener_cb);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
// run led_output_listener_cb on BLE profile change (on central)
ZMK_SUBSCRIPTION(led_output_listener, zmk_ble_active_profile_changed);
#else
// run led_output_listener_cb on peripheral status change event
ZMK_SUBSCRIPTION(led_output_listener, zmk_split_peripheral_status_changed);
#endif
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int led_battery_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }

    // check if we are in critical battery levels at state change, blink if we are
    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;

    if (battery_level > 0 && battery_level <= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL) {
        LOG_INF("Battery level %d, blinking fast for critical", battery_level);

        struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS,
                                   .rate = BLINK_FAST};
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }
    return 0;
}

// run led_battery_listener_cb on battery state change event
ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int led_layer_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }

    // ignore layer off events
    if (!as_zmk_layer_state_changed(eh)->state) {
        return 0;
    }

    uint8_t index = zmk_keymap_highest_layer_active();
    static const struct blink_item blink = {
        .duration_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
        .rate = BLINK_FRANTIC,
        .sleep_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS};
    static const struct blink_item final_blink = {
        .duration_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
        .rate = BLINK_MEDIUM};
    for (int i = 0; i < index; i++) {
        if (i < index - 1) {
            k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
        } else {
            k_msgq_put(&led_msgq, &final_blink, K_NO_WAIT);
        }
    }
    return 0;
}

ZMK_LISTENER(led_layer_listener, led_layer_listener_cb);
ZMK_SUBSCRIPTION(led_layer_listener, zmk_layer_state_changed);
#endif
#endif // IS_ENABLED(CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE)

extern void led_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    while (true) {
        // wait until a blink item is received and process it
        struct blink_item blink;
        k_msgq_get(&led_msgq, &blink, K_FOREVER);
        LOG_DBG("Got a blink item from msgq, rate %d, duration %d", blink.rate,
                blink.duration_ms);

        led_do_blink(blink.rate);

        // wait interval before processing another blink
        if (blink.sleep_ms > 0) {
            k_sleep(K_MSEC(blink.sleep_ms));
        } else {
            k_sleep(K_MSEC(CONFIG_RGBLED_WIDGET_INTERVAL_MS));
        }
    }
}

// define led_process_thread with stack size 1024, start running it 100 ms after boot
K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 100);

extern void led_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    // check and indicate battery level on thread start
    LOG_INF("Indicating initial battery status");

    struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS,
                               .first_item = true};
    uint8_t battery_level = zmk_battery_state_of_charge();
    int retry = 0;
    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    };

    if (battery_level == 0) {
        LOG_INF("Battery level undetermined (zero), blinking off");
        blink.rate = BLINK_OFF;
    } else if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_HIGH) {
        LOG_INF("Battery level %d, blinking slow", battery_level);
        blink.rate = BLINK_SLOW;
    } else if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_LOW) {
        LOG_INF("Battery level %d, blinking fast", battery_level);
        blink.rate = BLINK_FAST;
    } else {
        LOG_INF("Battery level %d", battery_level);
        // blink.color = LED_RED;
    }

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);

    // wait until blink should be displayed for further checks
    k_sleep(K_MSEC(CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS + CONFIG_RGBLED_WIDGET_INTERVAL_MS));
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#if IS_ENABLED(CONFIG_ZMK_BLE)
    // check and indicate current profile or peripheral connectivity status
    LOG_INF("Indicating initial connectivity status");
    output_blink();
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

    initialized = true;
    LOG_INF("Finished initializing LED widget");
}

// run init thread on boot for initial battery+output checks
K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 200);
