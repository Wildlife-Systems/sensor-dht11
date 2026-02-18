/*
 * sensor-dht11 - Read DHT11 sensors on Raspberry Pi
 * Copyright (C) 2024 Wildlife Systems
 *
 * C implementation using libgpiod for GPIO bit-banging.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sched.h>
#include <stdbool.h>
#include <signal.h>
#include <syslog.h>
#include <gpiod.h>

#include "dht11.h"
#include <ws_utils.h>

/* Global state for signal handler cleanup */
static volatile sig_atomic_t g_running = 1;
static struct gpiod_chip *g_chip = NULL;
static struct gpiod_line *g_line = NULL;

/* DHT11 timing constants (microseconds) */
#define DHT11_START_LOW_US      20000   /* Start signal: pull low for 20ms */
#define DHT11_START_HIGH_US     20      /* Then release for 20-40us */
#define DHT11_TIMEOUT_US        1000    /* Timeout waiting for edges */

/* GPIO chip for Raspberry Pi */
#define GPIO_CHIP_PATH  "/dev/gpiochip0"

/* Runtime data directory for service mode */
#define RUN_DIR_BASE    "/run/ws/dht"

/*
 * Get current time in microseconds
 */
static uint64_t micros(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/*
 * Log error to both stderr and syslog
 */
static void log_error(const char *fmt, ...) {
    va_list args;
    char buf[256];
    
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    fprintf(stderr, "%s\n", buf);
    syslog(LOG_ERR, "%s", buf);
}

/*
 * Signal handler for graceful cleanup
 */
static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
    
    /* Release GPIO resources if held */
    if (g_line) {
        gpiod_line_release(g_line);
        g_line = NULL;
    }
    if (g_chip) {
        gpiod_chip_close(g_chip);
        g_chip = NULL;
    }
    
    syslog(LOG_INFO, "Caught signal, exiting");
    closelog();
    _exit(1);
}

/*
 * Setup signal handlers
 */
static void setup_signal_handlers(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/*
 * Watchdog alarm handler - triggers if GPIO operations hang
 */
static void watchdog_handler(int sig) {
    (void)sig;
    log_error("Watchdog timeout - GPIO operations hung");
    
    /* Release GPIO resources if held */
    if (g_line) {
        gpiod_line_release(g_line);
        g_line = NULL;
    }
    if (g_chip) {
        gpiod_chip_close(g_chip);
        g_chip = NULL;
    }
    
    closelog();
    _exit(1);
}

/*
 * Setup watchdog timer
 */
static void setup_watchdog(void) {
    struct sigaction sa;
    sa.sa_handler = watchdog_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGALRM, &sa, NULL);
    alarm(WATCHDOG_TIMEOUT_SEC);
}

/*
 * Cancel watchdog timer
 */
static void cancel_watchdog(void) {
    alarm(0);
}

/*
 * Wait for a specific GPIO level with timeout
 * Returns the duration in microseconds, or -1 on timeout
 */
static int wait_for_level(struct gpiod_line *line, int level, int timeout_us) {
    uint64_t start = micros();
    uint64_t deadline = start + timeout_us;
    int current;
    
    while ((current = gpiod_line_get_value(line)) != level) {
        if (current < 0) {
            return -2;  /* Error reading GPIO */
        }
        if (micros() > deadline) {
            return -1;  /* Timeout */
        }
    }
    return (int)(micros() - start);
}

/*
 * Read DHT11 sensor using bit-banging
 * Returns 0 on success, -1 on error
 * If error_msg is provided, sets descriptive error message
 */
