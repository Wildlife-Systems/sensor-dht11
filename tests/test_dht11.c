/*
 * Unit tests for sensor-dht11
 * Tests JSON parsing, config loading, and output formatting
 * without requiring actual hardware.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "unity.h"

/* Include the header to get types and constants */
#include "../src/dht11.h"

/* ========== Test Fixtures ========== */

static char temp_config_path[256];
static int temp_file_counter = 0;

void setUp(void) {
    /* Create unique temp file path for each test */
    snprintf(temp_config_path, sizeof(temp_config_path), 
             "/tmp/dht11_test_%d_%d.json", getpid(), temp_file_counter++);
}

void tearDown(void) {
    /* Clean up temp file */
    unlink(temp_config_path);
}

/* Helper to write config file */
static void write_config(const char *json) {
    FILE *fp = fopen(temp_config_path, "w");
    if (fp) {
        fputs(json, fp);
        fclose(fp);
    }
}

/* ========== Config Loading Tests ========== */

void test_load_config_single_sensor(void) {
    int count = 0;
    
    write_config("[{\"pin\": 4, \"internal\": false, \"sensor_id\": \"test_sensor\"}]");
    
    sensor_config_t *configs = load_config(temp_config_path, &count);
    
    TEST_ASSERT_NOT_NULL(configs);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_INT(4, configs[0].pin);
    TEST_ASSERT_FALSE(configs[0].internal);
    
    free_config(configs, count);
}

void test_load_config_internal_true(void) {
    int count = 0;
    
    write_config("[{\"pin\": 17, \"internal\": true, \"sensor_id\": \"test_sensor\"}]");
    
    sensor_config_t *configs = load_config(temp_config_path, &count);
    
    TEST_ASSERT_NOT_NULL(configs);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_INT(17, configs[0].pin);
    TEST_ASSERT_TRUE(configs[0].internal);
    
    free_config(configs, count);
}

void test_load_config_custom_sensor_id(void) {
    int count = 0;
    
    write_config("[{\"pin\": 4, \"internal\": false, \"sensor_id\": \"my_custom_id\"}]");
    
    sensor_config_t *configs = load_config(temp_config_path, &count);
    
    TEST_ASSERT_NOT_NULL(configs);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_STRING("my_custom_id", configs[0].sensor_id);
    
    free_config(configs, count);
}

void test_load_config_custom_sensor_name(void) {
    int count = 0;
    
    write_config("[{\"pin\": 4, \"internal\": false, \"sensor_id\": \"test\", \"sensor_name\": \"enclosure_dht11\"}]");
    
    sensor_config_t *configs = load_config(temp_config_path, &count);
    
    TEST_ASSERT_NOT_NULL(configs);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_STRING("enclosure_dht11", configs[0].sensor_name);
    
    free_config(configs, count);
}

void test_load_config_default_sensor_name(void) {
    int count = 0;
    
    write_config("[{\"pin\": 4, \"internal\": false, \"sensor_id\": \"test\"}]");
    
    sensor_config_t *configs = load_config(temp_config_path, &count);
    
    TEST_ASSERT_NOT_NULL(configs);
    TEST_ASSERT_EQUAL_INT(1, count);
    /* sensor_name is NULL when not specified - sc-prototype provides default */
    TEST_ASSERT_NULL(configs[0].sensor_name);
    
    free_config(configs, count);
}

void test_load_config_multiple_sensors(void) {
    int count = 0;
    
    write_config("[\n"
                 "  {\"pin\": 4, \"internal\": true, \"sensor_id\": \"sensor1\"},\n"
                 "  {\"pin\": 17, \"internal\": false, \"sensor_id\": \"sensor2\"}\n"
                 "]");
    
    sensor_config_t *configs = load_config(temp_config_path, &count);
    
    TEST_ASSERT_NOT_NULL(configs);
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_INT(4, configs[0].pin);
    TEST_ASSERT_TRUE(configs[0].internal);
    TEST_ASSERT_EQUAL_INT(17, configs[1].pin);
    TEST_ASSERT_FALSE(configs[1].internal);
    
    free_config(configs, count);
}

void test_load_config_invalid_pin_uses_default(void) {
    int count = 0;
    
    /* Pin 50 is invalid, should use default pin 4 */
    write_config("[{\"pin\": 50, \"internal\": false, \"sensor_id\": \"test\"}]");
    
    sensor_config_t *configs = load_config(temp_config_path, &count);
    
    TEST_ASSERT_NOT_NULL(configs);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_INT(DEFAULT_PIN, configs[0].pin);
    
    free_config(configs, count);
}

void test_load_config_missing_file(void) {
    int count = 0;
    
    sensor_config_t *configs = load_config("/nonexistent/path/config.json", &count);
    
    TEST_ASSERT_NULL(configs);
    TEST_ASSERT_EQUAL_INT(0, count);
}

void test_load_config_empty_array(void) {
    int count = 0;
    
    write_config("[]");
    
    sensor_config_t *configs = load_config(temp_config_path, &count);
    
    TEST_ASSERT_NULL(configs);
    TEST_ASSERT_EQUAL_INT(0, count);
}

void test_load_config_defaults_when_fields_missing(void) {
    int count = 0;
    
    /* Only internal specified, pin should default */
    write_config("[{\"internal\": true}]");
    
    sensor_config_t *configs = load_config(temp_config_path, &count);
    
    TEST_ASSERT_NOT_NULL(configs);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_INT(DEFAULT_PIN, configs[0].pin);
    TEST_ASSERT_TRUE(configs[0].internal);
    
    free_config(configs, count);
}

/* ========== Version Tests ========== */

void test_version_string_format(void) {
    /* Version string should be in semver format */
    TEST_ASSERT_NOT_NULL(VERSION_STRING);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(VERSION_STRING));
    
    /* Check it contains dots (semver format) */
    TEST_ASSERT_NOT_NULL(strchr(VERSION_STRING, '.'));
}

/* ========== Constants Tests ========== */

void test_watchdog_timeout(void) {
    TEST_ASSERT_GREATER_THAN(0, WATCHDOG_TIMEOUT_SEC);
    TEST_ASSERT_LESS_THAN(120, WATCHDOG_TIMEOUT_SEC); /* Should be reasonable */
}

/* ========== Main Test Runner ========== */

int main(void) {
    UNITY_BEGIN();
    
    /* Config Loading */
    RUN_TEST(test_load_config_single_sensor);
    RUN_TEST(test_load_config_internal_true);
    RUN_TEST(test_load_config_custom_sensor_id);
    RUN_TEST(test_load_config_custom_sensor_name);
    RUN_TEST(test_load_config_default_sensor_name);
    RUN_TEST(test_load_config_multiple_sensors);
    RUN_TEST(test_load_config_invalid_pin_uses_default);
    RUN_TEST(test_load_config_missing_file);
    RUN_TEST(test_load_config_empty_array);
    RUN_TEST(test_load_config_defaults_when_fields_missing);
    
    /* Version */
    RUN_TEST(test_version_string_format);
    
    /* Constants */
    RUN_TEST(test_watchdog_timeout);
    
    return UNITY_END();
}
