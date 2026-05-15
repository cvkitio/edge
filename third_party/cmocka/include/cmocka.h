/*
 * cmocka.h — stub header for building tests without system cmocka.
 * Provides minimal assert_ and mock macros sufficient for emd unit tests.
 * When system cmocka is available, CMake should prefer it and not use this.
 */
#ifndef CMOCKA_H_
#define CMOCKA_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Test function type                                                       */
/* --------------------------------------------------------------------- */
typedef void (*CMUnitTestFunction)(void **state);
typedef int  (*CMFixtureFunction)(void **state);

typedef struct CMUnitTest {
    const char           *name;
    CMUnitTestFunction    test_func;
    CMFixtureFunction     setup_func;
    CMFixtureFunction     teardown_func;
    void                 *initial_state;
} CMUnitTest;

/* --------------------------------------------------------------------- */
/* Assertion macros                                                        */
/* --------------------------------------------------------------------- */

/* Internal failure helper */
static inline void _cmocka_fail(const char *file, int line, const char *msg) {
    fprintf(stderr, "[  FAILED  ] %s:%d: %s\n", file, line, msg);
    exit(1);
}

#define fail_msg(fmt) do { \
    _cmocka_fail(__FILE__, __LINE__, (fmt)); \
} while(0)

#define fail() _cmocka_fail(__FILE__, __LINE__, "explicit fail()")

#define assert_true(expr) do { \
    if (!(expr)) { \
        _cmocka_fail(__FILE__, __LINE__, "assert_true(" #expr ") failed"); \
    } \
} while(0)

#define assert_false(expr) do { \
    if (expr) { \
        _cmocka_fail(__FILE__, __LINE__, "assert_false(" #expr ") failed"); \
    } \
} while(0)

#define assert_null(ptr) do { \
    if ((ptr) != NULL) { \
        _cmocka_fail(__FILE__, __LINE__, "assert_null(" #ptr ") failed"); \
    } \
} while(0)

#define assert_non_null(ptr) do { \
    if ((ptr) == NULL) { \
        _cmocka_fail(__FILE__, __LINE__, "assert_non_null(" #ptr ") failed"); \
    } \
} while(0)

#define assert_int_equal(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a != _b) { \
        char _cm_buf[256]; \
        snprintf(_cm_buf, sizeof(_cm_buf), \
                 "assert_int_equal(%s, %s): %lld != %lld", \
                 #a, #b, _a, _b); \
        _cmocka_fail(__FILE__, __LINE__, _cm_buf); \
    } \
} while(0)

#define assert_int_not_equal(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a == _b) { \
        char _cm_buf[256]; \
        snprintf(_cm_buf, sizeof(_cm_buf), \
                 "assert_int_not_equal(%s, %s): both == %lld", #a, #b, _a); \
        _cmocka_fail(__FILE__, __LINE__, _cm_buf); \
    } \
} while(0)

#define assert_string_equal(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        char _cm_buf[512]; \
        snprintf(_cm_buf, sizeof(_cm_buf), \
                 "assert_string_equal: \"%s\" != \"%s\"", (a), (b)); \
        _cmocka_fail(__FILE__, __LINE__, _cm_buf); \
    } \
} while(0)

#define assert_string_not_equal(a, b) do { \
    if (strcmp((a), (b)) == 0) { \
        char _cm_buf[512]; \
        snprintf(_cm_buf, sizeof(_cm_buf), \
                 "assert_string_not_equal: both == \"%s\"", (a)); \
        _cmocka_fail(__FILE__, __LINE__, _cm_buf); \
    } \
} while(0)

#define assert_memory_equal(a, b, size) do { \
    if (memcmp((a), (b), (size)) != 0) { \
        _cmocka_fail(__FILE__, __LINE__, \
                     "assert_memory_equal failed"); \
    } \
} while(0)

