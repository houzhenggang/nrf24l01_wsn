/*
 * pic24f RTOS based OS
 * Copyright (C) 2015 <spiriou31@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdio.h>
// Board configuration
#include "board_config.h"
// os modules
#include "FreeRTOS.h"
#include "task.h"
#include "device.h"
// Utils
#include "logger.h"
#include "uip/uip.h"
// App modules
#include "uip/nrf24l01_link.h"
#include "serial/serial.h"
#include "tcp_app/tcp_server.h"
#include "mqtt/mqtt.h"
// I/O modules
#include "pic24f_gpio.h"
#include "pic24f_adc.h"
#include "ir_control.h"

static void mqtt_message_callback(const char* topic, uint8_t* payload, int payload_length);
static int mqtt_main_connect(struct mqtt_client* mqtt_client, uip_ipaddr_t *server);

extern TaskHandle_t nrf_task_handle;

static void main_task(void *arg)
{
    struct mqtt_client mqtt_client;
    struct timer timer_temp;
    struct timer timer_blink;
    char mqtt_temp_buf[16];

    // MQTT server IP
    uip_ipaddr_t mqtt_server;
    CONFIGURE_IP(&mqtt_server, MQTT_SRV);


    // Set blink led pin to output
    gpio_configure(BLINK_LED_PIN, 0, 0);
    // Set button pin to input
    gpio_configure(TST_BTN_PIN, 1, 0);
    // Turn led ON
    gpio_set(BLINK_LED_PIN, 1);
    // Configure ADC pins
    adc_configure(TEMP_SENSOR_PIN, true);
    adc_configure(LUM_SENSOR_PIN,  true);

    // Init user app timers
    timer_set(&timer_temp, CLOCK_SECOND * 10);
    timer_set(&timer_blink, CLOCK_SECOND / 2);

    // Init tcp test app module
    tcp_hello_init();
    // Init mqtt
    mqtt_init(&mqtt_client, MQTT_NODE_ID, mqtt_message_callback);
    mqtt_set_blocking_mode(&mqtt_client, 0);

    // Print unused byte count in heap for debug purposes
    log_info("heap free %d", xPortGetFreeHeapSize());

    while (1) {
        net_poll(pdMS_TO_TICKS(100));

        // Check if MQTT needs reconnection
        if (!mqtt_main_connect(&mqtt_client, &mqtt_server)) {
            gpio_toggle(BLINK_LED_PIN);
            continue;
        }
        // Temperature monitoring
        if(timer_expired(&timer_temp)) {
            timer_reset(&timer_temp);
            sprintf(mqtt_temp_buf, "t%04dl%02d",
                    (32*adc_get_value(TEMP_SENSOR_PIN))-5000, // Convert 0->1023 to Temp*100
                    adc_get_value(LUM_SENSOR_PIN));

            mqtt_publish_string(&mqtt_client, "home/th", mqtt_temp_buf);
        }
        // Blink LED
        if(timer_expired(&timer_blink)) {
            timer_reset(&timer_blink);
            gpio_toggle(BLINK_LED_PIN);
        }
    }
}

void vApplicationIdleHook(void)
{
    while (!log_is_empty()) {
        log_flush_msg();
    }
    // Go to idle mode (low power)
    Idle();
}

void vApplicationMallocFailedHook(void)
{
    log_error("Out of heap memory");
    while (1) {
        vApplicationIdleHook();
        Idle();
    }
}

void vApplicationStackOverflowHook( TaskHandle_t xTask, signed char *pcTaskName)
{
    log_error("Out of stack <%s>", pcTaskName);
    while (1) {
        vApplicationIdleHook();
        Idle();
    }
}

// Application main function
int main(void)
{
    // Init in a new stack frame
    {
        // Init device hardware
        setup_device();
        // Create main task
        xTaskCreate(main_task, "app", configMINIMAL_STACK_SIZE + 160, NULL, maintask_PRIORITY, NULL);
        // Init serial module
        serial_init(rxtask_PRIORITY);
        // Init nrf24l01 link module + tcp/ip stack
        uip_ipaddr_t ip, gateway, mask;
        CONFIGURE_IP(&ip,      IP_ADDR);  // Host IP
        CONFIGURE_IP(&gateway, GW_ADDR);  // Gateway IP
        CONFIGURE_IP(&mask,    NET_MASK); // Net mask
        // Init tcp link interface
        nrf_link_init(DEV_NRF24L01_0, ip, gateway, mask, nrftask_PRIORITY);
        // Init uip stack
        uip_init();
        // Set network parameters
        uip_sethostaddr(ip);
        uip_setdraddr(gateway);
        uip_setnetmask(mask);
    }

    // Start the scheduler
    vTaskStartScheduler();

    log_error("OS init failed");
    while (1) {
        vApplicationIdleHook();
        Idle();
    }
    return 0;
}

static uint8_t char2hex(char c)
{
    if(c>='0' && c<='9') {
        return (uint8_t)c-'0';
    }
    else if(c>='A' && c<='Z') {
        return (uint8_t)c-'A'+0xa;
    }
    else if(c>='a' && c<='z') {
        return (uint8_t)c-'a'+0xa;
    }
    log_error("Char Fail");
    return (uint8_t)c;
}
// MQTT callback function
static void mqtt_message_callback(const char* topic, uint8_t* payload, int payload_length)
{
    static int rc5_toggle=0;

    uint32_t data = 0;
    unsigned int i;
    payload[payload_length] = 0;
    log_debug("mqtt cb %s : %s", topic, payload);

    // TODO: switch on topic
    if(payload_length>8) {
        log_error("tv cmd too long (%d)", payload_length);
        return;
    }

    // Parse tv cmd data
    for(i=1; i<payload_length; i++) {
        data |= ((uint32_t)char2hex(payload[i])) << ((payload_length-i-1)*4);
    }

    switch (payload[0]) {
    case 's': // RC5 code
        // Update second start bit for normal RC5 cmd set
        if(payload[0] == 's') {
            data |= 0x1000;
        }
    case 't': // RC5 (7bits) code
        if(rc5_toggle) {
            data &= 0x17FF;
            rc5_toggle = 0;
        }
        else {
            data |= 0x800;
            rc5_toggle = 1;
        }
        ir_sendRC5(data, (payload_length-1)*4+1);
        ir_sendRC5(data, (payload_length-1)*4+1);
        break;

    case 'c': // Canal code
        data &= 0xFFF0FF;
        ir_sendCanal(data, (payload_length-1)*4-1);
        data |= 0x000100;
        ir_sendCanal(data, (payload_length-1)*4-1);
        break;

    case 'a': // Apple TV code
        ir_sendNEC(data, (payload_length-1)*4); // Cannot toggle : it sends the cmd twice
        break;

    default:
        log_error("Unknow command type %c", payload[0]);
    }
}

// MQTT connect and subscribe function
static int mqtt_main_connect(struct mqtt_client* mqtt_client, uip_ipaddr_t *server)
{
    static enum {
        MQTT_DISCONNECTED,
        MQTT_CONNECTING,
        MQTT_SUBSCRIBING_1,
        MQTT_SUBSCRIBING_2,
        MQTT_CONNECTED
    } mqtt_state = MQTT_DISCONNECTED;

    if (mqtt_get_state(mqtt_client) == MQTT_STATE_DISCONNECTED) {
        // MQTT disconnected, reset mqtt_state
        mqtt_state = MQTT_DISCONNECTED;
    }

    switch (mqtt_state) {
        case MQTT_CONNECTED:
            return 1;
        case MQTT_DISCONNECTED:
            // Send connection request
            mqtt_connect(mqtt_client, server, HTONS(MQTT_DEFAULT_PORT));
            mqtt_state++;
            return 0;
        case MQTT_CONNECTING:
            if (mqtt_is_connect_done(mqtt_client)) {
                mqtt_state++;
                // Subscribe to TV cmds channel
                mqtt_subscribe(mqtt_client, "home/tv");
            }
            return 0;
        case MQTT_SUBSCRIBING_1:
            if (mqtt_is_subscribe_done(mqtt_client)) {
                mqtt_state++;
                // Subscribe to home test channel
                mqtt_subscribe(mqtt_client, "home/test");
            }
            return 0;
        case MQTT_SUBSCRIBING_2:
            if (mqtt_is_subscribe_done(mqtt_client)) {
                mqtt_state++;
                log_info("mqtt connected (%d)", mqtt_get_state(mqtt_client));
            }
            return 0;
    }

    return 0;
}
