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
#include <unistd.h>
#include <time.h>
#include <errno.h>
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

/*
 * Get current time in microseconds
 */
static uint64_t micros(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
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
    ws_log_error("Watchdog timeout - GPIO operations hung");
    
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
        ws_log_error("Failed to open GPIO chip %s", GPIO_CHIP_PATH);
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
        ws_log_error("Failed to get GPIO line %d", gpio_pin);
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
        ws_log_error("Cannot request GPIO %d as output: %s", gpio_pin, strerror(errno));
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
        ws_log_error("Cannot request GPIO %d as input: %s", gpio_pin, strerror(errno));
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

/* json_escape_string is now provided by ws_utils.h as ws_json_escape_string */

/*
 * Parse a simple JSON config file - returns dynamically allocated array
 */
sensor_config_t *load_config(const char *path, int *count) {
    ws_config_iter_t it;
    sensor_config_t *configs;
    const char *entry, *entry_end;
    int n, idx = 0;

    *count = 0;

    n = ws_config_iter_open(&it, path);
    if (n <= 0) {
        ws_config_iter_close(&it);
        return NULL;
    }

    configs = calloc((size_t)n, sizeof(*configs));
    if (!configs) {
        ws_config_iter_close(&it);
        return NULL;
    }

    while (ws_config_iter_next(&it, &configs[idx].base, &entry, &entry_end)) {
        int parsed_pin = ws_json_parse_int(entry, entry_end, "pin", DEFAULT_PIN);
        if (ws_validate_gpio_pin(parsed_pin)) {
            configs[idx].pin = parsed_pin;
        } else {
            ws_log_error("Invalid GPIO pin %d (must be 2-27), using default %d",
                         parsed_pin, DEFAULT_PIN);
            configs[idx].pin = DEFAULT_PIN;
        }

        /* No sensor_id in the config: fall back to the Pi serial. */
        if (configs[idx].base.sensor_id == NULL) {
            configs[idx].base.sensor_id = ws_get_serial_with_suffix("dht11");
        }

        idx++;
    }

    ws_config_iter_close(&it);
    *count = idx;
    return configs;
}

/*
 * Free dynamically allocated config array and its string members
 */
void free_config(sensor_config_t *configs, int count) {
    if (configs) {
        for (int i = 0; i < count; i++) {
            ws_sensor_config_free_fields(&configs[i].base);
        }
        free(configs);
    }
}

/* get_prototype is now provided by ws_utils.h as ws_get_prototype_cached */

/*
 * Build a sensor JSON object from the sc-prototype template
 * If error_msg is not NULL, value is left null and error is populated
 * timestamp is the Unix timestamp when the sensor was read
 * sensor_name is the configurable name for the sensor_name field
 */
static void build_sensor_json(char *output, size_t output_len,
                               const char *sensor, const char *measures, const char *unit,
                               float value, bool internal, const char *sensor_id,
                               const char *sensor_name, const char *error_msg, time_t timestamp,
                               const ws_location_t *location) {
    if (ws_build_sensor_json_base(output, output_len,
                                  sensor, "dht11", measures, unit,
                                  sensor_id, sensor_name,
                                  internal, location, timestamp) != 0) {
        ws_log_error("sc-prototype failed - cannot generate JSON");
        return;
    }
    
    /* Exactly one of value or error; the library escapes the message. */
    ws_sensor_json_set_result(output, output_len, (double)value, 1, error_msg);
}

/*
 * Output sensor reading as JSON
 */
/*
 * Build "<escaped_id>_<measurement>". Returns NULL if the allocation fails,
 * in which case the reading is skipped rather than emitted half-formed.
 */
static char *measurement_id(const char *escaped_id, const char *measurement) {
    size_t len;
    char *out;

    if (!escaped_id) return NULL;

    len = strlen(escaped_id) + strlen(measurement) + 2;  /* '_' and terminator */
    out = malloc(len);
    if (!out) return NULL;

    snprintf(out, len, "%s_%s", escaped_id, measurement);
    return out;
}

/*
 * Append one measurement of one sensor to the output array.
 * The sensor_id suffix is the measurement name, so temperature and humidity
 * differ only in the arguments.
 */
static void append_reading(ws_json_array_builder_t *out, const char *escaped_id,
                           const sensor_config_t *config, const char *sensor,
                           const char *measures, const char *unit, float value,
                           const char *error_msg, time_t timestamp) {
    char json[2048];
    char *sensor_id = measurement_id(escaped_id, measures);

    if (!sensor_id) return;

    build_sensor_json(json, sizeof(json), sensor, measures, unit, value,
                      config->base.internal, sensor_id, config->base.sensor_name,
                      error_msg, timestamp, &config->base.location);
    ws_json_array_add(out, json);
    free(sensor_id);
}

/*
 * Output sensor readings as a JSON array
 */
void output_json(sensor_config_t *configs, int count, const char *filter, ws_location_filter_t location_filter) {
    ws_json_array_builder_t out;
    const char *json;
    int i;

    if (ws_json_array_init(&out) != 0) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    for (i = 0; i < count; i++) {
        sensor_reading_t reading;
        const char *error_msg = NULL;
        time_t read_timestamp;
        size_t id_len;
        char *escaped_id;

        /* Skip sensors that do not match the location filter */
        if (location_filter == WS_LOCATION_INTERNAL && !configs[i].base.internal) continue;
        if (location_filter == WS_LOCATION_EXTERNAL && configs[i].base.internal) continue;

        /* Escaping can at most double the length. */
        id_len = configs[i].base.sensor_id ? strlen(configs[i].base.sensor_id) : 0;
        escaped_id = malloc(id_len * 2 + 1);
        if (!escaped_id) {
            fprintf(stderr, "Memory allocation failed\n");
            ws_json_array_free(&out);
            return;
        }
        ws_json_escape_string(configs[i].base.sensor_id, escaped_id, id_len * 2 + 1);

        /* Timestamp when the sensor was read */
        read_timestamp = time(NULL);

        if (read_dht11(configs[i].pin, &reading) != 0 || !reading.valid) {
            error_msg = reading.error_msg;
        }

        if (!filter || strcmp(filter, "temperature") == 0 || strcmp(filter, "all") == 0) {
            append_reading(&out, escaped_id, &configs[i], "dht11_temperature",
                           "temperature", WS_UNIT_CELSIUS, reading.temperature,
                           error_msg, read_timestamp);
        }

        if (!filter || strcmp(filter, "humidity") == 0 || strcmp(filter, "all") == 0) {
            append_reading(&out, escaped_id, &configs[i], "dht11_humidity",
                           "humidity", WS_UNIT_PERCENTAGE, reading.humidity,
                           error_msg, read_timestamp);
        }

        free(escaped_id);
    }

    ws_json_array_end(&out);
    json = ws_json_array_get(&out);
    if (json) {
        printf("%s\n", json);
    } else {
        fprintf(stderr, "Memory allocation failed\n");
    }
    ws_json_array_free(&out);
}

int main(int argc, char *argv[]) {
    sensor_config_t *configs = NULL;
    /* Zero-initialised: the default path sets each field explicitly except
       location, which must read as WS_LOC_UNDECLARED rather than whatever
       was on the stack. A garbage source of WS_LOC_EXPLICIT would emit a
       GeoJSON Point built from uninitialised coordinates. */
    sensor_config_t default_config = {0};
    int config_count = 0;
    const char *filter = NULL;
    ws_location_filter_t location_filter = WS_LOCATION_ALL;
    
    /* Initialize syslog */
    ws_log_init("sensor-dht11");
    
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
        } else if (strcmp(argv[1], "enable") == 0) {
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "setup") == 0) {
            /* Bit-banged over libgpiod, so no device-tree overlay and
               nothing to set up. */
            printf("DHT11 sensor requires no additional setup.\n");
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "mock") == 0) {
            /* Output mock data for testing without hardware */
            char *serial = ws_get_serial_with_suffix("dht11_mock");
            /* No Pi serial (an unreadable /proc/cpuinfo) must not reach "%s"
               as NULL. Mock exists to work without the hardware, so fall back
               to a fixed id rather than failing. */
            const char *base = serial ? serial : "dht11_mock";
            time_t now = time(NULL);
            char json[2048];
            printf("[");
            /* Temperature */
            if (ws_build_sensor_json_base(json, sizeof(json), "dht11_temperature", "dht11", "temperature", WS_UNIT_CELSIUS,
                                          base, "Mock DHT11", false, NULL, now) == 0) {
                ws_sensor_json_set_value(json, 22.0, 1);
                printf("%s", json);
            }
            /* Humidity */
            char humid_id[128];
            snprintf(humid_id, sizeof(humid_id), "%s_humidity", base);
            if (ws_build_sensor_json_base(json, sizeof(json), "dht11_humidity", "dht11", "humidity", WS_UNIT_PERCENTAGE,
                                          humid_id, "Mock DHT11", false, NULL, now) == 0) {
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
            fprintf(stderr, "Usage: sensor-dht11 [--version|identify|list|setup|enable|mock|temperature|humidity|internal|external|all]\n");
            return WS_EXIT_INVALID_ARG;
        }
    }
    
    configs = load_config(CONFIG_PATH, &config_count);
    if (configs == NULL || config_count == 0) {
        /* Use default config - allocate dynamically for consistency */
        char *serial = ws_get_serial_with_suffix("dht11");
        default_config.pin = DEFAULT_PIN;
        default_config.base.internal = false;
        default_config.base.sensor_id = serial;
        default_config.base.sensor_name = NULL;  /* NULL = use sc-prototype default */
        configs = &default_config;
        config_count = 1;
    }
    
    output_json(configs, config_count, filter, location_filter);
    
    /* Free config */
    if (configs == &default_config) {
        /* Free just the strings from stack-allocated default */
        free(default_config.base.sensor_id);
        free(default_config.base.sensor_name);
    } else {
        free_config(configs, config_count);
    }
    
    /* Cancel watchdog before normal exit */
    cancel_watchdog();
    
    closelog();
    return WS_EXIT_SUCCESS;
}