static int dht11_read_raw(int gpio_pin, uint8_t data[5], char *error_msg, size_t error_len) {
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int i, j;
    
    /* Check if we should stop */
    if (!g_running) {
        return -1;
    }
    
    /* Open GPIO chip */
    chip = gpiod_chip_open(GPIO_CHIP_PATH);
    if (!chip) {
        log_error("Failed to open GPIO chip %s", GPIO_CHIP_PATH);
        fprintf(stderr, "Hint: Try running with sudo for GPIO access\n");
        if (error_msg) {
            snprintf(error_msg, error_len, "GPIO access denied - try running with sudo");
        }
        return -1;
    }
    g_chip = chip;  /* Store for signal handler cleanup */
    
    /* Get the GPIO line */
    line = gpiod_chip_get_line(chip, gpio_pin);
    if (!line) {
        log_error("Failed to get GPIO line %d", gpio_pin);
        if (error_msg) {
            snprintf(error_msg, error_len, "Failed to get GPIO line %d", gpio_pin);
        }
        gpiod_chip_close(chip);
        g_chip = NULL;
        return -1;
    }
    g_line = line;  /* Store for signal handler cleanup */
    
    /* === SEND START SIGNAL === */
    
    /* Request line as output, initially high */
    if (gpiod_line_request_output(line, "dht11", 1) < 0) {
        log_error("Cannot request GPIO %d as output: %s", gpio_pin, strerror(errno));
        fprintf(stderr, "Hint: Try running with sudo for GPIO access\n");
        if (error_msg) {
            snprintf(error_msg, error_len, "GPIO access denied - try running with sudo");
        }
        gpiod_chip_close(chip);
        return -1;
    }
    
    /* Pull low for at least 18ms to signal start */
    gpiod_line_set_value(line, 0);
    usleep(DHT11_START_LOW_US);
    
    /* Pull high and wait for DHT11 response */
    gpiod_line_set_value(line, 1);
    usleep(DHT11_START_HIGH_US);
    
    /* Release line and switch to input */
    gpiod_line_release(line);
    if (gpiod_line_request_input(line, "dht11") < 0) {
        log_error("Cannot request GPIO %d as input: %s", gpio_pin, strerror(errno));
        fprintf(stderr, "Hint: Try running with sudo for GPIO access\n");
        if (error_msg) {
            snprintf(error_msg, error_len, "GPIO access denied - try running with sudo");
        }
        gpiod_chip_close(chip);
        g_line = NULL;
        g_chip = NULL;
        return -1;
    }
    
    /* === WAIT FOR DHT11 RESPONSE === */
    
    /* DHT11 response: LOW for ~80us, then HIGH for ~80us, then LOW for first bit */
    /* Wait for response LOW */
    if (wait_for_level(line, 0, DHT11_TIMEOUT_US) < 0) {
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        g_line = NULL;
        g_chip = NULL;
        return -1;
    }
    
    /* Wait for response HIGH */
    if (wait_for_level(line, 1, DHT11_TIMEOUT_US) < 0) {
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        g_line = NULL;
        g_chip = NULL;
        return -1;
    }
    
    /* Wait for first data bit LOW (start of bit) */
    if (wait_for_level(line, 0, DHT11_TIMEOUT_US) < 0) {
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        g_line = NULL;
        g_chip = NULL;
        return -1;
    }
    
    /* === READ 40 BITS OF DATA === */
    /* Each bit: LOW for ~50us, then HIGH for 26-28us (0) or 70us (1)
     * Measure the HIGH pulse width to determine bit value */
    
    memset(data, 0, 5);
    int pulse_times[50];
    int num_pulses = 0;
    
    /* Read all available pulses */
    for (i = 0; i < 50; i++) {
        /* Wait for HIGH with timeout */
        int high_result = wait_for_level(line, 1, DHT11_TIMEOUT_US);
        if (high_result < 0) {
            break;  /* No more bits */
        }
        
        /* Measure how long the HIGH lasts */
        uint64_t start = micros();
        wait_for_level(line, 0, DHT11_TIMEOUT_US);
        int duration = (int)(micros() - start);
        
        pulse_times[num_pulses++] = duration;
        
        /* Stop if we hit a long timeout (line staying HIGH = end of data) */
        if (duration > 500) {
            break;
        }
    }
    
    /* Count valid pulses (not timeouts) */
    int valid_pulses = 0;
    for (i = 0; i < num_pulses; i++) {
        if (pulse_times[i] < 500) valid_pulses++;
    }
    
    /* We need at least 38 valid pulses - may be missing 1-2 due to timing */
    if (valid_pulses < 38) {
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        g_line = NULL;
        g_chip = NULL;
        return -1;
    }
    
    /* Find threshold from valid pulses */
    int min_pulse = 10000, max_pulse = 0;
    for (i = 0; i < num_pulses; i++) {
        if (pulse_times[i] < 500) {
            if (pulse_times[i] < min_pulse) min_pulse = pulse_times[i];
            if (pulse_times[i] > max_pulse) max_pulse = pulse_times[i];
        }
    }
    int threshold = (min_pulse + max_pulse) / 2;
    
    /* Extract valid pulses into a separate array */
    int valid_times[50];
    int v = 0;
    for (i = 0; i < num_pulses && v < 50; i++) {
        if (pulse_times[i] < 500) {
            valid_times[v++] = pulse_times[i];
        }
    }
    
    /* Decode bits - use last valid_pulses bits, treating them as rightmost */
    /* This handles the case where we missed 1-2 bits at the start */
    int bits_missing = 40 - valid_pulses;
    int bit_idx = 0;
    
    /* First fill in zeros for missing bits */
    for (i = 0; i < bits_missing; i++) {
        j = bit_idx / 8;
        data[j] <<= 1;
        bit_idx++;
    }
    
    /* Then decode the pulses we have */
    for (i = 0; i < valid_pulses && bit_idx < 40; i++) {
        j = bit_idx / 8;
        data[j] <<= 1;
        if (valid_times[i] > threshold) {
            data[j] |= 1;
        }
        bit_idx++;
    }
    
    gpiod_line_release(line);
    gpiod_chip_close(chip);
    g_line = NULL;
    g_chip = NULL;
    
    /* Verify checksum */
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        return -1;
    }
    
    return 0;
}

