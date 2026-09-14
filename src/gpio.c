/*
 * gpio.c - One GPIO line through the Linux GPIO character device
 *
 * Part of sensor-dht11; see gpio.h.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/gpio.h>

#include "gpio.h"

/* The consumer label gpioinfo shows against the line while a read holds it */
#define DHT_GPIO_CONSUMER  "dht11"

static const char access_denied[] = "GPIO access denied - try running with sudo";

/* Why the chip could not be opened, in words a reading's error can carry */
static void describe_open_failure(int err, char *error_msg, size_t error_len) {
    if (!error_msg || error_len == 0) {
        return;
    }
    if (err == EACCES || err == EPERM) {
        snprintf(error_msg, error_len, "%s", access_denied);
    } else if (err == ENOENT || err == ENODEV) {
        snprintf(error_msg, error_len, "No GPIO chip at %s", DHT_GPIO_CHIP_PATH);
    } else {
        snprintf(error_msg, error_len, "Cannot open %s: %s",
                 DHT_GPIO_CHIP_PATH, strerror(err));
    }
}

/* Why the line could not be requested */
static void describe_request_failure(int err, int pin, char *error_msg, size_t error_len) {
    if (!error_msg || error_len == 0) {
        return;
    }
    if (err == EACCES || err == EPERM) {
        snprintf(error_msg, error_len, "%s", access_denied);
    } else if (err == EBUSY) {
        /* w1-gpio, or another process, already holds the line */
        snprintf(error_msg, error_len, "GPIO %d is in use by another driver", pin);
    } else if (err == EINVAL) {
        snprintf(error_msg, error_len, "GPIO %d does not exist on %s",
                 pin, DHT_GPIO_CHIP_PATH);
    } else {
        snprintf(error_msg, error_len, "Cannot request GPIO %d: %s",
                 pin, strerror(err));
    }
}

int dht_gpio_open(int pin, char *error_msg, size_t error_len) {
    struct gpio_v2_line_request req;
    int chip_fd;
    int err;

    chip_fd = open(DHT_GPIO_CHIP_PATH, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) {
        err = errno;
        describe_open_failure(err, error_msg, error_len);
        errno = err;
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.offsets[0] = (__u32)pin;
    req.num_lines = 1;
    memcpy(req.consumer, DHT_GPIO_CONSUMER, sizeof(DHT_GPIO_CONSUMER));
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    /* Start high, as the line idles, so requesting it sends no pulse */
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 1;
    req.config.attrs[0].mask = 1;

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        err = errno;
        close(chip_fd);
        describe_request_failure(err, pin, error_msg, error_len);
        errno = err;
        return -1;
    }

    /* The line has its own descriptor, so the chip's is no longer needed */
    close(chip_fd);
    return req.fd;
}

int dht_gpio_set(int fd, int value) {
    struct gpio_v2_line_values values;

    values.bits = value ? 1 : 0;
    values.mask = 1;
    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0 ? -1 : 0;
}

int dht_gpio_input(int fd) {
    struct gpio_v2_line_config config;

    /* No bias flag is set, as libgpiod 1 set none for this driver */
    memset(&config, 0, sizeof(config));
    config.flags = GPIO_V2_LINE_FLAG_INPUT;
    return ioctl(fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &config) < 0 ? -1 : 0;
}

int dht_gpio_get(int fd) {
    struct gpio_v2_line_values values;

    values.bits = 0;
    values.mask = 1;
    if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
        return -1;
    }
    return (values.bits & 1) ? 1 : 0;
}

void dht_gpio_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}
