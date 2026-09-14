# Makefile for sensor-dht11 (C version)
# Build the DHT11 sensor reader for Raspberry Pi. It reaches the GPIO pin
# through the Linux GPIO character device, so no GPIO library is linked.

# Extract version from debian/changelog
VERSION := $(shell head -n1 debian/changelog | sed 's/.*(//' | sed 's/).*//')

CC = gcc
# EXTRA_CFLAGS / EXTRA_LDFLAGS come first so a caller's -I and -L win over the
# installed library: the root Makefile uses them to build against the
# libwildlifesystems it has just built rather than whatever is in /usr.
CFLAGS = $(EXTRA_CFLAGS) -Wall -Wextra -O2 -std=c99 -I/usr/include/ws -DVERSION=\"$(VERSION)\"
LDFLAGS = $(EXTRA_LDFLAGS) -lwildlifesystems

# /usr, not /usr/local: sr looks for drivers in /usr/bin only, so a driver
# installed by hand anywhere else is never found.
PREFIX ?= /usr
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1

SRCDIR = src
TARGET = sensor-dht11
SOURCES = $(SRCDIR)/dht11.c $(SRCDIR)/gpio.c
HEADERS = $(SRCDIR)/dht11.h $(SRCDIR)/gpio.h

.PHONY: all clean install uninstall debug deb

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

# Build with debug symbols
debug: CFLAGS += -g -DDEBUG
debug: $(TARGET)

# Install the binary
install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 man/sensor-dht11.1 $(DESTDIR)$(MANDIR)/

# Uninstall
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(MANDIR)/sensor-dht11.1

# Clean build artifacts
clean:
	rm -f $(TARGET)

# For Debian packaging
deb:
	dpkg-buildpackage -us -uc -b
