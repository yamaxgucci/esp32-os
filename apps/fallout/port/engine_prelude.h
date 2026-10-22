/*
 * ArgonOS - forced into the front of every engine translation unit.
 *
 * The engine is vendored and not ours to edit, and three of its files use
 * getenv, pow and sqrtf without including the header that declares them.  That
 * compiles on a desktop because glibc's headers include one another freely;
 * newlib's do not, and the compiler is right to object.
 *
 * So the declarations are put in front of every file instead, with -include.
 * Nothing else belongs here: this is a list of things upstream forgot, not a
 * place to configure the engine.  apps/doom does the same with doom_cfg.h.
 *
 * The C headers rather than <cmath> and <cstdlib>, deliberately.  The engine
 * calls these unqualified, at global scope, which is where math.h and stdlib.h
 * put them; <cmath> is only obliged to put them in namespace std.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_ENGINE_PRELUDE_H
#define AG_ENGINE_PRELUDE_H

#include <math.h>
#include <stdlib.h>

/*
 * And one that no header on this side declares because it is not standard
 * anywhere: strupr is a DOS extension that the map editor still uses.  Defined
 * in port/platform.cc.
 */
#ifdef __cplusplus
extern "C" {
#endif

char *strupr(char *s);
char *strlwr(char *s);

#ifdef __cplusplus
}
#endif

#endif /* AG_ENGINE_PRELUDE_H */
