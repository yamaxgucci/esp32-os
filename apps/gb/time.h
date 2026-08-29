/*
 * Just enough <time.h> for Peanut-GB, which includes it for one function.
 *
 * gb_set_rtc() takes a struct tm, for a cartridge with a real time clock in it
 * (MBC3, as in Pokemon Gold).  Nothing here calls it - ArgonOS has no wall
 * clock to offer and a Game Boy cartridge is not the place to start - but the
 * declaration has to compile, and the application libc shim in apps/common/libc
 * does not carry a time.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_GB_TIME_H
#define ARGON_GB_TIME_H

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#endif /* ARGON_GB_TIME_H */
