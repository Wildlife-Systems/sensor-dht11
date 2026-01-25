/*
 * Testable subset of dht11.c
 * Contains functions that can be tested without hardware:
 * - load_config
 * - validate_gpio_pin
 * - get_serial_number (stubbed)
 * - JSON parsing utilities
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <syslog.h>

#include "../src/dht11.h"

/*
 * Stub for log_error - just prints to stderr in test mode
 */
static void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

/*
 * Validate GPIO pin is in valid range for Raspberry Pi
 */
bool validate_gpio_pin(int pin) {
    return (pin >= GPIO_PIN_MIN && pin <= GPIO_PIN_MAX);
}

/*
 * Stub for get_serial_number - returns test value
 */
int get_serial_number(char *serial, size_t len) {
    snprintf(serial, len, "test_serial_dht11");
    return 0;
}

/*
 * Print version information
 */
void print_version(void) {
    printf("sensor-dht11 version %s\n", VERSION_STRING);
    printf("Copyright (C) 2024 Wildlife Systems\n");
}

/*
 * Parse a simple JSON config file
 */
int load_config(const char *path, sensor_config_t *configs, int *count) {
    FILE *fp;
    char buffer[MAX_JSON_SIZE];
    char *ptr;
    int sensor_idx = 0;
    char default_serial[SERIAL_LEN] = "";
    
    *count = 0;
    
    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    
    get_serial_number(default_serial, sizeof(default_serial));
    
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, fp);
    buffer[bytes_read] = '\0';
    fclose(fp);
    
    ptr = buffer;
    while ((ptr = strchr(ptr, '{')) != NULL && sensor_idx < MAX_SENSORS) {
        char *end = strchr(ptr, '}');
        if (!end) break;
        
        configs[sensor_idx].pin = DEFAULT_PIN;
        configs[sensor_idx].internal = false;
        strncpy(configs[sensor_idx].sensor_id, default_serial, SENSOR_ID_LEN - 1);
        strncpy(configs[sensor_idx].sensor_name, "dht11", SENSOR_NAME_LEN - 1);
        
        char *pin_ptr = strstr(ptr, "\"pin\"");
        if (pin_ptr && pin_ptr < end) {
            pin_ptr = strchr(pin_ptr, ':');
            if (pin_ptr) {
                int parsed_pin = atoi(pin_ptr + 1);
                if (validate_gpio_pin(parsed_pin)) {
                    configs[sensor_idx].pin = parsed_pin;
                } else {
                    log_error("Invalid GPIO pin %d (must be %d-%d), using default %d",
                              parsed_pin, GPIO_PIN_MIN, GPIO_PIN_MAX, DEFAULT_PIN);
                }
            }
        }
        
        char *internal_ptr = strstr(ptr, "\"internal\"");
        if (internal_ptr && internal_ptr < end) {
            internal_ptr = strchr(internal_ptr, ':');
            if (internal_ptr) {
                while (*internal_ptr == ':' || *internal_ptr == ' ') internal_ptr++;
                configs[sensor_idx].internal = (strncmp(internal_ptr, "true", 4) == 0);
            }
        }
        
        char *id_ptr = strstr(ptr, "\"sensor_id\"");
        if (id_ptr && id_ptr < end) {
            id_ptr = strchr(id_ptr, ':');
            if (id_ptr) {
                char *quote_start = strchr(id_ptr, '"');
                if (quote_start && quote_start < end) {
                    quote_start++;
                    char *quote_end = strchr(quote_start, '"');
                    if (quote_end && quote_end < end) {
                        size_t id_len = quote_end - quote_start;
                        if (id_len >= SENSOR_ID_LEN) id_len = SENSOR_ID_LEN - 1;
                        strncpy(configs[sensor_idx].sensor_id, quote_start, id_len);
                        configs[sensor_idx].sensor_id[id_len] = '\0';
                    }
                }
            }
        }
        
        char *name_ptr = strstr(ptr, "\"sensor_name\"");
        if (name_ptr && name_ptr < end) {
            name_ptr = strchr(name_ptr, ':');
            if (name_ptr) {
                char *quote_start = strchr(name_ptr, '"');
                if (quote_start && quote_start < end) {
                    quote_start++;
                    char *quote_end = strchr(quote_start, '"');
                    if (quote_end && quote_end < end) {
                        size_t name_len = quote_end - quote_start;
                        if (name_len >= SENSOR_NAME_LEN) name_len = SENSOR_NAME_LEN - 1;
                        strncpy(configs[sensor_idx].sensor_name, quote_start, name_len);
                        configs[sensor_idx].sensor_name[name_len] = '\0';
                    }
                }
            }
        }
        
        sensor_idx++;
        ptr = end + 1;
    }
    
    *count = sensor_idx;
    return 0;
}

/* Stub functions for linking - not actually tested */
int read_dht11(int gpio_pin, sensor_reading_t *reading) {
    (void)gpio_pin;
    reading->valid = false;
    snprintf(reading->error_msg, sizeof(reading->error_msg), "Hardware not available in test mode");
    return -1;
}

void output_json(sensor_config_t *configs, int count, const char *filter, int location_filter) {
    (void)configs;
    (void)count;
    (void)filter;
    (void)location_filter;
}

void identify(void) {
    /* No-op in test mode */
}

void list_sensors(void) {
    printf("temperature\n");
    printf("humidity\n");
}