/* Retry delays in microseconds: 0.05s x2, 0.1s x3, then 0.2, 0.4, 0.8, 1.6, 2s x3 */
static const useconds_t retry_delays_us[] = {
    50000, 50000,             /* 0.05s x2 */
    100000, 100000, 100000,   /* 0.1s x3 */
    200000, 400000, 800000, 1600000,  /* exponential */
    2000000, 2000000, 2000000  /* 2s x3 */
};
static const int num_retries = sizeof(retry_delays_us) / sizeof(retry_delays_us[0]);

/*
 * Read DHT11 with retries using predefined backoff schedule.
 * Elevates to SCHED_FIFO real-time priority during reads for reliable
 * GPIO timing, then restores normal scheduling afterward.
 */
int read_dht11(int gpio_pin, sensor_reading_t *reading) {
    uint8_t data[5];
    int attempt;
    struct sched_param rt_param = { .sched_priority = 99 };
    struct sched_param normal_param = { .sched_priority = 0 };
    int had_rt = 0;
    
    reading->valid = false;
    reading->error_msg[0] = '\0';
    
    /* Elevate to real-time FIFO scheduling for reliable GPIO timing */
    if (sched_setscheduler(0, SCHED_FIFO, &rt_param) == 0) {
        had_rt = 1;
    }
    
    for (attempt = 0; attempt <= num_retries; attempt++) {
        if (dht11_read_raw(gpio_pin, data, reading->error_msg, sizeof(reading->error_msg)) == 0) {
            /* DHT11 format: data[0]=humidity int, data[1]=humidity dec (always 0)
             *               data[2]=temp int, data[3]=temp dec (always 0)
             *               data[4]=checksum */
            reading->humidity = (float)data[0] + (float)data[1] / 10.0f;
            reading->temperature = (float)data[2] + (float)data[3] / 10.0f;
            reading->valid = true;
#ifdef DEBUG
            fprintf(stderr, "DEBUG: Success on attempt %d\\n", attempt + 1);
#endif
            /* Restore normal scheduling */
            if (had_rt)
                sched_setscheduler(0, SCHED_OTHER, &normal_param);
            return 0;
        }
        
        /* If we got a permission error, don't retry - it won't help */
        if (reading->error_msg[0] != '\0') {
            if (had_rt)
                sched_setscheduler(0, SCHED_OTHER, &normal_param);
            return -1;
        }
        
#ifdef DEBUG
        fprintf(stderr, "DEBUG: Attempt %d failed\\n", attempt + 1);
#endif
        
        /* Wait before next attempt (if not the last) */
        if (attempt < num_retries) {
            usleep(retry_delays_us[attempt]);
        }
    }
    
    /* Only set generic error if no specific error was set */
    if (reading->error_msg[0] == '\0') {
        snprintf(reading->error_msg, sizeof(reading->error_msg),
                 "Failed to read DHT11 after %d attempts", attempt);
    }
    /* Restore normal scheduling */
    if (had_rt)
        sched_setscheduler(0, SCHED_OTHER, &normal_param);
    return -1;
}

/*
 * Get Raspberry Pi serial number with _dht11 suffix.
 * Returns dynamically allocated string, caller must free.
 */
static char *get_serial_number(void) {
    char *raw_serial = ws_get_serial_number();
    if (!raw_serial) {
        return NULL;
    }
    
    /* Allocate space for raw serial + "_dht11" + null */
    size_t len = strlen(raw_serial) + 7;
    char *result = malloc(len);
    if (!result) {
        free(raw_serial);
        return NULL;
    }
    
    snprintf(result, len, "%s_dht11", raw_serial);
    free(raw_serial);
    return result;
}

/* json_escape_string is now provided by ws_utils.h as ws_json_escape_string */

/*
 * Count sensor objects in JSON buffer
 */
