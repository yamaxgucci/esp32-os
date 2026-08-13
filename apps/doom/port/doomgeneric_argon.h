#ifndef DOOMGENERIC_ARGON_H
#define DOOMGENERIC_ARGON_H

int  doom_argon_quit(void);
int  doom_argon_poll_sys(void);
void doom_argon_set_live_pad(int on);
void doom_argon_set_sound_path(const char *path);
/* Coalesced since last call. buttons: bit0 L, bit1 R, bit2 M, bit3 wheel-, bit4 wheel+. */
int  doom_argon_get_mouse(int *buttons, int *dx, int *dy);

#endif
