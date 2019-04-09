// ----------------------------------------------------------------------------
// Copyright 2016-2018 ARM Ltd.
//
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ----------------------------------------------------------------------------
#ifndef MBED_TEST_MODE

#include "mbed.h"
#include "simple-mbed-cloud-client.h"
#include "FATFileSystem.h"
#include "LittleFileSystem.h"
#include "MMA7660FC.h"

#define ADDR_MMA7660 (0x4C << 1)

#if defined(TARGET_WIO_3G) || defined (TARGET_WIO_BG96)
DigitalOut GrovePower(GRO_POWR, 1);
DigitalOut SD_POWER(PA_15, 1);
#endif
#if defined(TARGET_NUCLEO_F767ZI)
MMA7660FC acc(PD_13, PD_12, ADDR_MMA7660);
#else
MMA7660FC acc(I2C_SDA, I2C_SCL, ADDR_MMA7660);
#endif
InterruptIn collision(COLISN_PIN);

int hits = 0;

// Default network interface object. Don't forget to change the WiFi SSID/password in mbed_app.json if you're using WiFi.
NetworkInterface *net = NetworkInterface::get_default_instance();

// Default block device available on the target board
BlockDevice *bd = BlockDevice::get_default_instance();

#if COMPONENT_SD || COMPONENT_NUSD
// Use FATFileSystem for SD card type blockdevices
FATFileSystem fs("fs", bd);
#else
// Use LittleFileSystem for non-SD block devices to enable wear leveling and other functions
LittleFileSystem fs("fs", bd);
#endif

// Default LED to use for PUT/POST example
DigitalOut led(LED1);

// Declaring pointers for access to Pelion Device Management Client resources outside of main()
MbedCloudClientResource *button_res;
MbedCloudClientResource *led_res;
MbedCloudClientResource *post_res;
MbedCloudClientResource *acc_x_res;
MbedCloudClientResource *acc_y_res;
MbedCloudClientResource *acc_z_res;

// An event queue is a very useful structure to debounce information between contexts (e.g. ISR and normal threads)
// This is great because things such as network operations are illegal in ISR, so updating a resource in a button's fall() function is not allowed
EventQueue eventQueue;

void update_sensors() {
    float x, y, z;
    acc.read_Tilt(&x, &y, &z);
    printf("x: %6.2f degree\n", x);
    printf("y: %6.2f degree\n", y);
    printf("z: %6.2f degree\n", z);
    acc_x_res->set_value(x);
    acc_y_res->set_value(y);
    acc_z_res->set_value(z);
    
    printf("Collision hit %d times\n", hits);
    button_res->set_value(hits);
}

/**
 * PUT handler - sets the value of the built-in LED
 * @param resource The resource that triggered the callback
 * @param newValue Updated value for the resource
 */
void put_callback(MbedCloudClientResource *resource, m2m::String newValue) {
    printf("PUT received. New value: %s\n", newValue.c_str());
    led = atoi(newValue.c_str());
}

/**
 * POST handler - prints the content of the payload
 * @param resource The resource that triggered the callback
 * @param buffer If a body was passed to the POST function, this contains the data.
 *               Note that the buffer is deallocated after leaving this function, so copy it if you need it longer.
 * @param size Size of the body
 */
void post_callback(MbedCloudClientResource *resource, const uint8_t *buffer, uint16_t size) {
    printf("POST received (length %u). Payload: ", size);
    for (size_t ix = 0; ix < size; ix++) {
        printf("%02x ", buffer[ix]);
    }
    printf("\n");
}

/**
 * Collision sensor handler
 * This function will be triggered either by an interrupt from collision sensor
 */
void hit_collision() {
    hits++;
}

/**
 * Notification callback handler
 * @param resource The resource that triggered the callback
 * @param status The delivery status of the notification
 */
void collision_callback(MbedCloudClientResource *resource, const NoticationDeliveryStatus status) {
    printf("Collision notification, status %s (%d)\n", MbedCloudClientResource::delivery_status_to_string(status), status);
}

/**
 * Registration callback handler
 * @param endpoint Information about the registered endpoint such as the name (so you can find it back in portal)
 */
void registered(const ConnectorClientEndpointInfo *endpoint) {
    printf("Registered to Pelion Device Management. Endpoint Name: %s\n", endpoint->internal_endpoint_name.c_str());
}

int main(void) {
    printf("\nStarting Simple Pelion Device Management Client example\n");
    acc.init();

#if USE_BUTTON == 1
    // If the User button is pressed ons start, then format storage.
    if (button.read() == MBED_CONF_APP_BUTTON_PRESSED_STATE) {
        printf("User button is pushed on start. Formatting the storage...\n");
        int storage_status = StorageHelper::format(&fs, bd);
        if (storage_status != 0) {
            printf("ERROR: Failed to reformat the storage (%d).\n", storage_status);
        }
    } else {
        printf("You can hold the user button during boot to format the storage and change the device identity.\n");
    }
#endif /* USE_BUTTON */

    // Connect to the Internet (DHCP is expected to be on)
    printf("Connecting to the network using the default network interface...\n");
    net = NetworkInterface::get_default_instance();

    nsapi_error_t net_status = NSAPI_ERROR_NO_CONNECTION;
    while ((net_status = net->connect()) != NSAPI_ERROR_OK) {
        printf("Unable to connect to network (%d). Retrying...\n", net_status);
    }

    printf("Connected to the network successfully. IP address: %s\n", net->get_ip_address());

    printf("Initializing Pelion Device Management Client...\n");

    // SimpleMbedCloudClient handles registering over LwM2M to Pelion Device Management
    SimpleMbedCloudClient client(net, bd, &fs);
    int client_status = client.init();
    if (client_status != 0) {
        printf("Pelion Client initialization failed (%d)\n", client_status);
        return -1;
    }

    // Creating resources, which can be written or read from the cloud
    button_res = client.create_resource("3200/0/5501", "collision_count");
    button_res->set_value(0);
    button_res->methods(M2MMethod::GET);
    button_res->observable(true);
    button_res->attach_notification_callback(collision_callback);

    led_res = client.create_resource("3201/0/5853", "led_state");
    led_res->set_value(led.read());
    led_res->methods(M2MMethod::GET | M2MMethod::PUT);
    led_res->attach_put_callback(put_callback);

    post_res = client.create_resource("3300/0/5605", "execute_function");
    post_res->methods(M2MMethod::POST);
    post_res->attach_post_callback(post_callback);

    acc_x_res = client.create_resource("3313/0/5702", "accelerometer_x");
    acc_x_res->set_value(0);
    acc_x_res->methods(M2MMethod::GET);
    acc_x_res->observable(true);

    acc_y_res = client.create_resource("3313/0/5703", "accelerometer_y");
    acc_y_res->set_value(0);
    acc_y_res->methods(M2MMethod::GET);
    acc_y_res->observable(true);

    acc_z_res = client.create_resource("3313/0/5704", "accelerometer_z");
    acc_z_res->set_value(0);
    acc_z_res->methods(M2MMethod::GET);
    acc_z_res->observable(true);

    printf("Initialized Pelion Device Management Client. Registering...\n");

    // Callback that fires when registering is complete
    client.on_registered(&registered);

    // Register with Pelion DM
    client.register_and_connect();

    collision.mode(PullUp);
    collision.fall(eventQueue.event(&hit_collision));

    Ticker timer;
    timer.attach(eventQueue.event(update_sensors), 3.0);

    // You can easily run the eventQueue in a separate thread if required
    eventQueue.dispatch_forever();
}

#endif /* MBED_TEST_MODE */