static int count_sensors_in_json(const char *buffer) {
    int count = 0;
    const char *ptr = buffer;
    
    while ((ptr = strchr(ptr, '{')) != NULL) {
        count++;
        ptr++;
    }
    return count;
}

/*
 * Parse a simple JSON config file - returns dynamically allocated array
 */
sensor_config_t *load_config(const char *path, int *count) {
    FILE *fp;
    char *buffer = NULL;
    char *ptr;
    int sensor_idx = 0;
    sensor_config_t *configs = NULL;
    int sensor_count;
    long file_size;
    
    *count = 0;
    
    fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }
    
    /* Get file size */
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0) {
        fclose(fp);
        return NULL;
    }
    
    /* Allocate buffer for file contents */
    buffer = malloc(file_size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }
    
    size_t bytes_read = fread(buffer, 1, file_size, fp);
    buffer[bytes_read] = '\0';
    fclose(fp);
    
    /* Count sensors and allocate */
    sensor_count = count_sensors_in_json(buffer);
    if (sensor_count == 0) {
        free(buffer);
        return NULL;
    }
    
    configs = malloc(sensor_count * sizeof(sensor_config_t));
    if (!configs) {
        free(buffer);
        return NULL;
    }
    
    ptr = buffer;
    while ((ptr = strchr(ptr, '{')) != NULL && sensor_idx < sensor_count) {
        char *end = strchr(ptr, '}');
        if (!end) break;
        
        configs[sensor_idx].pin = DEFAULT_PIN;
        configs[sensor_idx].internal = false;
        configs[sensor_idx].sensor_id = NULL;
        configs[sensor_idx].sensor_name = NULL;
        
        char *pin_ptr = strstr(ptr, "\"pin\"");
        if (pin_ptr && pin_ptr < end) {
            pin_ptr = strchr(pin_ptr, ':');
            if (pin_ptr) {
                int parsed_pin = atoi(pin_ptr + 1);
                if (ws_validate_gpio_pin(parsed_pin)) {
                    configs[sensor_idx].pin = parsed_pin;
                } else {
                    log_error("Invalid GPIO pin %d (must be 2-27), using default %d",
                              parsed_pin, DEFAULT_PIN);
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
                        configs[sensor_idx].sensor_id = strndup(quote_start, id_len);
                    }
                }
            }
        }
        
        /* If no sensor_id in config, use Pi serial with _dht11 suffix */
        if (configs[sensor_idx].sensor_id == NULL) {
            configs[sensor_idx].sensor_id = get_serial_number();
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
                        configs[sensor_idx].sensor_name = strndup(quote_start, name_len);
                    }
                }
            }
        }
        
        sensor_idx++;
        ptr = end + 1;
    }
    
    free(buffer);
    *count = sensor_idx;
    return configs;
}

/*
 * Free dynamically allocated config array and its string members
 */
void free_config(sensor_config_t *configs, int count) {
    if (configs) {
        for (int i = 0; i < count; i++) {
            free(configs[i].sensor_id);
            free(configs[i].sensor_name);
        }
        free(configs);
    }
}

/* get_prototype is now provided by ws_utils.h as ws_get_prototype_cached */

/*
 * Replace a JSON field value in a template string (in-place)
 * Looks for "field": and replaces the value after it
 * For string fields, the value should NOT include quotes - they're preserved from original
 */
static void json_replace_field(char *json, size_t json_len, const char *field, const char *value) {
    char search[128];
    char *pos;
    char *value_start;
    char *value_end = NULL;
    size_t new_len, tail_len;
    size_t prefix_len;
    
    snprintf(search, sizeof(search), "\"%s\":", field);
    pos = strstr(json, search);
    if (!pos) {
        snprintf(search, sizeof(search), "\"%s\" :", field);
        pos = strstr(json, search);
    }
    if (!pos) return;
    
    /* Find the colon */
    value_start = strchr(pos, ':');
    if (!value_start) return;
    value_start++;
    
    /* Skip whitespace */
    while (*value_start == ' ' || *value_start == '\t') value_start++;
    
    /* Find end of value based on type */
    if (*value_start == '"') {
        /* String value - keep the quotes, just replace content between them */
        value_start++;  /* Move past opening quote */
        value_end = value_start;
        while (*value_end && *value_end != '"') {
            if (*value_end == '\\' && *(value_end + 1)) value_end++;
            value_end++;
        }
        /* value_end now points to closing quote - don't include it */
    } else if (strncmp(value_start, "null", 4) == 0) {
        value_end = value_start + 4;
    } else if (strncmp(value_start, "true", 4) == 0) {
        value_end = value_start + 4;
    } else if (strncmp(value_start, "false", 5) == 0) {
        value_end = value_start + 5;
    } else {
        /* Number - find comma or closing brace */
        value_end = value_start;
        while (*value_end && *value_end != ',' && *value_end != '}') value_end++;
    }
    
    if (!value_end) return;
    
    /* Calculate sizes */
    new_len = strlen(value);
    prefix_len = value_start - json;
    tail_len = strlen(value_end) + 1;  /* Include null terminator */
    
    /* Check if new string fits */
    if (prefix_len + new_len + tail_len > json_len) {
        return;  /* Would overflow */
    }
    
    /* Shift tail in-place, then copy new value */
    memmove(value_start + new_len, value_end, tail_len);
    memcpy(value_start, value, new_len);
}

