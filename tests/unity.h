/* ==========================================
    Unity - A Test Framework for C
    ThrowTheSwitch.org
    Copyright (c) 2007-2024 Mike Karlesky, Mark VanderVoord, & Greg Williams
    SPDX-License-Identifier: MIT
   ========================================== */

#ifndef UNITY_H
#define UNITY_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Unity Internals */
typedef void (*UnityTestFunction)(void);

typedef struct {
    const char* TestFile;
    int CurrentTestLineNumber;
    int NumberOfTests;
    int TestFailures;
    int TestIgnores;
    const char* CurrentTestName;
    int CurrentTestFailed;
    int CurrentTestIgnored;
} Unity_t;

extern Unity_t Unity;

/* Core Test Macros */
#define TEST_ASSERT(condition) \
    do { if (!(condition)) { UnityFail(__FILE__, __LINE__, #condition); } } while(0)

#define TEST_ASSERT_TRUE(condition) TEST_ASSERT(condition)
#define TEST_ASSERT_FALSE(condition) TEST_ASSERT(!(condition))

#define TEST_ASSERT_NULL(pointer) \
    do { if ((pointer) != NULL) { UnityFail(__FILE__, __LINE__, "Expected NULL"); } } while(0)

#define TEST_ASSERT_NOT_NULL(pointer) \
    do { if ((pointer) == NULL) { UnityFail(__FILE__, __LINE__, "Expected NOT NULL"); } } while(0)

#define TEST_ASSERT_EQUAL_INT(expected, actual) \
    do { if ((expected) != (actual)) { UnityFailInt(__FILE__, __LINE__, (expected), (actual)); } } while(0)

#define TEST_ASSERT_EQUAL_FLOAT(expected, actual, epsilon) \
    do { float _diff = (expected) - (actual); if (_diff < 0) _diff = -_diff; \
         if (_diff > (epsilon)) { UnityFailFloat(__FILE__, __LINE__, (expected), (actual)); } } while(0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) \
    do { if (strcmp((expected), (actual)) != 0) { UnityFailString(__FILE__, __LINE__, (expected), (actual)); } } while(0)

#define TEST_ASSERT_EQUAL_MEMORY(expected, actual, len) \
    do { if (memcmp((expected), (actual), (len)) != 0) { UnityFail(__FILE__, __LINE__, "Memory mismatch"); } } while(0)

#define TEST_ASSERT_GREATER_THAN(threshold, actual) \
    do { if (!((actual) > (threshold))) { UnityFail(__FILE__, __LINE__, "Expected greater than"); } } while(0)

#define TEST_ASSERT_LESS_THAN(threshold, actual) \
    do { if (!((actual) < (threshold))) { UnityFail(__FILE__, __LINE__, "Expected less than"); } } while(0)

#define TEST_FAIL_MESSAGE(message) UnityFail(__FILE__, __LINE__, message)
#define TEST_IGNORE_MESSAGE(message) UnityIgnore(__FILE__, __LINE__, message)
#define TEST_IGNORE() UnityIgnore(__FILE__, __LINE__, "Test ignored")

/* Test Runner Macros */
#define RUN_TEST(func) UnityRunTest(func, #func, __LINE__)

#define UNITY_BEGIN() UnityBegin(__FILE__)
#define UNITY_END() UnityEnd()

/* Function Prototypes */
void UnityBegin(const char* filename);
int UnityEnd(void);
void UnityRunTest(UnityTestFunction func, const char* name, int line);
void UnityFail(const char* file, int line, const char* message);
void UnityFailInt(const char* file, int line, int expected, int actual);
void UnityFailFloat(const char* file, int line, float expected, float actual);
void UnityFailString(const char* file, int line, const char* expected, const char* actual);
void UnityIgnore(const char* file, int line, const char* message);

/* Optional setUp/tearDown */
void setUp(void);
void tearDown(void);

#endif /* UNITY_H */
