# Makefile for sensor-dht11 unit tests
# Uses Unity test framework

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -I../src -I../include -DTEST_BUILD

# Source files
UNITY_SRC = unity.c
TEST_SRC = test_dht11.c

# We need to compile a test-friendly version of dht11.c
# that doesn't include GPIO-dependent code
SRC_DIR = ../src

# Object files
OBJS = unity.o test_dht11.o dht11_testable.o

# Test binary
TEST_BIN = run_tests

.PHONY: all clean test

all: $(TEST_BIN)

$(TEST_BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

unity.o: unity.c unity.h
	$(CC) $(CFLAGS) -c unity.c

test_dht11.o: test_dht11.c unity.h $(SRC_DIR)/dht11.h
	$(CC) $(CFLAGS) -c test_dht11.c

# Build testable version of dht11.c (without GPIO hardware calls)
dht11_testable.o: dht11_testable.c $(SRC_DIR)/dht11.h
	$(CC) $(CFLAGS) -c dht11_testable.c

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(OBJS) $(TEST_BIN)
