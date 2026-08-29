/*
 * ArgonOS - the contract between a C++ .AXE and its runtime.
 *
 * A C++ image does not define ag_main.  cxx_rt.cpp does, so that the static
 * constructors run before anything else and the destructors run after; the
 * application provides ag_cxx_main instead and is otherwise unchanged:
 *
 *     AG_APP("HELLO", "1.0", "you", 0);
 *     extern "C" int ag_cxx_main(int argc, char **argv) { ... }
 *
 * A vendored engine keeps its own main and the port writes the one line that
 * calls it, which is what apps/doom already does for doomgeneric.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_CXX_H
#define AG_CXX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Written by the application; called by cxx_rt.cpp with constructors done. */
int ag_cxx_main(int argc, char **argv);

/*
 * Runs the constructors, and the destructors, by hand.  ag_main does both
 * already - these are here for the case that does not go through it, which
 * today is a .SYS driver: its entry is ag_driver_init, the loader calls that
 * instead, and a driver written in C++ has to say when its statics come up.
 */
void ag_cxx_init(void);
void ag_cxx_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* AG_CXX_H */