/*
 * Build a sensor JSON object from the sc-prototype template
 * If error_msg is not NULL, value is set to null and error is populated
 */
/*
 * Build a sensor JSON object from the sc-prototype template
 * If error_msg is not NULL, value is set to null and error is populated
 * timestamp is the Unix timestamp when the sensor was read
 * sensor_name is the configurable name for the sensor_name field
 */
static void build_sensor_json(char *output, size_t output_len,
                               const char *sensor, const char *measures, const char *unit,
                               float value, bool internal, const char *sensor_id,
                               const char *sensor_name, const char *error_msg, time_t timestamp) {
    const char *prototype = ws_get_prototype_cached();
    char value_str[32];
    char quoted[512];
    char timestamp_str[32];
    
    if (!prototype || !*prototype) {
        log_error("sc-prototype failed - cannot generate JSON");
        output[0] = '\0';
        return;
    }
    
    /* Start with a copy of the prototype */
    strncpy(output, prototype, output_len - 1);
    output[output_len - 1] = '\0';
    
    /* Replace string fields - need to add quotes since prototype has null */
    snprintf(quoted, sizeof(quoted), "\"%s\"", sensor);
    json_replace_field(output, output_len, "sensor", quoted);
    
    snprintf(quoted, sizeof(quoted), "\"%s\"", measures);
    json_replace_field(output, output_len, "measures", quoted);
    
    snprintf(quoted, sizeof(quoted), "\"%s\"", unit);
    json_replace_field(output, output_len, "unit", quoted);
    
    snprintf(quoted, sizeof(quoted), "\"%s\"", sensor_id);
    json_replace_field(output, output_len, "sensor_id", quoted);
    
    /* Only replace sensor_name if config provides one, otherwise keep prototype default */
    if (sensor_name && sensor_name[0] != '\0') {
        snprintf(quoted, sizeof(quoted), "\"%s\"", sensor_name);
        json_replace_field(output, output_len, "sensor_name", quoted);
    }
    
    json_replace_field(output, output_len, "internal", internal ? "true" : "false");
    
    /* Add timestamp */
    snprintf(timestamp_str, sizeof(timestamp_str), "%ld", (long)timestamp);
    json_replace_field(output, output_len, "timestamp", timestamp_str);
    
    if (error_msg) {
        char escaped_error[256];
        ws_json_escape_string(error_msg, escaped_error, sizeof(escaped_error));
        json_replace_field(output, output_len, "value", "null");
        snprintf(quoted, sizeof(quoted), "\"%s\"", escaped_error);
        json_replace_field(output, output_len, "error", quoted);
    } else {
        snprintf(value_str, sizeof(value_str), "%.1f", value);
        json_replace_field(output, output_len, "value", value_str);
        json_replace_field(output, output_len, "error", "null");
    }
}

/*
 * Read a small text file into a buffer, stripping trailing newlines.
 * Returns 0 on success, -1 on error.
 */
static int read_run_file(const char *path, char *buf, size_t buf_len) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    size_t n = fread(buf, 1, buf_len - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    /* Strip trailing whitespace/newlines */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    return 0;
}

/*
 * Attempt to load cached sensor data from /run/ws/dht/sensorX/.
 * Populates the reading struct with temperature and humidity if available.
 * Sets read_timestamp from the cached timestamp file.
 * Returns 0 on success (cached data available), -1 if no cache.
 */
