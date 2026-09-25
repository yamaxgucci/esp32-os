/*
 * ArgonOS - the part of SDL2 that Fallout actually uses.
 *
 * The engine talks to the outside world through exactly one header, and the
 * surface it uses is small: 128k lines of game code, and 14 files that include
 * this.  So there is no SDL here - there is a header that declares the same
 * names with the same shapes, and a handful of .cc files behind it that reach
 * the ArgonOS ABI directly.  Porting SDL itself would mean a window system, a
 * renderer and an audio subsystem this OS already has and calls something else.
 *
 * What is declared here is what `grep -rho "SDL_[A-Za-z0-9_]*"` finds in the
 * engine and nothing more.  Anything the engine does not call is absent on
 * purpose: an empty stub that silently does nothing is worse than a link error
 * naming the thing that was forgotten.
 *
 * SDL2 is zlib-licensed, so this interface could have been copied wholesale.
 * It is written out instead because a subset that says what it supports is
 * more useful than a full header that lies about it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_FALLOUT_SDL_H
#define ARGON_FALLOUT_SDL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The real SDL.h pulls these in through SDL_stdinc.h, and four files in the
 * engine reach for malloc, memset, strstr or strncpy having included nothing
 * but this.  Dropping them here would mean patching the engine, which is worth
 * avoiding for as long as possible: every patch is a merge conflict waiting
 * for the next upstream tag.
 */
#include <stdlib.h>
#include <string.h>

/* The engine asks with SDL_VERSION_ATLEAST before using anything recent. */
#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 0
#define SDL_PATCHLEVEL 22
#define SDL_VERSIONNUM(X, Y, Z) ((X) * 1000 + (Y) * 100 + (Z))
#define SDL_COMPILEDVERSION \
    SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL)
#define SDL_VERSION_ATLEAST(X, Y, Z) \
    (SDL_COMPILEDVERSION >= SDL_VERSIONNUM(X, Y, Z))

