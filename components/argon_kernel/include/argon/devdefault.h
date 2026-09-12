/*
 * ArgonOS - which device a class uses when nobody named one.
 *
 * See devdefault.c for why the knowledge is split between the class and this
 * file, and why quality outranks distance.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_DEVDEFAULT_H
#define ARGON_DEVDEFAULT_H

#include <argon/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `name` here, or `<peer>.<name>` on the node next door.  NULL when neither.
 *
 * Local first: a remote device costs a round trip per call, and the placement
 * rule would rather move the process than the samples.
 */
ag_device_t *ag_dev_find_anywhere(const char *name);

/*
 * The system's default device for a class.
 *
 * `prefer` is a NULL-terminated list of device names, BEST FIRST, supplied by
 * the class: which of its own devices is better is a fact about that class and
 * cannot be read off the registry.  Each name is tried here before the node
 * next door, so distance breaks ties and does not decide the question - a real
 * output on the other board must beat a local one that swallows sound.
 *
 * `[cluster] audio|display|net|input` in SYSTEM.CFG comes before all of it: an
 * operator who has said which one means it.
 */
ag_device_t *ag_dev_default(ag_dev_class_t cls, const char *const *prefer);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_DEVDEFAULT_H */
