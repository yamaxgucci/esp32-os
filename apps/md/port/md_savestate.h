/*
 * ArgonOS Mega Drive - save states (md_savestate.c).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef MD_SAVESTATE_H
#define MD_SAVESTATE_H

#define MD_STATE_OK        0
#define MD_STATE_NO_FILE   1 /* could not be opened (no save yet, on load) */
#define MD_STATE_NO_MEMORY 2
#define MD_STATE_BAD       3 /* short, or not this build's (on load: the
                              * machine is then half loaded - reset it) */
#define MD_STATE_BOUNCE    512

/* Between frames, on the emulating core, with nothing drawing. */
int md_state_save(const char *path);
int md_state_load(const char *path);

#endif /* MD_SAVESTATE_H */