#ifdef __cplusplus
extern "C" {
#endif

typedef int8_t Sint8;
typedef uint8_t Uint8;
typedef int16_t Sint16;
typedef uint16_t Uint16;
typedef int32_t Sint32;
typedef uint32_t Uint32;
typedef int64_t Sint64;
typedef uint64_t Uint64;

typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

/* ---- geometry and colour ------------------------------------------------ */

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

typedef struct SDL_Color {
    Uint8 r, g, b, a;
} SDL_Color;

typedef struct SDL_Palette {
    int ncolors;
    SDL_Color *colors;
} SDL_Palette;

/*
 * Two pixel formats exist here and no more: the game draws in 8-bit paletted,
 * and presents through a 32-bit surface.  The enum keeps SDL's encoding
 * because SDL_BITSPERPIXEL takes the bits straight out of it, and the engine
 * uses that macro.
 */
#define SDL_PIXELFORMAT_INDEX8 0x13000801u
#define SDL_PIXELFORMAT_RGB565 0x15151002u
#define SDL_PIXELFORMAT_RGB888 0x16161804u
#define SDL_BITSPERPIXEL(fmt) (((fmt) >> 8) & 0xFF)
#define SDL_BYTESPERPIXEL(fmt) ((fmt) & 0xFF)

typedef struct SDL_PixelFormat {
    Uint32 format;
    SDL_Palette *palette;
    Uint8 BitsPerPixel;
    Uint8 BytesPerPixel;
    Uint32 Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormat;

typedef struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h;
    int pitch;
    void *pixels;
    int locked;
    int owns_pixels;
} SDL_Surface;

/* Opaque to the engine; it only ever holds and passes the pointers. */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_Cursor SDL_Cursor;

/* ---- scancodes ----------------------------------------------------------- */

/*
 * The numbering is SDL's own where it is well known, because kb.cc indexes a
 * table by it and a familiar number is easier to read in a dump.  The few this
 * port could not confirm - PRIOR, STOP, COLON, the keypad oddities - are given
 * values in a private range above 300 instead of guessed: nothing outside the
 * engine sees them, and a collision with a real one would be a duplicate case
 * label, which the compiler would refuse.  That is the check.
 */
typedef enum SDL_Scancode {
    SDL_SCANCODE_UNKNOWN = 0,

    SDL_SCANCODE_A = 4,
    SDL_SCANCODE_B = 5,
    SDL_SCANCODE_C = 6,
    SDL_SCANCODE_D = 7,
    SDL_SCANCODE_E = 8,
    SDL_SCANCODE_F = 9,
    SDL_SCANCODE_G = 10,
    SDL_SCANCODE_H = 11,
    SDL_SCANCODE_I = 12,
    SDL_SCANCODE_J = 13,
    SDL_SCANCODE_K = 14,
    SDL_SCANCODE_L = 15,
    SDL_SCANCODE_M = 16,
    SDL_SCANCODE_N = 17,
    SDL_SCANCODE_O = 18,
    SDL_SCANCODE_P = 19,
    SDL_SCANCODE_Q = 20,
    SDL_SCANCODE_R = 21,
    SDL_SCANCODE_S = 22,
    SDL_SCANCODE_T = 23,
    SDL_SCANCODE_U = 24,
    SDL_SCANCODE_V = 25,
    SDL_SCANCODE_W = 26,
    SDL_SCANCODE_X = 27,
    SDL_SCANCODE_Y = 28,
    SDL_SCANCODE_Z = 29,

    SDL_SCANCODE_1 = 30,
    SDL_SCANCODE_2 = 31,
    SDL_SCANCODE_3 = 32,
    SDL_SCANCODE_4 = 33,
    SDL_SCANCODE_5 = 34,
    SDL_SCANCODE_6 = 35,
    SDL_SCANCODE_7 = 36,
    SDL_SCANCODE_8 = 37,
    SDL_SCANCODE_9 = 38,
    SDL_SCANCODE_0 = 39,

    SDL_SCANCODE_RETURN = 40,
    SDL_SCANCODE_ESCAPE = 41,
    SDL_SCANCODE_BACKSPACE = 42,
    SDL_SCANCODE_TAB = 43,
    SDL_SCANCODE_SPACE = 44,

    SDL_SCANCODE_MINUS = 45,
    SDL_SCANCODE_EQUALS = 46,
    SDL_SCANCODE_LEFTBRACKET = 47,
    SDL_SCANCODE_RIGHTBRACKET = 48,
    SDL_SCANCODE_BACKSLASH = 49,
    SDL_SCANCODE_SEMICOLON = 51,
    SDL_SCANCODE_APOSTROPHE = 52,
    SDL_SCANCODE_GRAVE = 53,
    SDL_SCANCODE_COMMA = 54,
    SDL_SCANCODE_PERIOD = 55,
    SDL_SCANCODE_SLASH = 56,

    SDL_SCANCODE_CAPSLOCK = 57,

    SDL_SCANCODE_F1 = 58,
    SDL_SCANCODE_F2 = 59,
    SDL_SCANCODE_F3 = 60,
    SDL_SCANCODE_F4 = 61,
    SDL_SCANCODE_F5 = 62,
    SDL_SCANCODE_F6 = 63,
    SDL_SCANCODE_F7 = 64,
    SDL_SCANCODE_F8 = 65,
    SDL_SCANCODE_F9 = 66,
    SDL_SCANCODE_F10 = 67,
    SDL_SCANCODE_F11 = 68,
    SDL_SCANCODE_F12 = 69,

    SDL_SCANCODE_SCROLLLOCK = 71,
    SDL_SCANCODE_INSERT = 73,
    SDL_SCANCODE_HOME = 74,
    SDL_SCANCODE_DELETE = 76,
    SDL_SCANCODE_PAGEUP = 75,
    SDL_SCANCODE_END = 77,
    SDL_SCANCODE_PAGEDOWN = 78,
    SDL_SCANCODE_RIGHT = 79,
    SDL_SCANCODE_LEFT = 80,
    SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_UP = 82,

    SDL_SCANCODE_NUMLOCKCLEAR = 83,
    SDL_SCANCODE_KP_PERIOD = 99,
    SDL_SCANCODE_SYSREQ = 154,
    SDL_SCANCODE_KP_DIVIDE = 84,
    SDL_SCANCODE_KP_MULTIPLY = 85,
    SDL_SCANCODE_KP_MINUS = 86,
    SDL_SCANCODE_KP_PLUS = 87,
    SDL_SCANCODE_KP_ENTER = 88,
    SDL_SCANCODE_KP_1 = 89,
    SDL_SCANCODE_KP_2 = 90,
    SDL_SCANCODE_KP_3 = 91,
    SDL_SCANCODE_KP_4 = 92,
    SDL_SCANCODE_KP_5 = 93,
    SDL_SCANCODE_KP_6 = 94,
    SDL_SCANCODE_KP_7 = 95,
    SDL_SCANCODE_KP_8 = 96,
    SDL_SCANCODE_KP_9 = 97,
    SDL_SCANCODE_KP_0 = 98,

    SDL_SCANCODE_APPLICATION = 101,

    SDL_SCANCODE_F13 = 104,
    SDL_SCANCODE_F14 = 105,
    SDL_SCANCODE_F15 = 106,

    SDL_SCANCODE_LCTRL = 224,
    SDL_SCANCODE_LSHIFT = 225,
    SDL_SCANCODE_LALT = 226,
    SDL_SCANCODE_LGUI = 227,
    SDL_SCANCODE_RCTRL = 228,
    SDL_SCANCODE_RSHIFT = 229,
    SDL_SCANCODE_RALT = 230,
    SDL_SCANCODE_RGUI = 231,

    /* The unconfirmed ones.  See the note above. */
    SDL_SCANCODE_PRIOR = 300,
    SDL_SCANCODE_STOP = 301,
    SDL_SCANCODE_COLON = 302,
    SDL_SCANCODE_KP_COMMA = 303,
    SDL_SCANCODE_KP_DECIMAL = 304,
    SDL_SCANCODE_KP_EQUALS = 305,

    SDL_NUM_SCANCODES = 512
} SDL_Scancode;

typedef Sint32 SDL_Keycode;

typedef enum SDL_Keymod {
    KMOD_NONE = 0x0000,
    KMOD_LSHIFT = 0x0001,
    KMOD_RSHIFT = 0x0002,
    KMOD_LCTRL = 0x0040,
    KMOD_RCTRL = 0x0080,
    KMOD_LALT = 0x0100,
    KMOD_RALT = 0x0200,
    KMOD_LGUI = 0x0400,
    KMOD_RGUI = 0x0800,
    KMOD_NUM = 0x1000,
    KMOD_CAPS = 0x2000,
    KMOD_SCROLL = 0x8000,
    KMOD_SHIFT = KMOD_LSHIFT | KMOD_RSHIFT,
    KMOD_CTRL = KMOD_LCTRL | KMOD_RCTRL,
    KMOD_ALT = KMOD_LALT | KMOD_RALT
} SDL_Keymod;

typedef struct SDL_Keysym {
    SDL_Scancode scancode;
    SDL_Keycode sym;
    Uint16 mod;
    Uint32 unused;
} SDL_Keysym;

/* ---- events -------------------------------------------------------------- */

#define SDL_RELEASED 0
#define SDL_PRESSED 1

typedef enum {
    SDL_FIRSTEVENT = 0,
    SDL_QUIT = 0x100,
    SDL_WINDOWEVENT = 0x200,
    SDL_KEYDOWN = 0x300,
    SDL_KEYUP = 0x301,
    SDL_TEXTINPUT = 0x303,
    SDL_MOUSEMOTION = 0x400,
    SDL_MOUSEBUTTONDOWN = 0x401,
    SDL_MOUSEBUTTONUP = 0x402,
    SDL_MOUSEWHEEL = 0x403,
    SDL_FINGERDOWN = 0x700,
    SDL_FINGERUP = 0x701,
    SDL_FINGERMOTION = 0x702,
    SDL_LASTEVENT = 0xFFFF
} SDL_EventType;

typedef enum {
    SDL_WINDOWEVENT_EXPOSED = 3,
    SDL_WINDOWEVENT_SIZE_CHANGED = 6,
    SDL_WINDOWEVENT_FOCUS_GAINED = 12,
    SDL_WINDOWEVENT_FOCUS_LOST = 13
} SDL_WindowEventID;

#define SDL_BUTTON(X) (1 << ((X) - 1))
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3

typedef Sint64 SDL_TouchID;
typedef Sint64 SDL_FingerID;

typedef struct SDL_CommonEvent {
    Uint32 type;
    Uint32 timestamp;
} SDL_CommonEvent;

typedef struct SDL_WindowEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 event;
    Sint32 data1, data2;
} SDL_WindowEvent;

