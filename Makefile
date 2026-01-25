# Makefile for sensor-dht11 (C version)
# Build DHT11 sensor reader for Raspberry Pi using libgpiod

# Extract version from debian/changelog
VERSION := $(shell head -n1 debian/changelog | sed 's/.*(//' | sed 's/).*//')

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -I/usr/include/ws -DVERSION=\"$(VERSION)\"
LDFLAGS = -lgpiod -lwildlifesystems

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1

SRCDIR = src
TARGET = sensor-dht11
SOURCES = $(SRCDIR)/dht11.c
HEADERS = $(SRCDIR)/dht11.h

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
