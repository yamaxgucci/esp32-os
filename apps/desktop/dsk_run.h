/*
 * ArgonOS DESKTOP - handing the machine to a program, and getting it back.
 *
 * This is the Program Manager model and it is deliberate (docs/plans/desktop.md
 * §3.1): the shell gives up the display, the program owns the whole screen the
 * way a DOS program under Windows 3.11 did, and when it exits the desktop comes
 * back.  There is no window for somebody else's application and there is not
 * going to be one in v1 - that would need a framebuffer per application on a
 * machine where one is a fifth of the memory.
 *
 * The part that is easy to get wrong: a program that prints and exits has its
 * output wiped by the desktop repainting a millisecond later.  So the shell
 * reads the image's own header first, and a program that did not declare
 * AG_AXE_NEEDS_GFX gets its console left up until a key is pressed - which is
 * what `fm` does, for the same reason.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_RUN_H
#define ARGON_DSK_RUN_H

#include "dsk.h"

/* Called after the display comes back, so the shell can repaint everything. */
void dsk_run_init(void (*repaint_all)(void));

/*
 * Open whatever this is: run a .AXE, hand a file to its [assoc] handler, or
 * say that nothing can open it.  `note` gets a short line for the status
 * strip - it points at storage that outlives the call.
 */
void dsk_run_open(const char *path, const char **note);

/* Run a command line typed into the Run... box. */
void dsk_run_command(const char *line, const char **note);

#endif /* ARGON_DSK_RUN_H */
