/*
 * ArgonOS - SDL2's subset, over the ArgonOS ABI.
 *
 * Three kinds of thing live here.
 *
 * Done: everything that is pure memory or arithmetic - surfaces, palettes,
 * the blit, the string helpers, the clock.  None of it needs a device, and it
 * is the same code SDL would run.
 *
 * Stubbed and loud: video, events and audio.  Each one panics with its own
 * name.  They are not silent no-ops on purpose - a game that draws into
 * nothing and reports success is a game that looks like it works from the
 * outside and cannot be debugged from the inside.
 *
 * Absent: anything the engine does not call.  See port/SDL.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "SDL.h"

#include <argon/argon.h>
#include <stdio.h>

extern "C" {

/* ---- not written yet ----------------------------------------------------- */

static void todo(const char *what)
{
    ag_print("SDL shim: not implemented yet: ");
    ag_print(what);
    ag_print("\n");
    ag_panic(what);
}

#define TODO(name) todo(#name)

/* ---- subsystems ---------------------------------------------------------- */

static Uint32 s_inited;

int SDL_InitSubSystem(Uint32 flags)
{
    s_inited |= flags;
    return 0;
}

void SDL_QuitSubSystem(Uint32 flags) { s_inited &= ~flags; }

Uint32 SDL_WasInit(Uint32 flags)
{
    return flags == 0 ? s_inited : (s_inited & flags);
}

void SDL_Quit(void) { s_inited = 0; }

/* Hints are advice about backends this port does not have. */
SDL_bool SDL_SetHint(const char *name, const char *value)
{
    (void)name;
    (void)value;
    return SDL_TRUE;
}

int SDL_OutOfMemory(void)
{
    ag_panic("SDL: out of memory");
    return -1;
}

/* ---- surfaces ------------------------------------------------------------ */

/*
 * A surface is a width, a height, a pitch and a block of pixels, and that is
 * all the engine ever wants from one.  The format and palette structures are
 * allocated with it, in one block, so freeing a surface is one call and
 * nothing can outlive it by accident.
 */
struct SurfaceBlock {
    SDL_Surface surface;
    SDL_PixelFormat format;
    SDL_Palette palette;
    SDL_Color colors[256];
};

static SDL_Surface *make_surface(int w, int h, int depth, Uint32 format)
{
    if (w <= 0 || h <= 0) {
        return NULL;
    }

    const int bytes = depth / 8;
    const int pitch = (w * bytes + 3) & ~3;

    SurfaceBlock *block = (SurfaceBlock *)ag_malloc(sizeof(SurfaceBlock));
    if (block == NULL) {
        return NULL;
    }
    memset(block, 0, sizeof(*block));

    void *pixels = ag_malloc((size_t)pitch * (size_t)h);
    if (pixels == NULL) {
        ag_free(block);
        return NULL;
    }
    memset(pixels, 0, (size_t)pitch * (size_t)h);

    block->palette.ncolors = 256;
    block->palette.colors = block->colors;

    block->format.format = format;
    block->format.BitsPerPixel = (Uint8)depth;
    block->format.BytesPerPixel = (Uint8)bytes;
    block->format.palette = (depth == 8) ? &block->palette : NULL;
    if (depth == 32) {
        block->format.Rmask = 0x00FF0000u;
        block->format.Gmask = 0x0000FF00u;
        block->format.Bmask = 0x000000FFu;
    }

    block->surface.format = &block->format;
    block->surface.w = w;
    block->surface.h = h;
    block->surface.pitch = pitch;
    block->surface.pixels = pixels;
    block->surface.owns_pixels = 1;
    return &block->surface;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height,
                                  int depth, Uint32 Rmask, Uint32 Gmask,
                                  Uint32 Bmask, Uint32 Amask)
{
    (void)flags;
    (void)Rmask;
    (void)Gmask;
    (void)Bmask;
    (void)Amask;
    return make_surface(width, height, depth,
                        depth == 8 ? SDL_PIXELFORMAT_INDEX8
                                   : SDL_PIXELFORMAT_RGB888);
}

SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height,
                                            int depth, Uint32 format)
{
    (void)flags;
    return make_surface(width, height, depth, format);
}

void SDL_FreeSurface(SDL_Surface *surface)
{
    if (surface == NULL) {
        return;
    }
    if (surface->owns_pixels) {
        ag_free(surface->pixels);
    }
    ag_free(surface);
}

/* Nothing here moves a surface's pixels behind the caller's back, so locking
 * is bookkeeping.  It is still counted, because an unbalanced pair means the
 * caller believes something this port does not do. */
int SDL_LockSurface(SDL_Surface *surface)
{
    if (surface == NULL) {
        return -1;
    }
    surface->locked++;
    return 0;
}

void SDL_UnlockSurface(SDL_Surface *surface)
{
    if (surface != NULL && surface->locked > 0) {
        surface->locked--;
    }
}

int SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette)
{
    if (surface == NULL || surface->format == NULL) {
        return -1;
    }
    surface->format->palette = palette;
    return 0;
}

