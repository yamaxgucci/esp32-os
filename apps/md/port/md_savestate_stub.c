/*
 * ArgonOS Mega Drive — save-state open stubs (nowhere to put the bytes yet).
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "gwenesis_savestate.h"

SaveState *saveGwenesisStateOpenForRead(const char *fileName)
{
    (void)fileName;
    return 0;
}

SaveState *saveGwenesisStateOpenForWrite(const char *fileName)
{
    (void)fileName;
    return 0;
}

int saveGwenesisStateGet(SaveState *state, const char *tagName)
{
    (void)state;
    (void)tagName;
    return 0;
}

void saveGwenesisStateSet(SaveState *state, const char *tagName, int value)
{
    (void)state;
    (void)tagName;
    (void)value;
}

void saveGwenesisStateGetBuffer(SaveState *state, const char *tagName,
                                void *buffer, int length)
{
    (void)state;
    (void)tagName;
    (void)buffer;
    (void)length;
}

void saveGwenesisStateSetBuffer(SaveState *state, const char *tagName,
                                void *buffer, int length)
{
    (void)state;
    (void)tagName;
    (void)buffer;
    (void)length;
}
