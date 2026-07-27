/*
 * aguishot.c -- dump the public screen to a simple palette image file
 *
 * The point of this is host side verification: the emulator writes into a
 * directory hard drive, so whatever the Amiga saves here shows up on the
 * Mac immediately. tools/ags2png.py turns it into a PNG.
 *
 * File layout, all big endian:
 *
 *   char  magic[4]   "AGSH"
 *   UWORD version    1
 *   UWORD width
 *   UWORD height
 *   UWORD colors
 *   UBYTE palette[colors][3]     r, g, b
 *   UBYTE pixels[height][width]  palette indices
 *
 * The planar to chunky conversion is done by hand rather than with
 * ReadPixelLine8(). That call needs a deep temporary bitmap, only fills as
 * many planes as the source has, and is not dependable outside AmigaOS
 * proper. Walking the bitplanes is portable and a good deal faster.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <intuition/screens.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

#include <string.h>

#include "agui.h"

extern struct GfxBase *GfxBase;

struct Window *AGUI_Window(struct AGUIApp *app);

static void put_w(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)(v & 0xff);
}

/* one bitplane row -> chunky bytes, oring in this plane's bit */
static void merge_plane(UBYTE *line, const UBYTE *src, LONG width, UBYTE bit)
{
    LONG x = 0;

    while (x < width)
    {
        UBYTE v = *src++;

        if (v)
        {
            LONG n = width - x;
            LONG i;

            if (n > 8)
                n = 8;
            for (i = 0; i < n; i++)
                if (v & (0x80 >> i))
                    line[x + i] |= bit;
        }
        x += 8;
    }
}

BOOL AGUI_Snapshot(struct AGUIApp *app, CONST_STRPTR filename)
{
    struct Window  *win = AGUI_Window(app);
    struct Screen  *scr;
    struct BitMap  *bm;
    UBYTE          *line = NULL;
    ULONG          *table = NULL;
    UBYTE           header[12 + 256 * 3];
    BPTR            fh = 0;
    LONG            width, height, depth, colors, bpr, y, p, i;
    BOOL            ok = FALSE;

    if (!win)
        return FALSE;
    scr = win->WScreen;
    bm  = scr->RastPort.BitMap;
    /*
     * Only plain planar bitmaps can be walked directly. Graphics card and
     * AROS style bitmaps keep private data in Planes[] and must not be
     * dereferenced.
     */
    if (!bm || !bm->Planes[0])
        return FALSE;
    if (bm->Flags & BMF_HIJACKED)                      /* aka BMF_SPECIALFMT */
        return FALSE;

    width  = scr->Width;
    height = scr->Height;
    bpr    = bm->BytesPerRow;
    depth  = bm->Depth;
    if (depth > 8)
        depth = 8;
    colors = 1L << depth;

    if (height > bm->Rows)
        height = bm->Rows;
    if (width > bpr * 8)
        width = bpr * 8;

    line  = AllocVec(width, MEMF_CLEAR | MEMF_PUBLIC);
    table = AllocVec(sizeof(ULONG) * 3 * colors + 8, MEMF_CLEAR | MEMF_PUBLIC);
    if (!line || !table)
        goto out;

    memcpy(header, "AGSH", 4);
    put_w(header + 4, 1);
    put_w(header + 6, (UWORD)width);
    put_w(header + 8, (UWORD)height);
    put_w(header + 10, (UWORD)colors);

    /* GetRGB32 is V39. Under OS 2.x fall back to the 4 bit per gun call. */
    if (scr->ViewPort.ColorMap)
    {
        if (GfxBase->LibNode.lib_Version >= 39)
        {
            GetRGB32(scr->ViewPort.ColorMap, 0, colors, table);
            for (i = 0; i < colors; i++)
            {
                header[12 + i * 3 + 0] = (UBYTE)(table[i * 3 + 0] >> 24);
                header[12 + i * 3 + 1] = (UBYTE)(table[i * 3 + 1] >> 24);
                header[12 + i * 3 + 2] = (UBYTE)(table[i * 3 + 2] >> 24);
            }
        }
        else
        {
            for (i = 0; i < colors; i++)
            {
                ULONG v = GetRGB4(scr->ViewPort.ColorMap, i);

                header[12 + i * 3 + 0] = (UBYTE)(((v >> 8) & 0xf) * 0x11);
                header[12 + i * 3 + 1] = (UBYTE)(((v >> 4) & 0xf) * 0x11);
                header[12 + i * 3 + 2] = (UBYTE)((v & 0xf) * 0x11);
            }
        }
    }

    fh = Open((STRPTR)filename, MODE_NEWFILE);
    if (!fh)
        goto out;
    if (Write(fh, header, 12 + colors * 3) != 12 + colors * 3)
        goto out;

    for (y = 0; y < height; y++)
    {
        memset(line, 0, width);
        for (p = 0; p < depth; p++)
            if (bm->Planes[p])
                merge_plane(line, bm->Planes[p] + y * bpr, width,
                            (UBYTE)(1 << p));
        if (Write(fh, line, width) != width)
            goto out;
    }
    ok = TRUE;

out:
    if (fh)
        Close(fh);
    if (table)
        FreeVec(table);
    if (line)
        FreeVec(line);
    return ok;
}