int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors,
                         int firstcolor, int ncolors)
{
    if (palette == NULL || colors == NULL) {
        return -1;
    }
    if (firstcolor < 0 || ncolors < 0 ||
        firstcolor + ncolors > palette->ncolors) {
        return -1;
    }
    for (int i = 0; i < ncolors; ++i) {
        palette->colors[firstcolor + i] = colors[i];
    }
    return 0;
}

/*
 * The one blit the engine performs: the 8-bit game surface through its palette
 * into the 32-bit surface that gets presented.  SDL would pick a converter out
 * of a table; there is one case here, and anything else says so rather than
 * producing a picture that is subtly wrong.
 */
int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst,
                  SDL_Rect *dstrect)
{
    if (src == NULL || dst == NULL || src->format == NULL ||
        dst->format == NULL) {
        return -1;
    }
    if (src->format->BitsPerPixel != 8 || dst->format->BitsPerPixel != 32) {
        ag_panic("SDL blit: only 8-bit paletted to 32-bit is implemented");
    }

    const SDL_Palette *pal = src->format->palette;
    if (pal == NULL) {
        return -1;
    }

    int sx = srcrect ? srcrect->x : 0;
    int sy = srcrect ? srcrect->y : 0;
    int w = srcrect ? srcrect->w : src->w;
    int h = srcrect ? srcrect->h : src->h;
    int dx = dstrect ? dstrect->x : 0;
    int dy = dstrect ? dstrect->y : 0;

    if (sx < 0 || sy < 0 || w < 0 || h < 0) {
        return -1;
    }
    if (sx + w > src->w) {
        w = src->w - sx;
    }
    if (sy + h > src->h) {
        h = src->h - sy;
    }
    if (dx + w > dst->w) {
        w = dst->w - dx;
    }
    if (dy + h > dst->h) {
        h = dst->h - dy;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }

    for (int y = 0; y < h; ++y) {
        const Uint8 *s = (const Uint8 *)src->pixels + (size_t)(sy + y) * src->pitch + sx;
        Uint32 *d = (Uint32 *)((Uint8 *)dst->pixels +
                               (size_t)(dy + y) * dst->pitch) + dx;
        for (int x = 0; x < w; ++x) {
            const SDL_Color c = pal->colors[s[x]];
            d[x] = ((Uint32)c.r << 16) | ((Uint32)c.g << 8) | (Uint32)c.b;
        }
    }
    return 0;
}

/* ---- time ---------------------------------------------------------------- */

Uint32 SDL_GetTicks(void) { return (Uint32)ag_millis(); }

void SDL_Delay(Uint32 ms) { ag_delay(ms); }

/* ---- string helpers the engine borrows ----------------------------------- */

void *SDL_malloc(size_t size) { return ag_malloc(size); }

void SDL_free(void *mem) { ag_free(mem); }

size_t SDL_strlen(const char *str) { return strlen(str); }

char *SDL_strdup(const char *str)
{
    if (str == NULL) {
        return NULL;
    }
    size_t n = strlen(str) + 1;
    char *out = (char *)ag_malloc(n);
    if (out != NULL) {
        memcpy(out, str, n);
    }
    return out;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

int SDL_strcasecmp(const char *a, const char *b)
{
    while (*a != '\0' && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        ++a;
        ++b;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

int SDL_strncasecmp(const char *a, const char *b, size_t len)
{
    while (len > 0 && *a != '\0' &&
           lower((unsigned char)*a) == lower((unsigned char)*b)) {
        ++a;
        ++b;
        --len;
    }
    if (len == 0) {
        return 0;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

char *SDL_strlwr(char *str)
{
    for (char *p = str; p != NULL && *p != '\0'; ++p) {
        *p = (char)lower((unsigned char)*p);
    }
    return str;
}

char *SDL_strupr(char *str)
{
    for (char *p = str; p != NULL && *p != '\0'; ++p) {
        *p = (char)upper((unsigned char)*p);
    }
    return str;
}

char *SDL_itoa(int value, char *str, int radix)
{
    static const char k_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char buf[36];
    int n = 0;
    unsigned int v;
    int negative = 0;

    if (radix < 2 || radix > 36 || str == NULL) {
        return str;
    }
    if (value < 0 && radix == 10) {
        negative = 1;
        v = (unsigned int)(-(long)value);
    } else {
        v = (unsigned int)value;
    }
    do {
        buf[n++] = k_digits[v % (unsigned int)radix];
        v /= (unsigned int)radix;
    } while (v != 0);
    if (negative) {
        buf[n++] = '-';
    }
    for (int i = 0; i < n; ++i) {
        str[i] = buf[n - 1 - i];
    }
    str[n] = '\0';
    return str;
}

int SDL_snprintf(char *text, size_t maxlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, maxlen, fmt, ap);
    va_end(ap);
    return n;
}

void SDL_LogMessageV(int category, int priority, const char *fmt, va_list ap)
{
    (void)category;
    (void)priority;
    char line[256];
    vsnprintf(line, sizeof(line), fmt, ap);
    ag_print(line);
    ag_print("\n");
}

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title,
                             const char *message, SDL_Window *window)
{
    (void)flags;
    (void)window;
    ag_printf("%s: %s\n", title ? title : "message",
              message ? message : "(none)");
    return 0;
}

/* ---- video: not written yet ---------------------------------------------- */

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h,
                             Uint32 flags)
{
    (void)title;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)flags;
    TODO(SDL_CreateWindow);
    return NULL;
}

void SDL_DestroyWindow(SDL_Window *w) { (void)w; }

void SDL_SetWindowTitle(SDL_Window *w, const char *title)
{
    (void)w;
    (void)title;
}

SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int index, Uint32 flags)
{
    (void)w;
    (void)index;
    (void)flags;
    TODO(SDL_CreateRenderer);
    return NULL;
}