typedef struct SDL_KeyboardEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 state;
    Uint8 repeat;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_TextInputEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    char text[32];
} SDL_TextInputEvent;

typedef struct SDL_MouseMotionEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint32 state;
    Sint32 x, y, xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct SDL_MouseButtonEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 button;
    Uint8 state;
    Uint8 clicks;
    Sint32 x, y;
} SDL_MouseButtonEvent;

typedef struct SDL_MouseWheelEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Sint32 x, y;
    Uint32 direction;
} SDL_MouseWheelEvent;

typedef struct SDL_TouchFingerEvent {
    Uint32 type;
    Uint32 timestamp;
    SDL_TouchID touchId;
    SDL_FingerID fingerId;
    float x, y, dx, dy, pressure;
} SDL_TouchFingerEvent;

typedef struct SDL_QuitEvent {
    Uint32 type;
    Uint32 timestamp;
} SDL_QuitEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_CommonEvent common;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_TouchFingerEvent tfinger;
    SDL_QuitEvent quit;
    Uint8 padding[64];
} SDL_Event;

/* ---- audio --------------------------------------------------------------- */

typedef Uint16 SDL_AudioFormat;

#define AUDIO_U8 0x0008
#define AUDIO_S8 0x8008
#define AUDIO_S16LSB 0x8010
#define AUDIO_S16 AUDIO_S16LSB
#define SDL_AUDIO_BITSIZE(x) ((x) & 0xFF)

