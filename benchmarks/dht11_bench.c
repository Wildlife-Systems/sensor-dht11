/*
 * dht11_bench - Pure sensor read benchmark (no JSON, no sc-prototype)
 * Equivalent to Python Adafruit benchmark for fair comparison
 * Usage: sensor-dht11-bench [count]
 * Default count is 500
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <gpiod.h>

/* Constants */
#define DEFAULT_PIN     4
#define GPIO_CHIP_PATH  "/dev/gpiochip0"

/* DHT11 timing constants (microseconds) */
#define DHT11_START_LOW_US      20000   /* Start signal: pull low for 20ms */
#define DHT11_START_HIGH_US     20      /* Then release for 20-40us */
#define DHT11_TIMEOUT_US        1000    /* Timeout waiting for edges */

/* Global state for signal handler cleanup */
static volatile sig_atomic_t g_running = 1;
static struct gpiod_chip *g_chip = NULL;
static struct gpiod_line *g_line = NULL;

/*
 * Get current time in microseconds
 */
static uint64_t micros(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/*
 * Get current time in seconds (double precision)
 */
static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
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
    
    fprintf(stderr, "\nInterrupted, exiting\n");
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
 */
static int dht11_read_raw(int gpio_pin, uint8_t data[5]) {
    struct gpiod_chip *chip;
    struct gpiod_line *line;
    int i, j;
    
    if (!g_running) {
        return -1;
    }
    
    chip = gpiod_chip_open(GPIO_CHIP_PATH);
    if (!chip) {
        return -1;
    }
    g_chip = chip;
    
    line = gpiod_chip_get_line(chip, gpio_pin);
    if (!line) {
        gpiod_chip_close(chip);
        g_chip = NULL;
        return -1;
    }
    g_line = line;
    
    /* Send start signal */
    if (gpiod_line_request_output(line, "dht11", 1) < 0) {
        gpiod_chip_close(chip);
        g_chip = NULL;
        return -1;
    }
    
    gpiod_line_set_value(line, 0);
    usleep(DHT11_START_LOW_US);
    gpiod_line_set_value(line, 1);
    usleep(DHT11_START_HIGH_US);
    
    gpiod_line_release(line);
    if (gpiod_line_request_input(line, "dht11") < 0) {
        gpiod_chip_close(chip);
        g_line = NULL;
        g_chip = NULL;
        return -1;
    }
    
    /* Wait for DHT11 response */
    if (wait_for_level(line, 0, DHT11_TIMEOUT_US) < 0) {
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        g_line = NULL;
        g_chip = NULL;
        return -1;
    }
    
    if (wait_for_level(line, 1, DHT11_TIMEOUT_US) < 0) {
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        g_line = NULL;
        g_chip = NULL;
        return -1;
    }
    
    if (wait_for_level(line, 0, DHT11_TIMEOUT_US) < 0) {
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        g_line = NULL;
        g_chip = NULL;
        return -1;
    }
    
    /* Read 40 bits */
    memset(data, 0, 5);
    int pulse_times[50];
    int num_pulses = 0;
    
    for (i = 0; i < 50; i++) {
        int high_result = wait_for_level(line, 1, DHT11_TIMEOUT_US);
        if (high_result < 0) break;
        
        uint64_t start = micros();
        wait_for_level(line, 0, DHT11_TIMEOUT_US);
        int duration = (int)(micros() - start);
        
        pulse_times[num_pulses++] = duration;
        if (duration > 500) break;
    }
    
    int valid_pulses = 0;
    for (i = 0; i < num_pulses; i++) {
        if (pulse_times[i] < 500) valid_pulses++;
    }
    
    if (valid_pulses < 38) {
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        g_line = NULL;
        g_chip = NULL;
        return -1;
    }
    
    int min_pulse = 10000, max_pulse = 0;
    for (i = 0; i < num_pulses; i++) {
        if (pulse_times[i] < 500) {
            if (pulse_times[i] < min_pulse) min_pulse = pulse_times[i];
            if (pulse_times[i] > max_pulse) max_pulse = pulse_times[i];
        }
    }
    int threshold = (min_pulse + max_pulse) / 2;
    
    int valid_times[50];
    int v = 0;
    for (i = 0; i < num_pulses && v < 50; i++) {
        if (pulse_times[i] < 500) {
            valid_times[v++] = pulse_times[i];
        }
    }
    
    int bits_missing = 40 - valid_pulses;
    int bit_idx = 0;
    
    for (i = 0; i < bits_missing; i++) {
        j = bit_idx / 8;
        data[j] <<= 1;
        bit_idx++;
    }
    
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
    
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        return -1;
    }
    
    return 0;
}

