/* ==========================================
    Unity - A Test Framework for C
    ThrowTheSwitch.org
    Copyright (c) 2007-2024 Mike Karlesky, Mark VanderVoord, & Greg Williams
    SPDX-License-Identifier: MIT
   ========================================== */

#include "unity.h"

Unity_t Unity;

void UnityBegin(const char* filename) {
    Unity.TestFile = filename;
    Unity.NumberOfTests = 0;
    Unity.TestFailures = 0;
    Unity.TestIgnores = 0;
    printf("\n----- Running tests from %s -----\n\n", filename);
}

int UnityEnd(void) {
    printf("\n-----------------------\n");
    printf("%d Tests, %d Failures, %d Ignored\n", 
           Unity.NumberOfTests, Unity.TestFailures, Unity.TestIgnores);
    
    if (Unity.TestFailures == 0) {
        printf("OK\n");
    } else {
        printf("FAIL\n");
    }
    
    return Unity.TestFailures;
}

void UnityRunTest(UnityTestFunction func, const char* name, int line) {
    Unity.CurrentTestName = name;
    Unity.CurrentTestLineNumber = line;
    Unity.CurrentTestFailed = 0;
    Unity.CurrentTestIgnored = 0;
    Unity.NumberOfTests++;
    
    setUp();
    func();
    tearDown();
    
    if (Unity.CurrentTestFailed) {
        printf("FAIL: %s\n", name);
    } else if (Unity.CurrentTestIgnored) {
        printf("IGNORE: %s\n", name);
    } else {
        printf("PASS: %s\n", name);
    }
}

void UnityFail(const char* file, int line, const char* message) {
    Unity.CurrentTestFailed = 1;
    Unity.TestFailures++;
    printf("  %s:%d: %s\n", file, line, message);
}

void UnityFailInt(const char* file, int line, int expected, int actual) {
    Unity.CurrentTestFailed = 1;
    Unity.TestFailures++;
    printf("  %s:%d: Expected %d, got %d\n", file, line, expected, actual);
}

void UnityFailFloat(const char* file, int line, float expected, float actual) {
    Unity.CurrentTestFailed = 1;
    Unity.TestFailures++;
    printf("  %s:%d: Expected %f, got %f\n", file, line, expected, actual);
}

void UnityFailString(const char* file, int line, const char* expected, const char* actual) {
    Unity.CurrentTestFailed = 1;
    Unity.TestFailures++;
    printf("  %s:%d: Expected \"%s\", got \"%s\"\n", file, line, expected, actual);
}

void UnityIgnore(const char* file, int line, const char* message) {
    (void)file;
    (void)line;
    Unity.CurrentTestIgnored = 1;
    Unity.TestIgnores++;
    printf("  %s\n", message);
}

/* Default empty setUp/tearDown - can be overridden */
__attribute__((weak)) void setUp(void) {}
__attribute__((weak)) void tearDown(void) {}