static int load_cached_reading(int sensor_index, sensor_reading_t *reading, time_t *cached_timestamp) {
    char path[576];
    char buf[256];

    snprintf(path, sizeof(path), "%s/sensor%d/temperature", RUN_DIR_BASE, sensor_index);
    if (read_run_file(path, buf, sizeof(buf)) != 0) return -1;
    reading->temperature = strtof(buf, NULL);

    snprintf(path, sizeof(path), "%s/sensor%d/humidity", RUN_DIR_BASE, sensor_index);
    if (read_run_file(path, buf, sizeof(buf)) != 0) return -1;
    reading->humidity = strtof(buf, NULL);

    snprintf(path, sizeof(path), "%s/sensor%d/timestamp", RUN_DIR_BASE, sensor_index);
    if (read_run_file(path, buf, sizeof(buf)) == 0) {
        *cached_timestamp = (time_t)strtol(buf, NULL, 10);
    } else {
        return -1;  /* No timestamp - cannot verify age, reject */
    }

    /* Reject cached data older than 10 minutes */
    if ((time(NULL) - *cached_timestamp) > 600) {
        return -1;
    }

    reading->valid = true;
    return 0;
}

/*
 * Output sensor reading as JSON
 */
void output_json(sensor_config_t *configs, int count, const char *filter, ws_location_filter_t location_filter) {
    size_t output_size = 4096;  /* Initial size, will grow if needed */
    char *output = malloc(output_size);
    if (!output) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }
    strcpy(output, "[");
    int first = 1;
    int i;
    
    for (i = 0; i < count; i++) {
        sensor_reading_t reading;
        size_t id_len = configs[i].sensor_id ? strlen(configs[i].sensor_id) : 0;
        char *escaped_id = malloc(id_len * 2 + 1);
        if (!escaped_id) {
            free(output);
            fprintf(stderr, "Memory allocation failed\n");
            return;
        }
        const char *error_msg = NULL;
        const char *cached_warning = NULL;
        time_t read_timestamp;
        char cached_error_buf[256];
        
        /* Skip sensors that don't match the location filter */
        if (location_filter == WS_LOCATION_INTERNAL && !configs[i].internal) {
            free(escaped_id);
            continue;
        }
        if (location_filter == WS_LOCATION_EXTERNAL && configs[i].internal) {
            free(escaped_id);
            continue;
        }
        
        ws_json_escape_string(configs[i].sensor_id, escaped_id, id_len * 2 + 1);
        
        /* Capture timestamp when sensor is read */
        read_timestamp = time(NULL);
        
        if (read_dht11(configs[i].pin, &reading) != 0 || !reading.valid) {
            /* Live read failed - try to use cached data from /run/ws/dht/ */
            time_t cached_ts = 0;
            if (load_cached_reading(i, &reading, &cached_ts) == 0) {
                /* Have cached data - use its timestamp and note the fallback.
                 * Pass error_msg=NULL to build_sensor_json so the value is
                 * populated, then inject the warning into the error field. */
                if (cached_ts > 0) {
                    read_timestamp = cached_ts;
                }
                snprintf(cached_error_buf, sizeof(cached_error_buf),
                         "live read failed, using cached data from /run/ws/dht/sensor%d", i);
                cached_warning = cached_error_buf;
                syslog(LOG_WARNING, "sensor%d: live read failed, serving cached data (age %lds)",
                       i, (long)(time(NULL) - cached_ts));
            } else {
                /* No usable cache either */
                error_msg = reading.error_msg;
            }
        }
        
        if (!filter || strcmp(filter, "temperature") == 0 || strcmp(filter, "all") == 0) {
            char temp_json[1024];
            char *sensor_id_temp = malloc(id_len * 2 + 16);
            if (sensor_id_temp) {
                snprintf(sensor_id_temp, id_len * 2 + 16, "%s_temperature", escaped_id);
                
                build_sensor_json(temp_json, sizeof(temp_json),
                                  "dht11_temperature", "temperature", "Celsius",
                                  reading.temperature, configs[i].internal, sensor_id_temp,
                                  configs[i].sensor_name, error_msg, read_timestamp);
                
                /* If serving cached data, inject the warning into the error field
                 * without nullifying the value */
                if (cached_warning) {
                    char quoted_warn[512];
                    snprintf(quoted_warn, sizeof(quoted_warn), "\"%s\"", cached_warning);
                    json_replace_field(temp_json, sizeof(temp_json), "error", quoted_warn);
                }
                
                /* Grow output buffer if needed */
                size_t needed = strlen(output) + strlen(temp_json) + 3;
                if (needed > output_size) {
                    output_size = needed * 2;
                    char *new_output = realloc(output, output_size);
                    if (new_output) output = new_output;
                }
                
                if (!first) strcat(output, ",");
                strcat(output, temp_json);
                first = 0;
                free(sensor_id_temp);
            }
        }
        
        if (!filter || strcmp(filter, "humidity") == 0 || strcmp(filter, "all") == 0) {
            char humid_json[1024];
            char *sensor_id_humid = malloc(id_len * 2 + 16);
            if (sensor_id_humid) {
                snprintf(sensor_id_humid, id_len * 2 + 16, "%s_humidity", escaped_id);
                
                build_sensor_json(humid_json, sizeof(humid_json),
                                  "dht11_humidity", "humidity", "percentage",
                                  reading.humidity, configs[i].internal, sensor_id_humid,
                                  configs[i].sensor_name, error_msg, read_timestamp);
                
                /* If serving cached data, inject the warning into the error field
                 * without nullifying the value */
                if (cached_warning) {
                    char quoted_warn[512];
                    snprintf(quoted_warn, sizeof(quoted_warn), "\"%s\"", cached_warning);
                    json_replace_field(humid_json, sizeof(humid_json), "error", quoted_warn);
                }
                
                /* Grow output buffer if needed */
                size_t needed = strlen(output) + strlen(humid_json) + 3;
                if (needed > output_size) {
                    output_size = needed * 2;
                    char *new_output = realloc(output, output_size);
                    if (new_output) output = new_output;
                }
                
                if (!first) strcat(output, ",");
                strcat(output, humid_json);
                first = 0;
                free(sensor_id_humid);
            }
        }
        
        free(escaped_id);
    }
    
    strcat(output, "]");
    printf("%s\n", output);
    free(output);
}

