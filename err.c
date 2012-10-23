#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <stddef.h>
#include <stdbool.h>

#include "err.h"

static void disable_cancel() {
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL) != 0) {
        exit(EXIT_FAILURE);
    }
}

void sysfun_err(char const* function_string, char const* file, int const line) {
    disable_cancel();
    fprintf(stderr,
            "Unexpected value of %s in file %s, at line %d.\n"
            "Errno: %d, strerror(errno): %s\n", function_string, file, line,
            errno, strerror(errno));
    exit(EXIT_FAILURE);
}

void value_err(char const* value_string, char const* file, int const line,
        int const value, int const expected) {
    disable_cancel();
    fprintf(stderr,
            "Unexpected value of %s in file %s, at line %d.\n"
            "Is %d, expected %d.\n", value_string, file, line, value,
            expected);
    exit(EXIT_FAILURE);
}

void expected_err(const char *fmt, ...) {
  va_list fmt_args;

  disable_cancel();

  va_start(fmt_args, fmt);
  vfprintf(stderr, fmt, fmt_args);
  va_end (fmt_args);

  exit(EXIT_FAILURE);
}

