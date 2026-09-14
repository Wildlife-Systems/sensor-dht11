/*
 * sensor-dht11 - Read DHT11 sensors on Raspberry Pi
 * Copyright (C) 2024 Wildlife Systems
 *
 * C implementation that bit-bangs the GPIO line through the Linux GPIO
 * character device; see gpio.h.
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

#include "dht11.h"
#include "gpio.h"
#include <ws_utils.h>

/* Global state for signal handler cleanup */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_line_fd = -1;  /* line held by a read, or -1 */

/* DHT11 timing constants (microseconds) */
#define DHT11_START_LOW_US      20000   /* Start signal: pull low for 20ms */
#define DHT11_TIMEOUT_US        1000    /* Timeout waiting for edges */

/* Readings no DHT11 can give. Current parts are rated for -20 to 60 C; the
 * limits leave a margin, so a frame outside them was misread. */
#define DHT11_TEMPERATURE_MIN   (-40.0f)
#define DHT11_TEMPERATURE_MAX   80.0f
#define DHT11_HUMIDITY_MAX      100.0f

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
    static const char msg[] = "sensor-dht11: caught signal, exiting\n";

    (void)sig;
    g_running = 0;

    /* Release the GPIO line if a read holds it. Only calls that are safe in
       a signal handler are made here, so the message goes to stderr through
       write() rather than to syslog. */
    dht_gpio_close(g_line_fd);
    if (write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0) {
        /* Nothing further can be done while exiting */
    }
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
    static const char msg[] = "sensor-dht11: watchdog timeout - GPIO operations hung\n";

    (void)sig;

    /* Release the GPIO line, making only calls that are safe in a signal
       handler */
    dht_gpio_close(g_line_fd);
    if (write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0) {
        /* Nothing further can be done while exiting */
    }
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
static int wait_for_level(int fd, int level, int timeout_us) {
    uint64_t start = micros();
    uint64_t deadline = start + timeout_us;
    int current;

    while ((current = dht_gpio_get(fd)) != level) {
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
 * Send the start signal on a held line and receive the DHT11's 40 bits.
 * Returns 0 with data filled and its checksum verified, or -1. The caller
 * holds the line and releases it whatever this returns.
 */
static int dht11_exchange(int fd, uint8_t data[5]) {
    int i, j;

    /* === SEND START SIGNAL === */

    /* Pull low for at least 18ms to signal start */
    if (dht_gpio_set(fd, 0) < 0) {
        ws_log_error("Cannot drive the GPIO line low: %s", strerror(errno));
        return -1;
    }
    usleep(DHT11_START_LOW_US);

    /* Release the line to its pull-up by switching it to input; the DHT11
       answers 20-40us later. Switching in place is a single call, where
       releasing the line and requesting it again took two and could miss
       the start of the answer. */
    if (dht_gpio_input(fd) < 0) {
        ws_log_error("Cannot switch the GPIO line to input: %s", strerror(errno));
        return -1;
    }

    /* === WAIT FOR DHT11 RESPONSE === */
    
    /* DHT11 response: LOW for ~80us, then HIGH for ~80us, then LOW for first bit */
    /* Wait for response LOW */
    if (wait_for_level(fd, 0, DHT11_TIMEOUT_US) < 0) {
        return -1;
    }
    
    /* Wait for response HIGH */
    if (wait_for_level(fd, 1, DHT11_TIMEOUT_US) < 0) {
        return -1;
    }
    
    /* Wait for first data bit LOW (start of bit) */
    if (wait_for_level(fd, 0, DHT11_TIMEOUT_US) < 0) {
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
        int high_result = wait_for_level(fd, 1, DHT11_TIMEOUT_US);
        if (high_result < 0) {
            break;  /* No more bits */
        }
        
        /* Measure how long the HIGH lasts */
        uint64_t start = micros();
        wait_for_level(fd, 0, DHT11_TIMEOUT_US);
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

    /* Verify checksum */
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        return -1;
    }
    
    return 0;
}

/*
 * Read DHT11 sensor using bit-banging
 * Returns 0 on success, -1 on error
 * If error_msg is provided, sets descriptive error message
 */
static int dht11_read_raw(int gpio_pin, uint8_t data[5], char *error_msg, size_t error_len) {
    char why[128];
    int fd;
    int ret;

    /* Check if we should stop */
    if (!g_running) {
        return -1;
    }

    /* Request the line as an output, initially high. Failing to get the line
       is not a timing fault, so the message it leaves tells read_dht11 not
       to retry. */
    fd = dht_gpio_open(gpio_pin, why, sizeof(why));
    if (fd < 0) {
        int err = errno;

        ws_log_error("%s", why);
        if (err == EACCES || err == EPERM) {
            fprintf(stderr, "Hint: Try running with sudo for GPIO access\n");
        }
        if (error_msg) {
            snprintf(error_msg, error_len, "%s", why);
        }
        return -1;
    }
    g_line_fd = fd;  /* Store for signal handler cleanup */

    ret = dht11_exchange(fd, data);

    /* Every read releases the line here, whatever the exchange returned */
    g_line_fd = -1;
    dht_gpio_close(fd);
    return ret;
}

/*
 * Decode a DHT11 frame whose checksum has been verified.
 *
 * Bytes 0 and 1 carry the humidity's whole part and tenths, bytes 2 and 3 the
 * temperature's. Parts rated 0 to 50 C send zero tenths. Parts rated down to
 * -20 C send the temperature's tenths in the low bits of byte 3 and set its
 * top bit below 0 C, when the temperature is the negative of its digits:
 * 02 83 is -2.3 C.
 *
 * Returns 0 with both values set, or -1, leaving them untouched, when a
 * tenths digit is above 9 or a value is outside what a DHT11 can report. The
 * caller treats that as a failed read.
 */
int dht11_decode(const uint8_t data[5], float *humidity, float *temperature) {
    int humidity_tenths = data[1];
    int temperature_tenths = data[3] & 0x7F;
    float h = (float)data[0] + (float)humidity_tenths / 10.0f;
    float t = (float)data[2] + (float)temperature_tenths / 10.0f;

    if (data[3] & 0x80) {
        t = -t;
    }
    if (humidity_tenths > 9 || temperature_tenths > 9 || h > DHT11_HUMIDITY_MAX ||
        t < DHT11_TEMPERATURE_MIN || t > DHT11_TEMPERATURE_MAX) {
        return -1;
    }
    *humidity = h;
    *temperature = t;
    return 0;
}

/* Retry delays in microseconds: 0.05s x2, 0.1s x3, then 0.2, 0.4, 0.8, 1.6, 2s x3.
 * The full schedule sleeps for 9.4 s, which on its own exceeds the 10 s sr
 * allows a driver; it is walked only as far as the caller's budget permits. */
static const useconds_t retry_delays_us[] = {
    50000, 50000,             /* 0.05s x2 */
    100000, 100000, 100000,   /* 0.1s x3 */
    200000, 400000, 800000, 1600000,  /* exponential */
    2000000, 2000000, 2000000  /* 2s x3 */
};
static const int num_retries = sizeof(retry_delays_us) / sizeof(retry_delays_us[0]);

/*
 * Read DHT11 with retries using predefined backoff schedule, within a budget.
 *
 * A retry whose delay would run past the budget is not attempted, so the
 * call returns, with an error if need be, inside budget_us from when it was
 * made. Without this a failing sensor was retried for longer than sr waits,
 * and sr killed the driver before it could report the failure: the sensor
 * simply vanished from the output, which is the one thing a failure report
 * exists to prevent.
 *
 * Elevates to SCHED_FIFO real-time priority during reads for reliable
 * GPIO timing, then restores normal scheduling afterward.
 */
int read_dht11(int gpio_pin, sensor_reading_t *reading, unsigned long budget_us) {
    uint8_t data[5];
    int attempt;
    int attempts_made = 0;
    uint64_t deadline = micros() + budget_us;
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
        attempts_made = attempt + 1;
        if (dht11_read_raw(gpio_pin, data, reading->error_msg, sizeof(reading->error_msg)) == 0) {
#ifdef DEBUG
            fprintf(stderr, "DEBUG: frame %02X %02X %02X %02X %02X on attempt %d\n",
                    data[0], data[1], data[2], data[3], data[4], attempt + 1);
#endif
            if (dht11_decode(data, &reading->humidity, &reading->temperature) == 0) {
                reading->valid = true;
                /* Restore normal scheduling */
                if (had_rt)
                    sched_setscheduler(0, SCHED_OTHER, &normal_param);
                return 0;
            }
            /* The checksum held, but no DHT11 sends these values, so the frame
               was misread. Its bytes are logged, and the read is retried. */
            ws_log_warning("DHT11 on GPIO %d sent impossible values: %02X %02X %02X %02X %02X",
                           gpio_pin, data[0], data[1], data[2], data[3], data[4]);
        }
        
        /* If we got a permission error, don't retry - it won't help */
        if (reading->error_msg[0] != '\0') {
            if (had_rt)
                sched_setscheduler(0, SCHED_OTHER, &normal_param);
            return -1;
        }
        
#ifdef DEBUG
        fprintf(stderr, "DEBUG: Attempt %d failed\n", attempt + 1);
#endif

        /* Wait before the next attempt, unless the schedule is exhausted or
           the wait would carry us past the budget. */
        if (attempt >= num_retries) break;
        if (micros() + retry_delays_us[attempt] > deadline) break;
        usleep(retry_delays_us[attempt]);
    }

    /* Only set generic error if no specific error was set */
    if (reading->error_msg[0] == '\0') {
        snprintf(reading->error_msg, sizeof(reading->error_msg),
                 "Failed to read DHT11 after %d attempts in %.1f s",
                 attempts_made, (double)budget_us / 1e6);
    }
    /* Restore normal scheduling */
    if (had_rt)
        sched_setscheduler(0, SCHED_OTHER, &normal_param);
    return -1;
}

/* json_escape_string is now provided by ws_utils.h as ws_json_escape_string */

/*
 * What tells one DHT11 from another when neither has a configured id: the
 * pin it is read from. Used by the library's fallback id assignment.
 */
static void pin_designation(const void *entry, char *buf, size_t cap) {
    snprintf(buf, cap, "pin%d", ((const sensor_config_t *)entry)->pin);
}

/*
 * The config to use when there is no file: one sensor on the default pin,
 * identified by the node serial. Heap-allocated like a parsed config, so the
 * caller frees it the same way and needs no special case.
 */
static sensor_config_t *default_config(int *count) {
    sensor_config_t *configs = calloc(1, sizeof(*configs));
    if (!configs) return NULL;

    configs[0].pin = DEFAULT_PIN;
    if (ws_config_assign_fallback_ids(configs, sizeof(*configs), 1, "dht11",
                                      pin_designation) < 0) {
        free(configs);
        return NULL;
    }
    *count = 1;
    return configs;
}

/*
 * Parse the config file, or fall back to the default when there is none.
 * Returns NULL only when out of memory.
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
        return default_config(count);
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
        idx++;
    }

    ws_config_iter_close(&it);

    if (idx == 0) {
        free(configs);
        return default_config(count);
    }

    /* Entries without a sensor_id get one from the node serial. The library
       keeps a lone entry at "<serial>_dht11", as the default config has
       always produced, and tells two or more apart by pin. */
    if (ws_config_assign_fallback_ids(configs, sizeof(*configs), idx, "dht11",
                                      pin_designation) < 0) {
        free_config(configs, idx);
        return NULL;
    }

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
 * Returns 0 on success, -1 if the reading could not be built.
 */
static int build_sensor_json(char *output, size_t output_len,
                             const char *sensor, const char *measures, const char *unit,
                             float value, bool internal, const char *sensor_id,
                             const char *sensor_name, const char *error_msg, time_t timestamp,
                             const ws_location_t *location) {
    /* The strings go in raw: the library escapes them. */
    if (ws_build_sensor_json_base(output, output_len,
                                  sensor, "dht11", measures, unit,
                                  sensor_id, sensor_name,
                                  internal, location, timestamp) != 0) {
        ws_log_error("Could not build reading for %s", sensor);
        return -1;
    }

    /* Exactly one of value or error; the library escapes the message. */
    ws_sensor_json_set_result(output, output_len, (double)value, 1, error_msg);
    return 0;
}

/*
 * Append one measurement of one sensor to the output array.
 * The sensor_id suffix is the measurement name, so temperature and humidity
 * differ only in the arguments.
 */
static void append_reading(ws_json_array_builder_t *out, const sensor_config_t *config,
                           const char *sensor, const char *measures, const char *unit,
                           float value, const char *error_msg, time_t timestamp) {
    char json[2048];
    /* An unknown id leaves "sensor_id":null, which records that it is
       unknown. Dropping the reading instead would report the node as
       having no sensors, which is worse and silent. */
    char *sensor_id = ws_measurement_id(config->base.sensor_id, measures);

    /* A reading that could not be built is not added: the array would
       refuse the empty item anyway, and fail as a whole. */
    if (build_sensor_json(json, sizeof(json), sensor, measures, unit, value,
                          config->base.internal, sensor_id, config->base.sensor_name,
                          error_msg, timestamp, &config->base.location) == 0) {
        ws_json_array_add(out, json);
    }
    free(sensor_id);
}

/*
 * Output sensor readings as a JSON array.
 * Returns WS_EXIT_SUCCESS, or WS_EXIT_INVALID_ARG with nothing printed if the
 * array could not be built: no output means "could not report", where "[]"
 * would mean "no sensors".
 */
/*
 * Does this sensor pass the location filter?
 */
static bool selected(const sensor_config_t *config, ws_location_filter_t location_filter) {
    return ws_location_filter_matches(location_filter, config->base.internal);
}

int output_json(sensor_config_t *configs, int count, const char *filter, ws_location_filter_t location_filter) {
    ws_json_array_builder_t out;
    const char *json;
    unsigned long budget_us = 0;
    int selected_count = 0;
    int i;

    if (ws_json_array_init(&out) != 0) {
        ws_log_error("Out of memory building readings");
        return WS_EXIT_INVALID_ARG;
    }

    /* The read budget is for the whole invocation, so it is shared equally
       between the sensors that will actually be read. Two failing sensors
       then take the same wall time as one, rather than twice it, and the
       driver finishes inside sr's window however many are configured. */
    for (i = 0; i < count; i++) {
        if (selected(&configs[i], location_filter)) selected_count++;
    }
    if (selected_count > 0) {
        budget_us = (DHT11_READ_BUDGET_SEC * 1000000UL) / (unsigned long)selected_count;
    }

    for (i = 0; i < count; i++) {
        sensor_reading_t reading;
        const char *error_msg = NULL;
        time_t read_timestamp;

        if (!selected(&configs[i], location_filter)) continue;

        if (read_dht11(configs[i].pin, &reading, budget_us) != 0 || !reading.valid) {
            error_msg = reading.error_msg;
        }

        /* Stamped when the value was obtained, not when the attempt began.
           A read that retried for several seconds is reported at the time
           it succeeded. Every driver stamps after its read, so the field
           means one thing whichever driver wrote it. */
        read_timestamp = time(NULL);

        if (!filter || strcmp(filter, "temperature") == 0 || strcmp(filter, "all") == 0) {
            append_reading(&out, &configs[i], "dht11_temperature",
                           "temperature", WS_UNIT_CELSIUS, reading.temperature,
                           error_msg, read_timestamp);
        }

        if (!filter || strcmp(filter, "humidity") == 0 || strcmp(filter, "all") == 0) {
            append_reading(&out, &configs[i], "dht11_humidity",
                           "humidity", WS_UNIT_PERCENTAGE, reading.humidity,
                           error_msg, read_timestamp);
        }
    }

    ws_json_array_end(&out);
    json = ws_json_array_get(&out);
    if (!json) {
        ws_log_error("Out of memory building readings");
        ws_json_array_free(&out);
        return WS_EXIT_INVALID_ARG;
    }

    printf("%s\n", json);
    ws_json_array_free(&out);
    return WS_EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
    /* What this driver measures: the one source for the list command,
       the measurement filters it accepts, and its usage line. */
    static const char *measurements[] = {"temperature", "humidity", NULL};
    sensor_config_t *configs = NULL;
    int config_count = 0;
    const char *filter = NULL;
    ws_location_filter_t location_filter = WS_LOCATION_ALL;
    int status;

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
            ws_cmd_list_multiple(measurements);
        } else if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0 ||
                   strcmp(argv[1], "version") == 0) {
            ws_print_version("sensor-dht11", VERSION_STRING);
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "enable") == 0) {
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "setup") == 0) {
            /* Bit-banged through the GPIO character device, so no
               device-tree overlay and nothing to set up. */
            printf("DHT11 sensor requires no additional setup.\n");
            return WS_EXIT_SUCCESS;
        } else if (strcmp(argv[1], "mock") == 0) {
            /* Fixed readings in the real output format, for testing without
               hardware. The values are ours; the formatting is the library's,
               so mock cannot drift from what a real read produces. */
            static const ws_mock_reading_t mock[] = {
                /* Declared at the node, as sensor-onboard's mock declares its
                   physical sensors, so every driver's mock has one shape. */
                { "dht11_temperature", "temperature", NULL, WS_UNIT_CELSIUS,    22.0, 1, "{{node}}" },
                { "dht11_humidity",    "humidity",    NULL, WS_UNIT_PERCENTAGE, 55.0, 1, "{{node}}" },
            };
            return ws_cmd_mock("dht11", "dht11_mock", "Mock DHT11",
                               mock, sizeof(mock) / sizeof(mock[0]));
        } else if (ws_arg_is_measurement(argv[1], measurements)) {
            filter = argv[1];
        } else if (strcmp(argv[1], "internal") == 0) {
            location_filter = WS_LOCATION_INTERNAL;
        } else if (strcmp(argv[1], "external") == 0) {
            location_filter = WS_LOCATION_EXTERNAL;
        } else if (strcmp(argv[1], "all") != 0) {
            return ws_cmd_unknown_arg("sensor-dht11", argv[1], measurements);
        }
    }

    /* Every reading needs the template, so ask once before touching the
       sensor. Without it, fail with nothing printed: "[]" would claim the
       node has no sensors, and a partial array is not JSON at all. */
    status = ws_require_prototype();
    if (status != 0) {
        cancel_watchdog();
        closelog();
        return status;
    }

    configs = load_config(CONFIG_PATH, &config_count);
    if (!configs) {
        ws_log_error("Out of memory loading configuration");
        cancel_watchdog();
        closelog();
        return WS_EXIT_INVALID_ARG;
    }

    status = output_json(configs, config_count, filter, location_filter);

    free_config(configs, config_count);

    /* Cancel watchdog before normal exit */
    cancel_watchdog();

    closelog();
    return status;
}