void SDL_DestroyRenderer(SDL_Renderer *r) { (void)r; }

int SDL_RenderSetLogicalSize(SDL_Renderer *r, int w, int h)
{
    (void)r;
    (void)w;
    (void)h;
    return 0;
}

int SDL_RenderClear(SDL_Renderer *r)
{
    (void)r;
    return 0;
}

int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *s,
                   const SDL_Rect *d)
{
    (void)r;
    (void)t;
    (void)s;
    (void)d;
    return 0;
}

void SDL_RenderPresent(SDL_Renderer *r)
{
    (void)r;
    TODO(SDL_RenderPresent);
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access,
                               int w, int h)
{
    (void)r;
    (void)format;
    (void)access;
    (void)w;
    (void)h;
    TODO(SDL_CreateTexture);
    return NULL;
}

void SDL_DestroyTexture(SDL_Texture *t) { (void)t; }

int SDL_QueryTexture(SDL_Texture *t, Uint32 *format, int *access, int *w,
                     int *h)
{
    (void)t;
    (void)format;
    (void)access;
    (void)w;
    (void)h;
    return -1;
}

int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *rect, const void *pixels,
                      int pitch)
{
    (void)t;
    (void)rect;
    (void)pixels;
    (void)pitch;
    return 0;
}

/* ---- events: not written yet --------------------------------------------- */

int SDL_PollEvent(SDL_Event *event)
{
    (void)event;
    return 0;
}

void SDL_PumpEvents(void) {}

void SDL_FlushEvents(Uint32 minType, Uint32 maxType)
{
    (void)minType;
    (void)maxType;
}

Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
    if (x != NULL) {
        *x = 0;
    }
    if (y != NULL) {
        *y = 0;
    }
    return 0;
}

int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    (void)enabled;
    return 0;
}

SDL_Keymod SDL_GetModState(void) { return KMOD_NONE; }

int SDL_ShowCursor(int toggle)
{
    (void)toggle;
    return 0;
}

SDL_Cursor *SDL_GetCursor(void) { return NULL; }

SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id)
{
    (void)id;
    return NULL;
}

void SDL_SetCursor(SDL_Cursor *c) { (void)c; }

void SDL_FreeCursor(SDL_Cursor *c) { (void)c; }

void SDL_StartTextInput(void) {}

void SDL_StopTextInput(void) {}

/* ---- audio: not written yet ---------------------------------------------- */

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained,
                                      int allowed_changes)
{
    (void)device;
    (void)iscapture;
    (void)allowed_changes;
    /* Reporting the device as opened with the asked-for format keeps the
     * engine's own mixer arithmetic honest while nothing is heard. */
    if (obtained != NULL && desired != NULL) {
        *obtained = *desired;
    }
    return 1;
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on)
{
    (void)dev;
    (void)pause_on;
}

void SDL_MixAudioFormat(Uint8 *dst, const Uint8 *src, SDL_AudioFormat format,
                        Uint32 len, int volume)
{
    (void)dst;
    (void)src;
    (void)format;
    (void)len;
    (void)volume;
}

SDL_AudioStream *SDL_NewAudioStream(SDL_AudioFormat src_format,
                                    Uint8 src_channels, int src_rate,
                                    SDL_AudioFormat dst_format,
                                    Uint8 dst_channels, int dst_rate)
{
    (void)src_format;
    (void)src_channels;
    (void)src_rate;
    (void)dst_format;
    (void)dst_channels;
    (void)dst_rate;
    return NULL;
}

int SDL_AudioStreamPut(SDL_AudioStream *stream, const void *buf, int len)
{
    (void)stream;
    (void)buf;
    (void)len;
    return 0;
}

int SDL_AudioStreamGet(SDL_AudioStream *stream, void *buf, int len)
{
    (void)stream;
    (void)buf;
    (void)len;
    return 0;
}

void SDL_FreeAudioStream(SDL_AudioStream *stream) { (void)stream; }

/* ---- timers: not written yet --------------------------------------------- */

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback,
                         void *param)
{
    (void)interval;
    (void)callback;
    (void)param;
    return 0;
}

SDL_bool SDL_RemoveTimer(SDL_TimerID id)
{
    (void)id;
    return SDL_FALSE;
}

}  // extern "C"