#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x01
#define SDL_AUDIO_ALLOW_FORMAT_CHANGE 0x02
#define SDL_AUDIO_ALLOW_CHANNELS_CHANGE 0x04
#define SDL_AUDIO_ALLOW_SAMPLES_CHANGE 0x08
#define SDL_AUDIO_ALLOW_ANY_CHANGE 0x0F

#define SDL_MIX_MAXVOLUME 128

typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;

typedef Uint32 SDL_AudioDeviceID;

/* The resampler.  Fallout hands it 8- or 16-bit mono or stereo at whatever
 * rate the sample was recorded at, and takes back the mixer's format. */
typedef struct SDL_AudioStream SDL_AudioStream;

/* ---- subsystems ---------------------------------------------------------- */

#define SDL_INIT_TIMER 0x00000001u
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_EVENTS 0x00004000u

#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000u
#define SDL_WINDOW_FULLSCREEN 0x00000001u
#define SDL_WINDOW_OPENGL 0x00000002u
#define SDL_WINDOW_ALLOW_HIGHDPI 0x00002000u

#define SDL_TEXTUREACCESS_STREAMING 1

#define SDL_DISABLE 0
#define SDL_ENABLE 1

#define SDL_HINT_RENDER_DRIVER "SDL_RENDER_DRIVER"
#define SDL_HINT_MOUSE_TOUCH_EVENTS "SDL_MOUSE_TOUCH_EVENTS"
#define SDL_HINT_TOUCH_MOUSE_EVENTS "SDL_TOUCH_MOUSE_EVENTS"

