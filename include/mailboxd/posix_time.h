/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_POSIX_TIME_H
#define MAILBOXD_POSIX_TIME_H

/*
 * clock_gettime(CLOCK_MONOTONIC) and usleep() on glibc/Linux (GCC 15+ C99).
 * Include before <time.h> / <unistd.h> in .c files, or rely on CMake
 * _DEFAULT_SOURCE on Linux builds.
 */
#if defined(__linux__) || defined(__GLIBC__)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#endif

#include <time.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#endif /* MAILBOXD_POSIX_TIME_H */
