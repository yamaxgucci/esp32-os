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

/*
 * The operator's choice, at run time: `dev default audio zero.pcm0`.  A name
 * from the one device list - this board's or a neighbour's, the list does not
 * say which wire - and it comes before [cluster] in SYSTEM.CFG and before the
 * class's own preference.  NULL or "" gives the choice back.  -AG_EINVAL for a
 * class that has no default; the name is not checked, because the device it
 * names may be a board that has not come up yet.
 */
ag_err_t ag_dev_default_choose(ag_dev_class_t cls, const char *name);
/* What was chosen for the class, or NULL. */
const char *ag_dev_default_chosen(ag_dev_class_t cls);

/*
 * The chosen GROUP, `a+b+c`: its members present now, into `out`, in the
 * order they were named; 0 when the choice is not a group.  See devdefault.c.
 */
int ag_dev_default_group(ag_dev_class_t cls, ag_device_t **out, int max);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_DEVDEFAULT_H */
