/*
 * sensor-dht11 - Read DHT11 sensors on Raspberry Pi
 * Copyright (C) 2024 Wildlife Systems
 *
 * Header file for DHT11 sensor reading functionality using libgpiod
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

/* Watchdog timeout for entire read operation (seconds) */
#define WATCHDOG_TIMEOUT_SEC  30

/* Default configuration */
#define DEFAULT_PIN       4
#define CONFIG_PATH       "/etc/ws/sensors/dht11.json"

/* Sensor configuration structure */
typedef struct {
    int pin;
    bool internal;
    char *sensor_id;    /* Dynamically allocated */
    char *sensor_name;  /* Dynamically allocated, NULL if not set */
} sensor_config_t;

/* Sensor reading structure */
typedef struct {
    float temperature;
    float humidity;
    bool valid;
    char error_msg[128];
} sensor_reading_t;

/* Function prototypes */
int read_dht11(int gpio_pin, sensor_reading_t *reading);
sensor_config_t *load_config(const char *path, int *count);
void free_config(sensor_config_t *configs, int count);
void output_json(sensor_config_t *configs, int count, const char *filter, ws_location_filter_t location_filter);

#endif /* DHT11_H */
