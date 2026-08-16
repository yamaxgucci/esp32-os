/*
 * Minimal libc stand-in header for -nostdlib .AXE builds.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SMS_CTYPE_H
#define ARGON_SMS_CTYPE_H

int isspace(int c);
int isdigit(int c);
int isxdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isprint(int c);
int isupper(int c);
int islower(int c);
int toupper(int c);
int tolower(int c);

#endif
