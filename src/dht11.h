/*
 * sensor-dht11 - Read DHT11 sensors on Raspberry Pi
 * Copyright (C) 2024 Wildlife Systems
 *
 * Header file for DHT11 sensor reading functionality, through the Linux GPIO
 * character device
 */

#ifndef DHT11_H
#define DHT11_H

#include <stdbool.h>
#include <stdint.h>
#include <ws_utils.h>

/* Version information - passed via -DVERSION from Makefile (extracted from debian/changelog) */
#ifndef VERSION
#define VERSION "unknown"
#endif
#define VERSION_STRING VERSION

/* Time allowed for reading every configured sensor, retries included. sr
   sends SIGTERM after 10 seconds, so a driver that takes longer is killed
   before it can report anything: the reading, and the error explaining why
   it failed, are both lost. 8 leaves a margin for sc-prototype and the
   reads themselves. The budget is shared between the sensors being read. */
#define DHT11_READ_BUDGET_SEC  8

/* Watchdog for the whole invocation (seconds). A last resort for a GPIO call
   that hangs outright; the retry budget above is what bounds a normal
   failure, and expires long before this does. */
#define WATCHDOG_TIMEOUT_SEC  30

/* Default configuration */
#define DEFAULT_PIN       4
#define CONFIG_PATH       WS_CONFIG_PATH("dht11")

/* Sensor configuration structure.
 * The fields every driver shares live in base; pin is ours alone. */
typedef struct {
    ws_sensor_config_base_t base;
    int pin;
} sensor_config_t;

/* Sensor reading structure */
typedef struct {
    float temperature;
    float humidity;
    bool valid;
    char error_msg[128];
} sensor_reading_t;

/* Function prototypes */
int read_dht11(int gpio_pin, sensor_reading_t *reading, unsigned long budget_us);
sensor_config_t *load_config(const char *path, int *count);
void free_config(sensor_config_t *configs, int count);
int output_json(sensor_config_t *configs, int count, const char *filter, ws_location_filter_t location_filter);

#endif /* DHT11_H */
