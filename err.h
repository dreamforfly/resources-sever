#ifndef _ERR_
#define _ERR_

/**
 * Macros which help with error checking.
 */

#define CHECK_SYSFUN(FUN)                                                     \
    if ((FUN) == -1) {                                                        \
        sysfun_err(#FUN, __FILE__, __LINE__);                                 \
    }

#define CHECK_VALUE(VAL, EXP, RET)                                            \
    if ((RET = VAL) != (EXP)) {                                               \
        value_err(#VAL, __FILE__, __LINE__, RET, EXP);                        \
    }

#define TRY_MALLOC(SIZE, PTR)                                                 \
    if ((PTR = malloc(SIZE)) == NULL) {                                       \
        expected_err("Bad malloc");                                           \
    }

#define QERROR -1
#define QREMOVED 0
#define QSUCCESS 1

#define CHECK_QUEUE(RET)                                                      \
    if (RET == -1 && errno != EINVAL && errno != EIDRM) {                     \
        RET = QERROR;                                                         \
    } else if (RET == -1) {                                                   \
        RET = QREMOVED;                                                       \
    } else  {                                                                 \
        RET = QSUCCESS;                                                       \
    }

/* Print error information and errno to stderr and exit. */
extern void sysfun_err(char const* function_string, char const* file, int const line);

/* Print an information about an unexpected value and exit.  */
extern void value_err(char const* value_string, char const* file, int const line,
        int const value, int const expected);

/* Print an information about an expected error and exit. */
extern void expected_err(const char *fmt, ...);

#endif /* _ERR_ */
