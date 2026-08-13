//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	WAD I/O functions.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m_misc.h"
#include "w_file.h"
#include "z_zone.h"

#ifdef ARGON_TARGET
#include <argon/argon.h>
#endif

typedef struct
{
    wad_file_t wad;
    FILE *fstream;
} stdc_wad_file_t;

#ifdef ARGON_TARGET
/* HostFS: R_Init does thousands of tiny fseek+fread. One 256 KiB window. */
#define WAD_RCACHE (256 * 1024)
static unsigned char   *s_rcache;
static stdc_wad_file_t *s_rcache_file;
static unsigned         s_rcache_pos;
static unsigned         s_rcache_len;
#endif

extern wad_file_class_t stdc_wad_file;

static wad_file_t *W_StdC_OpenFile(char *path)
{
    stdc_wad_file_t *result;
    FILE *fstream;

    fstream = fopen(path, "rb");

    if (fstream == NULL)
    {
        return NULL;
    }

    // Create a new stdc_wad_file_t to hold the file handle.

    result = Z_Malloc(sizeof(stdc_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = NULL;
    result->wad.length = M_FileLength(fstream);
    result->fstream = fstream;

    return &result->wad;
}

static void W_StdC_CloseFile(wad_file_t *wad)
{
    stdc_wad_file_t *stdc_wad;

    stdc_wad = (stdc_wad_file_t *) wad;

#ifdef ARGON_TARGET
    if (s_rcache_file == stdc_wad) {
        s_rcache_file = NULL;
        s_rcache_len = 0;
    }
#endif

    fclose(stdc_wad->fstream);
    Z_Free(stdc_wad);
}

// Read data from the specified position in the file into the 
// provided buffer.  Returns the number of bytes read.

size_t W_StdC_Read(wad_file_t *wad, unsigned int offset,
                   void *buffer, size_t buffer_len)
{
    stdc_wad_file_t *stdc_wad;
    size_t           result;

    stdc_wad = (stdc_wad_file_t *) wad;

#ifdef ARGON_TARGET
    {
        unsigned char *dst = (unsigned char *)buffer;
        size_t         left = buffer_len;

        if (s_rcache == NULL) {
            s_rcache = (unsigned char *)malloc(WAD_RCACHE);
        }
        while (left > 0 && s_rcache != NULL) {
            unsigned take;
            if (s_rcache_file != stdc_wad || offset < s_rcache_pos ||
                offset >= s_rcache_pos + s_rcache_len) {
                unsigned want = WAD_RCACHE;
                size_t   got;
                if (offset >= stdc_wad->wad.length) {
                    break;
                }
                if (offset + want > stdc_wad->wad.length) {
                    want = stdc_wad->wad.length - offset;
                }
                if (fseek(stdc_wad->fstream, (long)offset, SEEK_SET) != 0) {
                    break;
                }
                got = fread(s_rcache, 1, want, stdc_wad->fstream);
                s_rcache_file = stdc_wad;
                s_rcache_pos = offset;
                s_rcache_len = (unsigned)got;
                ag_heartbeat();
                if (got == 0) {
                    break;
                }
            }
            take = (s_rcache_pos + s_rcache_len) - offset;
            if (take > left) {
                take = (unsigned)left;
            }
            memcpy(dst, s_rcache + (offset - s_rcache_pos), take);
            dst += take;
            offset += take;
            left -= take;
        }
        if (s_rcache != NULL) {
            return buffer_len - left;
        }
    }
#endif

    fseek(stdc_wad->fstream, offset, SEEK_SET);
    result = fread(buffer, 1, buffer_len, stdc_wad->fstream);

    return result;
}


wad_file_class_t stdc_wad_file = 
{
    W_StdC_OpenFile,
    W_StdC_CloseFile,
    W_StdC_Read,
};