/*
 * Ensure a directory path exists, creating parent directories as needed.
 * Returns 0 on success, -1 on error.
 */
static int mkdirs(const char *path, mode_t mode) {
    char tmp[512];
    char *p;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/*
 * Write a string to a file, creating/truncating it.
 * Returns 0 on success, -1 on error.
 */
static int write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        log_error("Failed to write %s: %s", path, strerror(errno));
        return -1;
    }
    fputs(content, fp);
    fclose(fp);
    return 0;
}

/*
 * Record sensor readings to /run/ws/dht/sensorX/ files.
 * For each sensor in the config, creates:
 *   /run/ws/dht/sensorX/temperature
 *   /run/ws/dht/sensorX/humidity
 *   /run/ws/dht/sensorX/timestamp
 *   /run/ws/dht/sensorX/sensor_id
 *   /run/ws/dht/sensorX/internal
 *   /run/ws/dht/sensorX/error      (only on read failure)
 */
static int write_to_run(sensor_config_t *configs, int count) {
    int i;
    int errors = 0;
    char dirpath[512];
    char filepath[576];
    char buf[256];
    time_t now;

    for (i = 0; i < count; i++) {
        sensor_reading_t reading;

        snprintf(dirpath, sizeof(dirpath), "%s/sensor%d", RUN_DIR_BASE, i);
        if (mkdirs(dirpath, 0755) != 0) {
            log_error("Failed to create directory %s: %s", dirpath, strerror(errno));
            errors++;
            continue;
        }

        /* Capture timestamp */
        now = time(NULL);

        /* Write sensor metadata */
        snprintf(filepath, sizeof(filepath), "%s/sensor_id", dirpath);
        write_file(filepath, configs[i].sensor_id ? configs[i].sensor_id : "unknown");

        snprintf(filepath, sizeof(filepath), "%s/internal", dirpath);
        write_file(filepath, configs[i].internal ? "true" : "false");

        snprintf(filepath, sizeof(filepath), "%s/timestamp", dirpath);
        snprintf(buf, sizeof(buf), "%ld", (long)now);
        write_file(filepath, buf);

        /* Attempt to read the sensor */
        if (read_dht11(configs[i].pin, &reading) == 0 && reading.valid) {
            snprintf(filepath, sizeof(filepath), "%s/temperature", dirpath);
            snprintf(buf, sizeof(buf), "%.1f", reading.temperature);
            write_file(filepath, buf);

            snprintf(filepath, sizeof(filepath), "%s/humidity", dirpath);
            snprintf(buf, sizeof(buf), "%.1f", reading.humidity);
            write_file(filepath, buf);

            /* Remove stale error file on success */
            snprintf(filepath, sizeof(filepath), "%s/error", dirpath);
            unlink(filepath);

            syslog(LOG_INFO, "sensor%d: temperature=%.1f humidity=%.1f",
                   i, reading.temperature, reading.humidity);
        } else {
            snprintf(filepath, sizeof(filepath), "%s/error", dirpath);
            write_file(filepath, reading.error_msg[0] ? reading.error_msg : "read failed");
            log_error("sensor%d: read failed: %s", i,
                      reading.error_msg[0] ? reading.error_msg : "unknown error");
            errors++;
        }
    }

    return errors > 0 ? -1 : 0;
}