#define SDL_LOG_CATEGORY_APPLICATION 0
#define SDL_LOG_PRIORITY_INFO 3
#define SDL_MESSAGEBOX_ERROR 0x00000010u

typedef enum { SDL_SYSTEM_CURSOR_ARROW = 0 } SDL_SystemCursor;

typedef Uint32 SDL_TimerID;
typedef Uint32 (*SDL_TimerCallback)(Uint32 interval, void *param);

/* ---- the calls ----------------------------------------------------------- */

int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
void SDL_QuitSubSystem(Uint32 flags);
Uint32 SDL_WasInit(Uint32 flags);
void SDL_Quit(void);
SDL_bool SDL_SetHint(const char *name, const char *value);
int SDL_OutOfMemory(void);

/* video */
SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h,
                             Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
void SDL_SetWindowTitle(SDL_Window *window, const char *title);
SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture,
                   const SDL_Rect *src, const SDL_Rect *dst);
void SDL_RenderPresent(SDL_Renderer *renderer);
SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format,
                               int access, int w, int h);
void SDL_DestroyTexture(SDL_Texture *texture);
int SDL_QueryTexture(SDL_Texture *texture, Uint32 *format, int *access, int *w,
                     int *h);
int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect,
                      const void *pixels, int pitch);

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height,
                                  int depth, Uint32 Rmask, Uint32 Gmask,
                                  Uint32 Bmask, Uint32 Amask);
SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height,
                                            int depth, Uint32 format);
void SDL_FreeSurface(SDL_Surface *surface);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
int SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette);
int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors,
                         int firstcolor, int ncolors);
int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst,
                  SDL_Rect *dstrect);
#define SDL_BlitSurface SDL_UpperBlit

/* input */
int SDL_PollEvent(SDL_Event *event);
int SDL_PushEvent(SDL_Event *event);
const Uint8 *SDL_GetKeyboardState(int *numkeys);
void SDL_PumpEvents(void);
void SDL_FlushEvents(Uint32 minType, Uint32 maxType);
Uint32 SDL_GetRelativeMouseState(int *x, int *y);
int SDL_SetRelativeMouseMode(SDL_bool enabled);
SDL_Keymod SDL_GetModState(void);
int SDL_ShowCursor(int toggle);
SDL_Cursor *SDL_GetCursor(void);
SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id);
void SDL_SetCursor(SDL_Cursor *cursor);
void SDL_FreeCursor(SDL_Cursor *cursor);
void SDL_StartTextInput(void);
void SDL_StopTextInput(void);

/* audio */
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained,
                                      int allowed_changes);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_MixAudioFormat(Uint8 *dst, const Uint8 *src, SDL_AudioFormat format,
                        Uint32 len, int volume);
SDL_AudioStream *SDL_NewAudioStream(SDL_AudioFormat src_format,
                                    Uint8 src_channels, int src_rate,
                                    SDL_AudioFormat dst_format,
                                    Uint8 dst_channels, int dst_rate);
int SDL_AudioStreamPut(SDL_AudioStream *stream, const void *buf, int len);
int SDL_AudioStreamGet(SDL_AudioStream *stream, void *buf, int len);
void SDL_FreeAudioStream(SDL_AudioStream *stream);

/* time */
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback,
                         void *param);
SDL_bool SDL_RemoveTimer(SDL_TimerID id);

/* odds and ends the engine borrows rather than writing itself */
void *SDL_malloc(size_t size);
void SDL_free(void *mem);
char *SDL_strdup(const char *str);
size_t SDL_strlen(const char *str);
int SDL_strcasecmp(const char *a, const char *b);
int SDL_strncasecmp(const char *a, const char *b, size_t len);
char *SDL_strlwr(char *str);
char *SDL_strupr(char *str);
char *SDL_itoa(int value, char *str, int radix);
int SDL_snprintf(char *text, size_t maxlen, const char *fmt, ...);
void SDL_LogMessageV(int category, int priority, const char *fmt, va_list ap);
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title,
                             const char *message, SDL_Window *window);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_FALLOUT_SDL_H */