/* Retry delays in microseconds: 0.1s x3, then 0.2, 0.4, 0.8, 1.6, 2s x3 */
static const useconds_t retry_delays_us[] = {
    100000, 100000, 100000,
    200000, 400000, 800000, 1600000,
    2000000, 2000000, 2000000
};
static const int num_retries = sizeof(retry_delays_us) / sizeof(retry_delays_us[0]);

/*
 * Read DHT11 with retries
 * Returns number of attempts on success, -1 on failure
 */
static int read_dht11_with_attempts(int gpio_pin, float *temperature, float *humidity) {
    uint8_t data[5];
    int attempt;
    
    for (attempt = 0; attempt <= num_retries; attempt++) {
        if (dht11_read_raw(gpio_pin, data) == 0) {
            *humidity = (float)data[0] + (float)data[1] / 10.0f;
            *temperature = (float)data[2] + (float)data[3] / 10.0f;
            return attempt + 1;  /* Return number of attempts (1-based) */
        }
        
        if (attempt < num_retries) {
            usleep(retry_delays_us[attempt]);
        }
    }
    
    return -1;  /* Failed after all retries */
}

int main(int argc, char *argv[]) {
    int count = 500;  /* Default to 500 readings */
    int gpio_pin = DEFAULT_PIN;
    int i;
    int successes = 0, failures = 0;
    int total_attempts = 0;
    double *times = NULL;
    double total_time = 0;
    FILE *outfile;
    
    setup_signal_handlers();
    
    if (argc >= 2) {
        count = atoi(argv[1]);
        if (count <= 0) {
            fprintf(stderr, "Usage: %s [count]\n", argv[0]);
            return 1;
        }
    }
    
    times = calloc(count, sizeof(double));
    if (!times) {
        fprintf(stderr, "Failed to allocate memory for %d readings\n", count);
        return 1;
    }
    
    outfile = fopen("results_c.csv", "w");
    if (!outfile) {
        fprintf(stderr, "Cannot open results_c.csv for writing\n");
        return 1;
    }
    fprintf(outfile, "read,time,attempts\n");
    
    fprintf(stderr, "Running %d DHT11 reads on GPIO %d...\n", count, gpio_pin);
    
    for (i = 0; i < count && g_running; i++) {
        float temperature, humidity;
        double start = get_time_sec();
        
        int attempts = read_dht11_with_attempts(gpio_pin, &temperature, &humidity);
        
        double elapsed = get_time_sec() - start;
        times[i] = elapsed;
        total_time += elapsed;
        
        /* Write to CSV file */
        fprintf(outfile, "%d,%.6f,%d\n", i + 1, elapsed, attempts);
        
        if (attempts > 0) {
            successes++;
            total_attempts += attempts;
        } else {
            failures++;
        }
        
        /* Progress indicator every 50 reads */
        if ((i + 1) % 50 == 0) {
            fprintf(stderr, "  Progress: %d/%d\n", i + 1, count);
        }
        
        /* Short delay between readings (same as Python benchmark) */
        if (i < count - 1) {
            usleep(100000);  /* 100ms between readings */
        }
    }
    
    fclose(outfile);
    
    /* Calculate min/max/avg times */
    double min_time = times[0], max_time = times[0];
    for (i = 1; i < count; i++) {
        if (times[i] < min_time) min_time = times[i];
        if (times[i] > max_time) max_time = times[i];
    }
    
    /* Print summary to stdout */
    printf("\n=== C Statistics ===\n");
    printf("Readings:     %d success, %d failed (%.1f%% success rate)\n",
           successes, failures, 100.0 * successes / count);
    
    if (successes > 0) {
        printf("Avg attempts: %.2f per successful read\n",
               (double)total_attempts / successes);
    }
    
    printf("Timing:       min=%.4fs, max=%.4fs, avg=%.4fs, total=%.4fs\n",
           min_time, max_time, total_time / count, total_time);
    printf("Results saved to results_c.csv\n");
    
    free(times);
    return failures > 0 ? 1 : 0;
}