int main(int argc, char *argv[]) {
    sensor_config_t *configs = NULL;
    sensor_config_t default_config;
    int config_count = 0;
    const char *filter = NULL;
    ws_location_filter_t location_filter = WS_LOCATION_ALL;
    
    /* Initialize syslog */
    openlog("sensor-dht11", LOG_PID | LOG_CONS, LOG_USER);
    
    /* Setup signal handlers for graceful cleanup */
    setup_signal_handlers();
    
    /* Setup watchdog to prevent hanging on GPIO issues */
    setup_watchdog();
    
    if (argc >= 2) {
        if (strcmp(argv[1], "identify") == 0) {
            ws_cmd_identify();
        } else if (strcmp(argv[1], "list") == 0) {
            static const char *measurements[] = {"temperature", "humidity", NULL};
            ws_cmd_list_multiple(measurements);
        } else if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0 ||
                   strcmp(argv[1], "version") == 0) {
            ws_print_version("sensor-dht11", VERSION_STRING);
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "record") == 0) {
            /* Service mode: read sensors and write to /run/ws/dht/sensorX/ */
            configs = load_config(CONFIG_PATH, &config_count);
            if (configs == NULL || config_count == 0) {
                char *serial = get_serial_number();
                default_config.pin = DEFAULT_PIN;
                default_config.internal = false;
                default_config.sensor_id = serial;
                default_config.sensor_name = NULL;
                configs = &default_config;
                config_count = 1;
            }
            int rc = write_to_run(configs, config_count);
            if (configs == &default_config) {
                free(default_config.sensor_id);
                free(default_config.sensor_name);
            } else {
                free_config(configs, config_count);
            }
            cancel_watchdog();
            closelog();
            return rc == 0 ? WS_EXIT_SUCCESS : 1;
        } else if (strcmp(argv[1], "enable") == 0) {
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "setup") == 0) {
            /* DHT11 has no setup requirements beyond the overlay */
            printf("DHT11 sensor requires no additional setup.\n");
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "mock") == 0) {
            /* Output mock data for testing without hardware */
            char *serial = ws_get_serial_with_suffix("dht11_mock");
            time_t now = time(NULL);
            char json[2048];
            printf("[");
            /* Temperature */
            if (ws_build_sensor_json_base(json, sizeof(json), "dht11_temperature", "temperature", "Celsius",
                                          serial, "Mock DHT11", false, now) == 0) {
                ws_sensor_json_set_value(json, 22.0, 1);
                printf("%s", json);
            }
            /* Humidity */
            char humid_id[128];
            snprintf(humid_id, sizeof(humid_id), "%s_humidity", serial);
            if (ws_build_sensor_json_base(json, sizeof(json), "dht11_humidity", "humidity", "percentage",
                                          humid_id, "Mock DHT11", false, now) == 0) {
                ws_sensor_json_set_value(json, 55.0, 1);
                printf(",%s", json);
            }
            printf("]\n");
            free(serial);
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "temperature") == 0 || 
                   strcmp(argv[1], "humidity") == 0) {
            filter = argv[1];
        } else if (strcmp(argv[1], "internal") == 0) {
            location_filter = WS_LOCATION_INTERNAL;
        } else if (strcmp(argv[1], "external") == 0) {
            location_filter = WS_LOCATION_EXTERNAL;
        } else if (strcmp(argv[1], "all") != 0) {
            fprintf(stderr, "Unknown command: %s\n", argv[1]);
            fprintf(stderr, "Usage: sensor-dht11 [--version|identify|list|setup|enable|mock|record|temperature|humidity|internal|external|all]\n");
            return WS_EXIT_INVALID_ARG;
        }
    }
    
    configs = load_config(CONFIG_PATH, &config_count);
    if (configs == NULL || config_count == 0) {
        /* Use default config - allocate dynamically for consistency */
        char *serial = get_serial_number();
        default_config.pin = DEFAULT_PIN;
        default_config.internal = false;
        default_config.sensor_id = serial;
        default_config.sensor_name = NULL;  /* NULL = use sc-prototype default */
        configs = &default_config;
        config_count = 1;
    }
    
    output_json(configs, config_count, filter, location_filter);
    
    /* Free config */
    if (configs == &default_config) {
        /* Free just the strings from stack-allocated default */
        free(default_config.sensor_id);
        free(default_config.sensor_name);
    } else {
        free_config(configs, config_count);
    }
    
    /* Cancel watchdog before normal exit */
    cancel_watchdog();
    
    closelog();
    return WS_EXIT_SUCCESS;
}
