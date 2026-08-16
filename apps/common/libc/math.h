/*
 * Minimal libc stand-in header for -nostdlib .AXE builds.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SMS_MATH_H
#define ARGON_SMS_MATH_H

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double sin(double x);
double cos(double x);
double fabs(double x);
float  fabsf(float x);
float  sinf(float x);
float  cosf(float x);
float  powf(float x, float y);
float  logf(float x);
float  floorf(float x);

#endif
