/* Minimal test harness: each test file defines main() and ends with CHECK_RESULT(). */
#ifndef BARTAP_TEST_CHECK_H
#define BARTAP_TEST_CHECK_H

#include <stdio.h>

static int check_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("%s:%d: failed: %s\n", __FILE__, __LINE__, #cond);     \
            check_failures++;                                             \
        }                                                                 \
    } while (0)

#define CHECK_RESULT() (check_failures == 0 ? 0 : 1)

#endif /* BARTAP_TEST_CHECK_H */
