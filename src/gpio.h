/*
 * gpio.h - One GPIO line through the Linux GPIO character device
 *
 * Part of sensor-dht11. The DHT11 protocol is bit-banged, so the driver needs
 * only to hold a line, drive it, switch it to input and sample it. Version 2
 * of the GPIO character device interface, in Linux 5.10 and later, provides
 * each of those as a single ioctl, so no GPIO library sits between the driver
 * and the kernel.
 */

#ifndef DHT11_GPIO_H
#define DHT11_GPIO_H

#include <stddef.h>

/* GPIO chip for the Raspberry Pi header */
#define DHT_GPIO_CHIP_PATH  "/dev/gpiochip0"

/*
 * Request a line on DHT_GPIO_CHIP_PATH as an output driven high.
 * Returns the line's file descriptor, or -1 with errno set and a short
 * statement of the cause written to error_msg.
 */
int dht_gpio_open(int pin, char *error_msg, size_t error_len);

/* Drive the line low (value 0) or high. Returns 0, or -1 with errno set. */
int dht_gpio_set(int fd, int value);

/* Switch the line to input, releasing it to its pull-up. Returns 0, or -1. */
int dht_gpio_input(int fd);

/* Sample the line. Returns 0 or 1, or -1 with errno set. */
int dht_gpio_get(int fd);

/* Release the line. Makes no call that is unsafe in a signal handler. */
void dht_gpio_close(int fd);

#endif /* DHT11_GPIO_H */