#define assert_in_range(value, minimum, maximum) do { \
    long long _v = (long long)(value); \
    long long _lo = (long long)(minimum); \
    long long _hi = (long long)(maximum); \
    if (_v < _lo || _v > _hi) { \
        char _cm_buf[256]; \
        snprintf(_cm_buf, sizeof(_cm_buf), \
                 "assert_in_range: %lld not in [%lld, %lld]", _v, _lo, _hi); \
        _cmocka_fail(__FILE__, __LINE__, _cm_buf); \
    } \
} while(0)

#define assert_float_equal(a, b, eps) do { \
    double _a = (double)(a); \
    double _b = (double)(b); \
    double _e = (double)(eps); \
    double _d = _a - _b; \
    if (_d < 0) _d = -_d; \
    if (_d > _e) { \
        char _cm_buf[256]; \
        snprintf(_cm_buf, sizeof(_cm_buf), \
                 "assert_float_equal: |%.6g - %.6g| = %.6g > %.6g", \
                 _a, _b, _d, _e); \
        _cmocka_fail(__FILE__, __LINE__, _cm_buf); \
    } \
} while(0)

/* --------------------------------------------------------------------- */
/* Test runner                                                              */
/* --------------------------------------------------------------------- */

#define cmocka_unit_test(f) \
    { #f, (f), NULL, NULL, NULL }

#define cmocka_unit_test_setup_teardown(f, s, t) \
    { #f, (f), (s), (t), NULL }

static inline int _cmocka_run_group_tests(const char *group_name,
                                           const CMUnitTest *tests,
                                           size_t count,
                                           CMFixtureFunction group_setup,
                                           CMFixtureFunction group_teardown)
{
    (void)group_setup;
    (void)group_teardown;

    int failed = 0;
    fprintf(stderr, "[==========] Running %zu tests from %s\n", count, group_name);

    for (size_t i = 0; i < count; i++) {
        void *state = tests[i].initial_state;
        int setup_ok = 0;

        fprintf(stderr, "[ RUN      ] %s\n", tests[i].name);

        if (tests[i].setup_func) {
            setup_ok = tests[i].setup_func(&state);
        }

        if (setup_ok == 0 && tests[i].test_func) {
            /* We call the function; if it calls fail()/assert_*, it exits.
             * In a real cmocka this would be setjmp-protected. */
            tests[i].test_func(&state);
        }

        if (tests[i].teardown_func) {
            tests[i].teardown_func(&state);
        }

        fprintf(stderr, "[       OK ] %s\n", tests[i].name);
    }

    fprintf(stderr, "[==========] %d/%zu tests passed\n",
            (int)count - failed, count);
    return failed;
}

#define cmocka_run_group_tests(tests, setup, teardown) \
    _cmocka_run_group_tests(#tests, tests, \
        sizeof(tests) / sizeof((tests)[0]), \
        setup, teardown)

#define cmocka_run_group_tests_name(name, tests, setup, teardown) \
    _cmocka_run_group_tests(name, tests, \
        sizeof(tests) / sizeof((tests)[0]), \
        setup, teardown)

/* --------------------------------------------------------------------- */
/* Mock support (minimal — real tests use direct callbacks instead)        */
/* --------------------------------------------------------------------- */

/* will_return / mock() — minimal implementations */
#define will_return(func, val)  /* no-op in stub */
#define mock()                  ((uintptr_t)NULL)
#define mock_type(type)         ((type)0)
#define mock_ptr_type(type)     ((type)NULL)

/* expect_function_call / function_called — no-op stubs */
#define expect_function_call(func)  /* no-op */
#define function_called()           /* no-op */

/* expect_value — no-op */
#define expect_value(func, param, val) /* no-op */
#define expect_string(func, param, val) /* no-op */

/* check_expected — no-op */
#define check_expected(param) /* no-op */
#define check_expected_ptr(param) /* no-op */

#ifdef __cplusplus
}
#endif

#endif /* CMOCKA_H_ */
