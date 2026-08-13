#include <stdio.h>

#include "m_argv.h"

#include "doomgeneric.h"

pixel_t* DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

#ifdef ARGON_TARGET
	/* Present path reads I_VideoBuffer; keep a tiny dummy so other code is happy. */
	DG_ScreenBuffer = malloc(4);
#else
	DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
#endif

	DG_Init();

	D_DoomMain ();
}

