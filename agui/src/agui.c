/*
 * agui.c -- Intuition/GadTools application framework, AmigaOS 3.1+
 *
 * Everything here sticks to Kickstart 3.1 (V40) APIs so that binaries run
 * unmodified on a stock A500+/A600/A1200/A4000 as well as on 3.5/3.9/3.2.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/sghooks.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <utility/hooks.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/utility.h>
#include <proto/asl.h>
#include <clib/alib_protos.h>       /* NewList() and friends, from amiga.lib */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "agui.h"

/* ------------------------------------------------------------------ */
/* layout constants, all in pixels unless noted                        */
/* ------------------------------------------------------------------ */

#define PAD      4      /* margin between window border and contents   */
#define HGAP     4      /* gap between widgets on the same row         */
#define VGAP     3      /* gap between rows                            */
#define LABGAP   8      /* gap between a left label and its gadget     */

#define MAXROWS  64

struct AGRow
{
    UWORD  r_First;     /* index of first widget in the row            */
    UWORD  r_Count;
    UWORD  r_MinHeight;
    UWORD  r_Height;    /* after distributing spare space              */
    UWORD  r_Top;
    UWORD  r_VWeight;
    UWORD  r_LabelCol;  /* width reserved for the leading label, or 0  */
};

struct AGUIApp
{
    struct AGSpec     *a_Spec;
    struct AGWidget   *a_Widgets;
    UWORD              a_Count;

    struct Screen     *a_Screen;
    struct DrawInfo   *a_DrawInfo;
    APTR               a_VisualInfo;
    struct Window     *a_Window;
    struct Gadget     *a_GList;
    struct Menu       *a_Menu;

    struct MsgPort    *a_TimerPort;
    struct timerequest *a_TimerReq;
    BOOL               a_TimerOpen;
    BOOL               a_TimerPending;
    UWORD              a_TickInterval;  /* 1/10 s, 0 = no timer            */
    UWORD              a_ShotCountdown; /* ticks left before the auto shot */

    UWORD              a_FontW;
    UWORD              a_FontH;
    UWORD              a_LabelCol;   /* global left label column width  */
    UWORD              a_MinW;       /* minimum inner width             */
    UWORD              a_MinH;       /* minimum inner height            */

    struct AGRow       a_Rows[MAXROWS];
    UWORD              a_NumRows;

    BOOL               a_Done;
};

/* library bases -- opened by AGUI_Open, closed by AGUI_Close */
struct Library      *GadToolsBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase      *GfxBase = NULL;
struct Library      *UtilityBase = NULL;
struct Library      *AslBase = NULL;    /* only while a requester is up */

/*
 * How many AGUIApps currently hold the libraries open. A modal dialog is an
 * ordinary second AGUIApp opened from inside the first one's handler, so
 * closing it must not pull the libraries out from under the window that is
 * still on screen.
 */
static LONG agui_libs_users = 0;

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static char agui_logfile[128] = "";
static char agui_autoshot[128] = "";

void AGUI_AutoSnapshot(CONST_STRPTR filename)
{
    if (filename)
    {
        strncpy(agui_autoshot, (const char *)filename, sizeof(agui_autoshot) - 1);
        agui_autoshot[sizeof(agui_autoshot) - 1] = '\0';
    }
    else
        agui_autoshot[0] = '\0';
}

void AGUI_LogTo(CONST_STRPTR filename)
{
    if (filename)
    {
        strncpy(agui_logfile, (const char *)filename, sizeof(agui_logfile) - 1);
        agui_logfile[sizeof(agui_logfile) - 1] = '\0';
    }
    else
        agui_logfile[0] = '\0';
}

void AGUI_Log(CONST_STRPTR fmt, ...)
{
    char    buf[512];
    va_list ap;
    BPTR    fh;
    int     len;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, (const char *)fmt, ap);
    va_end(ap);

    len = (int)strlen(buf);
    buf[len++] = '\n';
    buf[len] = '\0';

    if (agui_logfile[0])
    {
        fh = Open(agui_logfile, MODE_READWRITE);
        if (fh)
        {
            Seek(fh, 0, OFFSET_END);
            Write(fh, buf, len);
            Close(fh);
        }
    }

    /* also to the console, when we were started from a shell */
    fh = Output();
    if (fh)
        Write(fh, buf, len);
}

/* ------------------------------------------------------------------ */
/* list handling for AG_LISTVIEW                                       */
/* ------------------------------------------------------------------ */

struct AGListNode
{
    struct Node  ln_Node;
    char         ln_Text[1];   /* variable length                      */
};

struct AGList
{
    struct List  al_List;
};

static struct AGList *list_new(void)
{
    struct AGList *l = AllocMem(sizeof(struct AGList), MEMF_CLEAR | MEMF_PUBLIC);
    if (l)
        NewList(&l->al_List);
    return l;
}

static void list_empty(struct AGList *l)
{
    struct Node *n;

    if (!l)
        return;
    while ((n = RemHead(&l->al_List)))
    {
        struct AGListNode *an = (struct AGListNode *)n;
        FreeVec(an);
    }
}

static void list_free(struct AGList *l)
{
    if (!l)
        return;
    list_empty(l);
    FreeMem(l, sizeof(struct AGList));
}

static void list_add(struct AGList *l, CONST_STRPTR text)
{
    ULONG len;
    struct AGListNode *n;

    if (!l || !text)
        return;
    len = strlen((const char *)text);
    n = AllocVec(sizeof(struct AGListNode) + len + 1, MEMF_CLEAR | MEMF_PUBLIC);
    if (!n)
        return;
    strcpy(n->ln_Text, (const char *)text);
    n->ln_Node.ln_Name = n->ln_Text;
    AddTail(&l->al_List, &n->ln_Node);
}

/* ------------------------------------------------------------------ */
/* widget lookup                                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* password gadgets                                                    */
/* ------------------------------------------------------------------ */

/*
 * A GadTools string gadget shows whatever is in its buffer, so a password
 * field keeps two: the gadget holds asterisks, and the text the user
 * actually typed lives here, out of sight of anyone reading over their
 * shoulder or looking at a screenshot.
 *
 * They are kept in step by a string edit hook. Intuition applies each
 * keystroke to a work buffer and then calls the hook, which works out
 * what changed, makes the same change to the real text, and hands back a
 * row of asterisks to display.
 *
 * The character just typed is left legible until the next keystroke, and
 * is covered up when the gadget is left. That is the useful half of the
 * "show it for a moment" behaviour without a timer poking at a gadget
 * somebody is in the middle of editing.
 */
struct AGPassword
{
    struct Hook      pw_Hook;
    struct AGWidget *pw_Widget;
    ULONG            pw_Max;
    char             pw_Mask[1];   /* pw_Max+1 asterisks, then pw_Max+1
                                    * bytes of real text -- see pw_real */
};

static char *pw_real(struct AGPassword *p)
{
    return p->pw_Mask + p->pw_Max + 1;
}

static void pw_mask(struct AGPassword *p, LONG len)
{
    LONG i;

    if (len > (LONG)p->pw_Max)
        len = p->pw_Max;
    for (i = 0; i < len; i++)
        p->pw_Mask[i] = '*';
    p->pw_Mask[len] = 0;
}

static ULONG pw_edit(struct Hook *hook, struct SGWork *sgw, ULONG *cmd)
{
    struct AGPassword *p = (struct AGPassword *)hook->h_Data;
    char *real;
    LONG oldlen, newlen, pos, i, reveal = -1;

    if (!p || *cmd != SGH_KEY)
        return 0;                      /* not ours, let Intuition decide */

    real   = pw_real(p);
    oldlen = (LONG)strlen(real);
    newlen = sgw->NumChars;
    pos    = sgw->BufferPos;

    if (newlen < 0 || newlen > (LONG)p->pw_Max)
        return 0;
    if (pos < 0)
        pos = 0;
    if (pos > newlen)
        pos = newlen;

    if (newlen > oldlen)
    {
        /* n characters were inserted, ending at the cursor */
        LONG n  = newlen - oldlen;
        LONG at = pos - n;

        if (at < 0)
            at = 0;
        memmove(real + at + n, real + at, (size_t)(oldlen - at + 1));
        for (i = 0; i < n; i++)
            real[at + i] = sgw->WorkBuffer[at + i];
        if (n == 1)
            reveal = at;
    }
    else if (newlen < oldlen)
    {
        /*
         * n characters were removed at the cursor. Backspace leaves the
         * cursor where the deleted character was, and so does delete, so
         * one case covers both -- and also the clear and delete-to-end
         * keys, which land at 0 and at the cursor respectively.
         */
        LONG n  = oldlen - newlen;
        LONG at = pos;

        if (at > newlen)
            at = newlen;
        memmove(real + at, real + at + n, (size_t)(oldlen - at - n + 1));
    }

    for (i = 0; i < newlen; i++)
        sgw->WorkBuffer[i] = (i == reveal) ? real[i] : '*';
    sgw->WorkBuffer[newlen] = 0;

    sgw->Actions |= SGA_USE | SGA_REDISPLAY;
    return ~0UL;
}

static struct AGPassword *pw_new(struct AGWidget *w)
{
    ULONG max = w->ag_MaxChars ? w->ag_MaxChars : 128;
    struct AGPassword *p = AllocVec(sizeof(struct AGPassword) + 2 * (max + 1),
                                    MEMF_CLEAR | MEMF_PUBLIC);

    if (!p)
        return NULL;
    p->pw_Max    = max;
    p->pw_Widget = w;
    p->pw_Hook.h_Entry    = (ULONG (*)())HookEntry;
    p->pw_Hook.h_SubEntry = (ULONG (*)())pw_edit;
    p->pw_Hook.h_Data     = p;

    if (w->ag_Text)
        strncpy(pw_real(p), (char *)w->ag_Text, max);
    pw_mask(p, (LONG)strlen(pw_real(p)));
    return p;
}

/* cover the character the hook left legible */
static void pw_hide(struct AGUIApp *app, struct AGWidget *w)
{
    struct AGPassword *p = (struct AGPassword *)w->ag_Private;

    if (!p)
        return;
    pw_mask(p, (LONG)strlen(pw_real(p)));
    if (w->ag_Gadget)
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTST_String, (ULONG)p->pw_Mask, TAG_END);
}

static struct AGWidget *find_widget(struct AGUIApp *app, UWORD id)
{
    UWORD i;

    if (!app || !id)
        return NULL;
    for (i = 0; i < app->a_Count; i++)
        if (app->a_Widgets[i].ag_ID == id)
            return &app->a_Widgets[i];
    return NULL;
}

static struct AGWidget *find_by_gadget(struct AGUIApp *app, struct Gadget *g)
{
    UWORD i;

    for (i = 0; i < app->a_Count; i++)
        if (app->a_Widgets[i].ag_Gadget == g)
            return &app->a_Widgets[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* metrics                                                             */
/* ------------------------------------------------------------------ */

static UWORD text_width(struct AGUIApp *app, CONST_STRPTR s)
{
    if (!s || !*s)
        return 0;
    return (UWORD)TextLength(&app->a_Screen->RastPort, (STRPTR)s,
                             (ULONG)strlen((const char *)s));
}

/* does this kind get its label drawn to the left of the gadget? */
static BOOL has_left_label(struct AGWidget *w)
{
    if (!w->ag_Label || !*w->ag_Label)
        return FALSE;
    return (w->ag_Kind != AG_BUTTON && w->ag_Kind != AG_SPACE);
}

static UWORD widget_height(struct AGUIApp *app, struct AGWidget *w)
{
    UWORD base = app->a_FontH + 6;

    switch (w->ag_Kind)
    {
    case AG_LISTVIEW:
        /* ag_Max is the wanted number of visible rows, 4 if unset */
        return (UWORD)((w->ag_Max > 0 ? w->ag_Max : 4) * (app->a_FontH + 1) + 4);
    case AG_SPACE:
        return (UWORD)(app->a_FontH / 2);
    default:
        return base;
    }
}

/* how many characters the level readout of a slider needs */
static UWORD slider_digits(struct AGWidget *w)
{
    LONG  v[2];
    UWORD i, best = 1;

    v[0] = w->ag_Min;
    v[1] = w->ag_Max;
    for (i = 0; i < 2; i++)
    {
        LONG  n = v[i];
        UWORD d = 1;

        if (n < 0)
        {
            d++;
            n = -n;
        }
        while (n >= 10)
        {
            n /= 10;
            d++;
        }
        if (d > best)
            best = d;
    }
    return best;
}

/*
 * Space a widget needs to the right of its own box. GadTools draws the
 * slider level outside ng_Width, so it has to be budgeted for or it lands
 * on top of the window border.
 */
static UWORD widget_right_reserve(struct AGUIApp *app, struct AGWidget *w)
{
    if (w->ag_Kind == AG_SLIDER)
        return (UWORD)((slider_digits(w) + 1) * app->a_FontW);
    return 0;
}

static UWORD widget_natural_width(struct AGUIApp *app, struct AGWidget *w)
{
    UWORD n = 0;
    STRPTR *p;

    switch (w->ag_Kind)
    {
    case AG_BUTTON:
        n = (UWORD)(text_width(app, w->ag_Label) + 3 * app->a_FontW);
        if (n < 8 * app->a_FontW)
            n = (UWORD)(8 * app->a_FontW);
        /* room for a longer title the application may set later */
        if (w->ag_MaxChars)
        {
            UWORD want = (UWORD)((w->ag_MaxChars + 3) * app->a_FontW);
            if (want > n)
                n = want;
        }
        break;

    case AG_CHECKBOX:
        n = 26;
        break;

    case AG_CYCLE:
        if (w->ag_Labels)
            for (p = w->ag_Labels; *p; p++)
            {
                UWORD t = text_width(app, *p);
                if (t > n)
                    n = t;
            }
        n = (UWORD)(n + 4 * app->a_FontW);
        break;

    case AG_TEXT:
        n = text_width(app, w->ag_Text ? w->ag_Text : (STRPTR)"");
        n = (UWORD)(n + 2 * app->a_FontW);
        if (n < 8 * app->a_FontW)
            n = (UWORD)(8 * app->a_FontW);
        break;

    case AG_SPACE:
        n = app->a_FontW;
        break;

    default:                       /* string, integer, slider, listview */
        n = (UWORD)(12 * app->a_FontW);
        break;
    }
    return (UWORD)(n + widget_right_reserve(app, w));
}

static UWORD widget_hweight(struct AGWidget *w)
{
    if (w->ag_Flags & AGF_NOWEIGHT)
        return 0;
    switch (w->ag_Kind)
    {
    case AG_BUTTON:
    case AG_CHECKBOX:
    case AG_CYCLE:
        return 0;
    default:
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* row partitioning and minimum size                                   */
/* ------------------------------------------------------------------ */

static void compute_rows(struct AGUIApp *app)
{
    UWORD i, r = 0;
    UWORD labelcol = 0;

    app->a_NumRows = 0;

    for (i = 0; i < app->a_Count; i++)
    {
        struct AGWidget *w = &app->a_Widgets[i];

        if (i == 0 || !(w->ag_Flags & AGF_SAMEROW))
        {
            if (app->a_NumRows >= MAXROWS)
                break;
            r = app->a_NumRows++;
            app->a_Rows[r].r_First    = i;
            app->a_Rows[r].r_Count    = 0;
            app->a_Rows[r].r_VWeight  = 0;
            app->a_Rows[r].r_LabelCol = 0;

            if (has_left_label(w))
            {
                UWORD lw = (UWORD)(text_width(app, w->ag_Label) + LABGAP);
                if (lw > labelcol)
                    labelcol = lw;
                app->a_Rows[r].r_LabelCol = 1;   /* marker, real width later */
            }
        }
        app->a_Rows[r].r_Count++;
        /* AGF_GROW alone decides vertical growth. AGF_NOWEIGHT is about
         * width, and a list that should not stretch downwards must not be
         * forced to stop stretching sideways as well. */
        if (w->ag_Flags & AGF_GROW)
            app->a_Rows[r].r_VWeight = 1;
    }

    app->a_LabelCol = labelcol;

    /* row heights and the overall minimum size */
    app->a_MinW = 0;
    app->a_MinH = 0;

    for (r = 0; r < app->a_NumRows; r++)
    {
        struct AGRow *row = &app->a_Rows[r];
        UWORD h = 0, wsum = 0, k;

        if (row->r_LabelCol)
            row->r_LabelCol = labelcol;

        for (k = 0; k < row->r_Count; k++)
        {
            struct AGWidget *w = &app->a_Widgets[row->r_First + k];
            UWORD wh = widget_height(app, w);
            UWORD ww = widget_natural_width(app, w);

            if (k > 0 && has_left_label(w))
                ww = (UWORD)(ww + text_width(app, w->ag_Label) + LABGAP);
            if (k > 0)
                ww = (UWORD)(ww + HGAP);
            wsum = (UWORD)(wsum + ww);
            if (wh > h)
                h = wh;
        }
        row->r_MinHeight = h;
        row->r_Height    = h;

        wsum = (UWORD)(wsum + row->r_LabelCol);
        if (wsum > app->a_MinW)
            app->a_MinW = wsum;

        app->a_MinH = (UWORD)(app->a_MinH + h);
        if (r + 1 < app->a_NumRows)
            app->a_MinH = (UWORD)(app->a_MinH + VGAP);
    }

    if (app->a_Spec->ag_MinCols)
    {
        UWORD want = (UWORD)(app->a_Spec->ag_MinCols * app->a_FontW);
        if (want > app->a_MinW)
            app->a_MinW = want;
    }
}

/* ------------------------------------------------------------------ */
/* gadget creation                                                     */
/* ------------------------------------------------------------------ */

static struct Gadget *make_gadget(struct AGUIApp *app, struct Gadget *prev,
                                  struct AGWidget *w, struct NewGadget *ng)
{
    struct Gadget *g = NULL;

    switch (w->ag_Kind)
    {
    case AG_BUTTON:
        ng->ng_GadgetText = w->ag_Label;
        ng->ng_Flags      = PLACETEXT_IN;
        g = CreateGadget(BUTTON_KIND, prev, ng, TAG_END);
        break;

    case AG_TEXT:
        g = CreateGadget(TEXT_KIND, prev, ng,
                         GTTX_Text,   (ULONG)(w->ag_Text ? w->ag_Text : (STRPTR)""),
                         GTTX_Border, TRUE,
                         GTTX_CopyText, TRUE,
                         TAG_END);
        break;

    case AG_STRING:
        /* ag_Private is the shadow buffer that carries the contents across
         * a rebuild, see save_live_state() */
        g = CreateGadget(STRING_KIND, prev, ng,
                         GTST_String,   (ULONG)(w->ag_Private ? (STRPTR)w->ag_Private
                                                              : (STRPTR)""),
                         GTST_MaxChars, (ULONG)(w->ag_MaxChars ? w->ag_MaxChars : 128),
                         TAG_END);
        break;

    case AG_PASSWORD:
        {
            struct AGPassword *p = (struct AGPassword *)w->ag_Private;

            if (!p)
                return prev;
            g = CreateGadget(STRING_KIND, prev, ng,
                             GTST_String,   (ULONG)p->pw_Mask,
                             GTST_MaxChars, (ULONG)p->pw_Max,
                             GTST_EditHook, (ULONG)&p->pw_Hook,
                             TAG_END);
        }
        break;

    case AG_INTEGER:
        g = CreateGadget(INTEGER_KIND, prev, ng,
                         GTIN_Number,   (ULONG)w->ag_Value,
                         GTIN_MaxChars, 12,
                         TAG_END);
        break;

    case AG_CHECKBOX:
        g = CreateGadget(CHECKBOX_KIND, prev, ng,
                         GTCB_Checked, (ULONG)(w->ag_Value ? TRUE : FALSE),
                         GTCB_Scaled,  TRUE,
                         TAG_END);
        break;

    case AG_CYCLE:
        g = CreateGadget(CYCLE_KIND, prev, ng,
                         GTCY_Labels, (ULONG)w->ag_Labels,
                         GTCY_Active, (ULONG)w->ag_Value,
                         TAG_END);
        break;

    case AG_SLIDER:
        g = CreateGadget(SLIDER_KIND, prev, ng,
                         GTSL_Min,     (ULONG)w->ag_Min,
                         GTSL_Max,     (ULONG)w->ag_Max,
                         GTSL_Level,   (ULONG)w->ag_Value,
                         GTSL_MaxLevelLen, (ULONG)slider_digits(w),
                         GTSL_LevelFormat, (ULONG)"%ld",
                         GTSL_LevelPlace,  PLACETEXT_RIGHT,
                         GA_RelVerify, TRUE,
                         TAG_END);
        break;

    case AG_LISTVIEW:
        if (!w->ag_Private)
            w->ag_Private = (APTR)list_new();
        g = CreateGadget(LISTVIEW_KIND, prev, ng,
                         GTLV_Labels,       (ULONG)(w->ag_Private
                                              ? &((struct AGList *)w->ag_Private)->al_List
                                              : NULL),
                         GTLV_Selected,     (ULONG)w->ag_Value,
                         GTLV_ScrollWidth,  16,
                         TAG_END);
        break;

    case AG_SPACE:
    default:
        return prev;               /* nothing to create, keep the chain */
    }

    if (g && (w->ag_Flags & AGF_DISABLED))
        GT_SetGadgetAttrs(g, NULL, NULL, GA_Disabled, TRUE, TAG_END);

    w->ag_Gadget = g;
    return g ? g : prev;
}

/*
 * Lays the widget table out into the window's inner rectangle and attaches
 * the resulting gadget list. Called on open and again on every resize.
 */
static BOOL build_gadgets(struct AGUIApp *app)
{
    struct Window *win = app->a_Window;
    struct Gadget *gad;
    struct NewGadget ng;
    LONG x0, y0, iw, ih;
    LONG spare, totalw;
    UWORD r, k, i;

    for (i = 0; i < app->a_Count; i++)
        app->a_Widgets[i].ag_Gadget = NULL;

    app->a_GList = NULL;
    gad = CreateContext(&app->a_GList);
    if (!gad)
        return FALSE;

    x0 = win->BorderLeft + PAD;
    y0 = win->BorderTop  + PAD;
    iw = (LONG)win->Width  - win->BorderLeft - win->BorderRight  - 2 * PAD;
    ih = (LONG)win->Height - win->BorderTop  - win->BorderBottom - 2 * PAD;

    /* distribute spare vertical space over the rows that asked for it */
    totalw = 0;
    for (r = 0; r < app->a_NumRows; r++)
    {
        app->a_Rows[r].r_Height = app->a_Rows[r].r_MinHeight;
        totalw += app->a_Rows[r].r_VWeight;
    }
    spare = ih - (LONG)app->a_MinH;
    if (spare > 0 && totalw > 0)
    {
        LONG given = 0;
        for (r = 0; r < app->a_NumRows; r++)
        {
            if (app->a_Rows[r].r_VWeight)
            {
                LONG add = spare * app->a_Rows[r].r_VWeight / totalw;
                app->a_Rows[r].r_Height = (UWORD)(app->a_Rows[r].r_Height + add);
                given += add;
            }
        }
        /* hand the rounding remainder to the last flexible row */
        if (given < spare)
            for (r = app->a_NumRows; r > 0; r--)
                if (app->a_Rows[r - 1].r_VWeight)
                {
                    app->a_Rows[r - 1].r_Height =
                        (UWORD)(app->a_Rows[r - 1].r_Height + (spare - given));
                    break;
                }
    }

    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr   = app->a_Screen->Font;
    ng.ng_VisualInfo = app->a_VisualInfo;

    {
        LONG y = y0;

        for (r = 0; r < app->a_NumRows; r++)
        {
            struct AGRow *row = &app->a_Rows[r];
            LONG x = x0 + row->r_LabelCol;
            LONG avail = iw - row->r_LabelCol;
            LONG natsum = 0, wsum = 0;

            row->r_Top = (UWORD)y;

            for (k = 0; k < row->r_Count; k++)
            {
                struct AGWidget *w = &app->a_Widgets[row->r_First + k];
                natsum += widget_natural_width(app, w);
                if (k > 0)
                {
                    natsum += HGAP;
                    if (has_left_label(w))
                        natsum += text_width(app, w->ag_Label) + LABGAP;
                }
                wsum += widget_hweight(w);
            }

            {
                LONG extra = avail - natsum;
                LONG handed = 0;

                if (extra < 0)
                    extra = 0;

                for (k = 0; k < row->r_Count; k++)
                {
                    struct AGWidget *w = &app->a_Widgets[row->r_First + k];
                    LONG gw = widget_natural_width(app, w);
                    LONG hw = widget_hweight(w);

                    if (k > 0)
                    {
                        x += HGAP;
                        if (has_left_label(w))
                            x += text_width(app, w->ag_Label) + LABGAP;
                    }
                    if (hw && wsum)
                    {
                        LONG add = extra * hw / wsum;
                        if (k == row->r_Count - 1)
                            add = extra - handed;    /* soak up rounding */
                        gw += add;
                        handed += add;
                    }
                    if (gw < 8)
                        gw = 8;

                    /* the box itself is narrower than the footprint when the
                     * gadget draws something to its right */
                    {
                        LONG reserve = widget_right_reserve(app, w);
                        LONG boxw = gw - reserve;
                        if (boxw < 8)
                            boxw = 8;
                        ng.ng_Width = (WORD)boxw;
                    }

                    ng.ng_LeftEdge   = (WORD)x;
                    ng.ng_TopEdge    = (WORD)y;
                    ng.ng_Height     = (WORD)((w->ag_Kind == AG_LISTVIEW)
                                              ? row->r_Height
                                              : widget_height(app, w));
                    ng.ng_GadgetID   = w->ag_ID;
                    ng.ng_UserData   = (APTR)w;
                    ng.ng_GadgetText = has_left_label(w) ? w->ag_Label : NULL;
                    ng.ng_Flags      = PLACETEXT_LEFT;

                    gad = make_gadget(app, gad, w, &ng);
                    if (!gad)
                    {
                        FreeGadgets(app->a_GList);
                        app->a_GList = NULL;
                        return FALSE;
                    }
                    x += gw;
                }
            }
            y += row->r_Height + VGAP;
        }
    }

    AddGList(win, app->a_GList, -1, -1, NULL);
    RefreshGList(app->a_GList, win, NULL, -1);
    GT_RefreshWindow(win, NULL);
    return TRUE;
}

/*
 * Gadgets are thrown away and recreated on every resize, so anything the
 * user changed has to be pulled back out of them first.
 */
static void save_live_state(struct AGUIApp *app)
{
    UWORD i;

    for (i = 0; i < app->a_Count; i++)
    {
        struct AGWidget *w = &app->a_Widgets[i];
        struct StringInfo *si;

        if (!w->ag_Gadget)
            continue;

        switch (w->ag_Kind)
        {
        case AG_STRING:
            si = (struct StringInfo *)w->ag_Gadget->SpecialInfo;
            if (si && si->Buffer && w->ag_Private)
            {
                ULONG max = w->ag_MaxChars ? w->ag_MaxChars : 128;
                strncpy((char *)w->ag_Private, (char *)si->Buffer, max);
                ((char *)w->ag_Private)[max] = '\0';
            }
            break;
        case AG_INTEGER:
            si = (struct StringInfo *)w->ag_Gadget->SpecialInfo;
            if (si)
                w->ag_Value = si->LongInt;
            break;
        case AG_CHECKBOX:
            w->ag_Value = (w->ag_Gadget->Flags & GFLG_SELECTED) ? 1 : 0;
            break;
        default:
            break;
        }
    }
}

static void tear_down_gadgets(struct AGUIApp *app)
{
    if (app->a_GList)
    {
        RemoveGList(app->a_Window, app->a_GList, -1);
        FreeGadgets(app->a_GList);
        app->a_GList = NULL;
    }
}

static void clear_interior(struct AGUIApp *app)
{
    struct Window *win = app->a_Window;
    struct RastPort *rp = win->RPort;
    LONG pen = 0;

    if (app->a_DrawInfo)
        pen = app->a_DrawInfo->dri_Pens[BACKGROUNDPEN];

    SetAPen(rp, pen);
    SetDrMd(rp, JAM1);
    RectFill(rp, win->BorderLeft, win->BorderTop,
             win->Width - win->BorderRight - 1,
             win->Height - win->BorderBottom - 1);
}

static void relayout(struct AGUIApp *app)
{
    save_live_state(app);
    tear_down_gadgets(app);
    clear_interior(app);
    build_gadgets(app);
}

/* ------------------------------------------------------------------ */
/* timer                                                               */
/* ------------------------------------------------------------------ */

static void timer_start(struct AGUIApp *app)
{
    UWORD ticks;

    if (!app->a_TimerOpen)
        return;
    ticks = app->a_TickInterval;
    app->a_TimerReq->tr_node.io_Command = TR_ADDREQUEST;
    app->a_TimerReq->tr_time.tv_secs    = ticks / 10;
    app->a_TimerReq->tr_time.tv_micro   = (ticks % 10) * 100000;
    SendIO((struct IORequest *)app->a_TimerReq);
    app->a_TimerPending = TRUE;
}

static void timer_open(struct AGUIApp *app)
{
    if (!app->a_TickInterval)
        return;
    app->a_TimerPort = CreateMsgPort();
    if (!app->a_TimerPort)
        return;
    app->a_TimerReq = (struct timerequest *)
        CreateIORequest(app->a_TimerPort, sizeof(struct timerequest));
    if (!app->a_TimerReq)
        return;
    if (OpenDevice("timer.device", UNIT_VBLANK,
                   (struct IORequest *)app->a_TimerReq, 0) == 0)
        app->a_TimerOpen = TRUE;
}

static void timer_close(struct AGUIApp *app)
{
    if (app->a_TimerOpen)
    {
        if (app->a_TimerPending)
        {
            AbortIO((struct IORequest *)app->a_TimerReq);
            WaitIO((struct IORequest *)app->a_TimerReq);
            app->a_TimerPending = FALSE;
        }
        CloseDevice((struct IORequest *)app->a_TimerReq);
        app->a_TimerOpen = FALSE;
    }
    if (app->a_TimerReq)
    {
        DeleteIORequest((struct IORequest *)app->a_TimerReq);
        app->a_TimerReq = NULL;
    }
    if (app->a_TimerPort)
    {
        DeleteMsgPort(app->a_TimerPort);
        app->a_TimerPort = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

static void drop_libs(void)
{
    if (UtilityBase)   { CloseLibrary(UtilityBase);   UtilityBase = NULL; }
    if (GadToolsBase)  { CloseLibrary(GadToolsBase);  GadToolsBase = NULL; }
    if (GfxBase)       { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase);
                         IntuitionBase = NULL; }
}

static BOOL open_libs(void)
{
    if (agui_libs_users > 0)
    {
        agui_libs_users++;
        return TRUE;
    }

    /* v37 across the board: that is OS 2.04, which is as far back as
     * gadtools goes. Anything newer is used only after a version check. */
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    UtilityBase   = OpenLibrary("utility.library", 37);

    if (IntuitionBase && GfxBase && GadToolsBase && UtilityBase)
    {
        agui_libs_users = 1;
        return TRUE;
    }
    drop_libs();               /* a partial open owns nothing */
    return FALSE;
}

static void close_libs(void)
{
    if (agui_libs_users <= 0)
        return;
    if (--agui_libs_users == 0)
        drop_libs();
}

struct AGUIApp *AGUI_Open(struct AGSpec *spec)
{
    struct AGUIApp *app;
    UWORD i;
    LONG w, h;

    if (!spec || !spec->ag_Widgets)
        return NULL;
    if (!open_libs())
        return NULL;

    app = AllocMem(sizeof(struct AGUIApp), MEMF_CLEAR | MEMF_PUBLIC);
    if (!app)
    {
        close_libs();
        return NULL;
    }
    app->a_Spec    = spec;
    app->a_Widgets = spec->ag_Widgets;

    for (i = 0; spec->ag_Widgets[i].ag_Kind != AG_END && i < 512; i++)
        ;
    app->a_Count = i;

    app->a_Screen = LockPubScreen(NULL);
    if (!app->a_Screen)
        goto fail;

    app->a_DrawInfo   = GetScreenDrawInfo(app->a_Screen);
    app->a_VisualInfo = GetVisualInfo(app->a_Screen, TAG_END);
    if (!app->a_VisualInfo)
        goto fail;

    app->a_FontH = app->a_Screen->RastPort.TxHeight;
    app->a_FontW = app->a_Screen->RastPort.TxWidth;
    if (!app->a_FontW)
        app->a_FontW = 8;
    if (!app->a_FontH)
        app->a_FontH = 8;

    /* per widget storage that has to outlive the gadgets themselves, so a
     * resize does not throw away what the user typed or picked */
    for (i = 0; i < app->a_Count; i++)
    {
        struct AGWidget *w = &app->a_Widgets[i];

        if (w->ag_Kind == AG_LISTVIEW && !w->ag_Private)
            w->ag_Private = (APTR)list_new();
        else if (w->ag_Kind == AG_PASSWORD)
        {
            if (!(w->ag_Private = (APTR)pw_new(w)))
                goto fail;
        }
        else if (w->ag_Kind == AG_STRING)
        {
            ULONG max = w->ag_MaxChars ? w->ag_MaxChars : 128;
            char *buf = AllocVec(max + 1, MEMF_CLEAR | MEMF_PUBLIC);

            if (!buf)
                goto fail;
            if (w->ag_Text)
                strncpy(buf, (char *)w->ag_Text, max);
            w->ag_Private = (APTR)buf;
        }
    }

    compute_rows(app);

    /* a first guess at the outer size, corrected once the real borders
     * of the window are known */
    w = app->a_MinW + app->a_Screen->WBorLeft + app->a_Screen->WBorRight
        + 2 * PAD + 18;
    h = app->a_MinH + app->a_Screen->WBorTop + app->a_Screen->WBorBottom
        + app->a_FontH + 3 + 2 * PAD;

    if (w > app->a_Screen->Width)
        w = app->a_Screen->Width;
    if (h > app->a_Screen->Height)
        h = app->a_Screen->Height;

    app->a_Window = OpenWindowTags(NULL,
        WA_Title,        (ULONG)spec->ag_Title,
        WA_ScreenTitle,  (ULONG)(spec->ag_ScreenTitle ? spec->ag_ScreenTitle
                                                      : spec->ag_Title),
        WA_PubScreen,    (ULONG)app->a_Screen,
        WA_Left,         20,
        WA_Top,          app->a_Screen->BarHeight + 10,
        WA_InnerWidth,   (ULONG)(app->a_MinW + 2 * PAD),
        WA_InnerHeight,  (ULONG)(app->a_MinH + 2 * PAD),
        WA_MinWidth,     100,
        WA_MinHeight,    50,
        WA_MaxWidth,     (ULONG)~0,
        WA_MaxHeight,    (ULONG)~0,
        WA_DragBar,      TRUE,
        WA_DepthGadget,  TRUE,
        WA_CloseGadget,  TRUE,
        WA_SizeGadget,   TRUE,
        WA_SizeBRight,   TRUE,
        WA_Activate,     TRUE,
        WA_SmartRefresh, TRUE,
        WA_AutoAdjust,   TRUE,
        WA_NewLookMenus, TRUE,
        WA_IDCMP,        IDCMP_CLOSEWINDOW  | IDCMP_REFRESHWINDOW |
                         IDCMP_NEWSIZE      | IDCMP_GADGETUP      |
                         IDCMP_GADGETDOWN   | IDCMP_MOUSEMOVE     |
                         IDCMP_MENUPICK     | IDCMP_VANILLAKEY    |
                         IDCMP_INTUITICKS,
        TAG_END);

    if (!app->a_Window)
        goto fail;

    UnlockPubScreen(NULL, app->a_Screen);

    /* now that the real border sizes are known, pin down the minimum */
    {
        struct Window *win = app->a_Window;
        LONG minw = app->a_MinW + win->BorderLeft + win->BorderRight + 2 * PAD;
        LONG minh = app->a_MinH + win->BorderTop + win->BorderBottom + 2 * PAD;

        WindowLimits(win, minw, minh, -1, -1);
        if (win->Width < minw || win->Height < minh)
            ChangeWindowBox(win, win->LeftEdge, win->TopEdge,
                            win->Width  < minw ? minw : win->Width,
                            win->Height < minh ? minh : win->Height);
    }

    if (spec->ag_Menu)
    {
        app->a_Menu = CreateMenus(spec->ag_Menu, GTMN_FrontPen, 0, TAG_END);
        if (app->a_Menu)
        {
            if (LayoutMenus(app->a_Menu, app->a_VisualInfo,
                            GTMN_NewLookMenus, TRUE, TAG_END))
                SetMenuStrip(app->a_Window, app->a_Menu);
            else
            {
                FreeMenus(app->a_Menu);
                app->a_Menu = NULL;
            }
        }
    }

    if (!build_gadgets(app))
        goto fail;

    app->a_TickInterval = spec->ag_Ticks;
    if (agui_autoshot[0])
    {
        /* the auto snapshot needs a timer even if the app did not ask for
         * one, and it should not have to wait for a slow application tick */
        if (!app->a_TickInterval || app->a_TickInterval > 5)
            app->a_TickInterval = 5;
        app->a_ShotCountdown = 3;
    }
    timer_open(app);
    return app;

fail:
    AGUI_Close(app);
    return NULL;
}

void AGUI_Close(struct AGUIApp *app)
{
    UWORD i;

    if (!app)
        return;

    timer_close(app);

    if (app->a_Window)
    {
        if (app->a_Menu)
            ClearMenuStrip(app->a_Window);
        tear_down_gadgets(app);
        CloseWindow(app->a_Window);
        app->a_Window = NULL;
    }
    else if (app->a_Screen)
        UnlockPubScreen(NULL, app->a_Screen);

    if (app->a_Menu)
    {
        FreeMenus(app->a_Menu);
        app->a_Menu = NULL;
    }
    if (app->a_VisualInfo)
    {
        FreeVisualInfo(app->a_VisualInfo);
        app->a_VisualInfo = NULL;
    }
    if (app->a_DrawInfo && app->a_Screen)
    {
        FreeScreenDrawInfo(app->a_Screen, app->a_DrawInfo);
        app->a_DrawInfo = NULL;
    }
    for (i = 0; i < app->a_Count; i++)
    {
        struct AGWidget *w = &app->a_Widgets[i];

        if (!w->ag_Private)
            continue;
        if (w->ag_Kind == AG_LISTVIEW)
            list_free((struct AGList *)w->ag_Private);
        else if (w->ag_Kind == AG_STRING || w->ag_Kind == AG_PASSWORD)
        {
            /* a password buffer is wiped, not just handed back */
            if (w->ag_Kind == AG_PASSWORD)
            {
                struct AGPassword *p = (struct AGPassword *)w->ag_Private;
                memset(pw_real(p), 0, p->pw_Max + 1);
            }
            FreeVec(w->ag_Private);
        }
        w->ag_Private = NULL;
    }

    FreeMem(app, sizeof(struct AGUIApp));
    close_libs();
}

void AGUI_Quit(struct AGUIApp *app)
{
    if (app)
        app->a_Done = TRUE;
}

APTR AGUI_UserData(struct AGUIApp *app)
{
    return app ? app->a_Spec->ag_UserData : NULL;
}

struct Window *AGUI_Window(struct AGUIApp *app)
{
    return app ? app->a_Window : NULL;
}

struct Gadget *AGUI_Gadget(struct AGUIApp *app, UWORD id)
{
    struct AGWidget *w = find_widget(app, id);

    return w ? w->ag_Gadget : NULL;
}

/* ------------------------------------------------------------------ */
/* event loop                                                          */
/* ------------------------------------------------------------------ */

static void dispatch(struct AGUIApp *app, struct AGEvent *ev)
{
    if (app->a_Spec->ag_Handler)
        app->a_Spec->ag_Handler(app, ev);
}

/* everything waiting on the window's port, then back to the caller */
static void pump_messages(struct AGUIApp *app)
{
    struct IntuiMessage *imsg;
    struct AGEvent ev;

    while ((imsg = GT_GetIMsg(app->a_Window->UserPort)))
    {
        ULONG class = imsg->Class;
        UWORD code  = imsg->Code;
        UWORD qual  = imsg->Qualifier;
        APTR  iaddr = imsg->IAddress;

        GT_ReplyIMsg(imsg);

        memset(&ev, 0, sizeof(ev));
        ev.ev_Code      = code;
        ev.ev_Qualifier = qual;

        switch (class)
        {
        case IDCMP_CLOSEWINDOW:
            ev.ev_Type = AGE_CLOSE;
            dispatch(app, &ev);
            break;

        case IDCMP_REFRESHWINDOW:
            GT_BeginRefresh(app->a_Window);
            GT_EndRefresh(app->a_Window, TRUE);
            break;

        case IDCMP_NEWSIZE:
            relayout(app);
            break;

        case IDCMP_VANILLAKEY:
            ev.ev_Type = AGE_KEY;
            dispatch(app, &ev);
            break;

        case IDCMP_MENUPICK:
            {
                UWORD mnum = code;
                while (mnum != MENUNULL && !app->a_Done)
                {
                    struct MenuItem *item = ItemAddress(app->a_Menu, mnum);
                    if (!item)
                        break;
                    ev.ev_Type = AGE_MENU;
                    ev.ev_Code = (ULONG)GTMENUITEM_USERDATA(item);
                    ev.ev_ID   = (UWORD)(ULONG)GTMENUITEM_USERDATA(item);
                    dispatch(app, &ev);
                    mnum = item->NextSelect;
                }
            }
            break;

        case IDCMP_GADGETUP:
        case IDCMP_GADGETDOWN:
            {
                struct Gadget *g = (struct Gadget *)iaddr;
                struct AGWidget *w = find_by_gadget(app, g);

                if (w)
                {
                    ev.ev_ID = w->ag_ID;
                    switch (w->ag_Kind)
                    {
                    case AG_BUTTON:
                        if (class == IDCMP_GADGETUP)
                        {
                            ev.ev_Type = AGE_CLICK;
                            dispatch(app, &ev);
                        }
                        break;
                    case AG_CHECKBOX:
                        w->ag_Value = (g->Flags & GFLG_SELECTED) ? 1 : 0;
                        ev.ev_Type  = AGE_CHANGE;
                        ev.ev_Value = w->ag_Value;
                        if (class == IDCMP_GADGETUP)
                            dispatch(app, &ev);
                        break;
                    case AG_PASSWORD:
                        /* leaving the field covers the last character
                         * the edit hook left legible */
                        if (class == IDCMP_GADGETUP)
                        {
                            pw_hide(app, w);
                            ev.ev_Type = AGE_CHANGE;
                            dispatch(app, &ev);
                        }
                        break;
                    case AG_STRING:
                        if (class == IDCMP_GADGETUP)
                        {
                            ev.ev_Type = AGE_CHANGE;
                            dispatch(app, &ev);
                        }
                        break;
                    default:
                        if (class == IDCMP_GADGETUP ||
                            w->ag_Kind == AG_SLIDER)
                        {
                            w->ag_Value = (LONG)code;
                            ev.ev_Type  = AGE_CHANGE;
                            ev.ev_Value = (LONG)code;
                            dispatch(app, &ev);
                        }
                        break;
                    }
                }
            }
            break;

        default:
            break;
        }
    }
}

void AGUI_Poll(struct AGUIApp *app)
{
    if (app && app->a_Window)
        pump_messages(app);
}

void AGUI_Run(struct AGUIApp *app)
{
    ULONG winsig, timersig, sigs;
    struct AGEvent ev;

    if (!app || !app->a_Window)
        return;

    winsig   = 1UL << app->a_Window->UserPort->mp_SigBit;
    timersig = app->a_TimerOpen ? (1UL << app->a_TimerPort->mp_SigBit) : 0;

    if (app->a_TimerOpen)
        timer_start(app);

    memset(&ev, 0, sizeof(ev));
    ev.ev_Type = AGE_OPEN;
    dispatch(app, &ev);

    while (!app->a_Done)
    {
        sigs = Wait(winsig | timersig | SIGBREAKF_CTRL_C);

        if (sigs & SIGBREAKF_CTRL_C)
        {
            memset(&ev, 0, sizeof(ev));
            ev.ev_Type = AGE_CLOSE;
            dispatch(app, &ev);
            break;
        }

        if (timersig && (sigs & timersig))
        {
            while (GetMsg(app->a_TimerPort))
                ;
            app->a_TimerPending = FALSE;
            memset(&ev, 0, sizeof(ev));
            ev.ev_Type = AGE_TICK;
            dispatch(app, &ev);

            if (app->a_ShotCountdown && --app->a_ShotCountdown == 0)
            {
                AGUI_Snapshot(app, (CONST_STRPTR)agui_autoshot);
                agui_autoshot[0] = 0;   /* one shot: the first window only */
                AGUI_Quit(app);
            }
            if (!app->a_Done)
                timer_start(app);
        }

        if (sigs & winsig)
            pump_messages(app);
    }
}

/* ------------------------------------------------------------------ */
/* value access                                                        */
/* ------------------------------------------------------------------ */

STRPTR AGUI_GetString(struct AGUIApp *app, UWORD id)
{
    struct AGWidget *w = find_widget(app, id);

    if (!w)
        return (STRPTR)"";
    /* a password gadget only ever holds asterisks; the real text is ours */
    if (w->ag_Kind == AG_PASSWORD)
        return w->ag_Private ? (STRPTR)pw_real((struct AGPassword *)w->ag_Private)
                             : (STRPTR)"";
    if (w->ag_Gadget && w->ag_Gadget->SpecialInfo)
        return ((struct StringInfo *)w->ag_Gadget->SpecialInfo)->Buffer;
    if (w->ag_Private && w->ag_Kind == AG_STRING)
        return (STRPTR)w->ag_Private;
    return (STRPTR)"";
}

void AGUI_SetString(struct AGUIApp *app, UWORD id, CONST_STRPTR s)
{
    struct AGWidget *w = find_widget(app, id);

    if (w && w->ag_Kind == AG_PASSWORD && w->ag_Private)
    {
        struct AGPassword *p = (struct AGPassword *)w->ag_Private;

        memset(pw_real(p), 0, p->pw_Max + 1);
        strncpy(pw_real(p), s ? (char *)s : "", p->pw_Max);
        pw_hide(app, w);
        return;
    }

    if (!w || w->ag_Kind != AG_STRING)
        return;

    if (w->ag_Private)
    {
        ULONG max = w->ag_MaxChars ? w->ag_MaxChars : 128;
        strncpy((char *)w->ag_Private, s ? (char *)s : "", max);
        ((char *)w->ag_Private)[max] = '\0';
    }
    if (w->ag_Gadget)
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTST_String, (ULONG)(w->ag_Private ? (STRPTR)w->ag_Private
                                                             : (STRPTR)s),
                          TAG_END);
}

LONG AGUI_GetValue(struct AGUIApp *app, UWORD id)
{
    struct AGWidget *w = find_widget(app, id);

    if (!w)
        return 0;

    if (w->ag_Gadget)
    {
        switch (w->ag_Kind)
        {
        case AG_CHECKBOX:
            return (w->ag_Gadget->Flags & GFLG_SELECTED) ? 1 : 0;
        case AG_INTEGER:
            if (w->ag_Gadget->SpecialInfo)
                return ((struct StringInfo *)w->ag_Gadget->SpecialInfo)->LongInt;
            break;
        default:
            break;
        }
    }
    return w->ag_Value;
}

void AGUI_SetValue(struct AGUIApp *app, UWORD id, LONG value)
{
    struct AGWidget *w = find_widget(app, id);

    if (!w)
        return;
    w->ag_Value = value;
    if (!w->ag_Gadget)
        return;

    switch (w->ag_Kind)
    {
    case AG_CHECKBOX:
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTCB_Checked, (ULONG)(value ? TRUE : FALSE), TAG_END);
        break;
    case AG_CYCLE:
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTCY_Active, (ULONG)value, TAG_END);
        break;
    case AG_SLIDER:
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTSL_Level, (ULONG)value, TAG_END);
        break;
    case AG_INTEGER:
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTIN_Number, (ULONG)value, TAG_END);
        break;
    case AG_LISTVIEW:
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTLV_Selected, (ULONG)value, TAG_END);
        break;
    default:
        break;
    }
}

void AGUI_SetText(struct AGUIApp *app, UWORD id, CONST_STRPTR s)
{
    struct AGWidget *w = find_widget(app, id);

    if (!w)
        return;
    w->ag_Text = (STRPTR)s;
    if (w->ag_Gadget && w->ag_Kind == AG_TEXT)
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTTX_Text, (ULONG)s, TAG_END);
    else if (w->ag_Gadget && w->ag_Kind == AG_STRING)
        AGUI_SetString(app, id, s);
}

void AGUI_SetTextF(struct AGUIApp *app, UWORD id, CONST_STRPTR fmt, ...)
{
    static char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), (const char *)fmt, ap);
    va_end(ap);
    AGUI_SetText(app, id, (CONST_STRPTR)buf);
}

void AGUI_Disable(struct AGUIApp *app, UWORD id, BOOL disabled)
{
    struct AGWidget *w = find_widget(app, id);

    if (w && w->ag_Gadget)
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GA_Disabled, (ULONG)disabled, TAG_END);
}

/*
 * GadTools draws a button's title from the IntuiText it hung on the
 * gadget, and offers no tag to change it, so the text pointer is swapped
 * and the gadget redrawn. The caller keeps the string alive, and the
 * gadget box does not change size -- see ag_MaxChars.
 */
void AGUI_SetLabel(struct AGUIApp *app, UWORD id, CONST_STRPTR s)
{
    struct AGWidget *w = find_widget(app, id);
    struct IntuiText *it;

    if (!w || !s)
        return;
    w->ag_Label = (STRPTR)s;
    if (!w->ag_Gadget || w->ag_Kind != AG_BUTTON)
        return;

    it = (struct IntuiText *)w->ag_Gadget->GadgetText;
    if (!it)
        return;

    it->IText = (STRPTR)s;
    /* centre it again: GadTools placed the old text by hand */
    it->LeftEdge = (WORD)((w->ag_Gadget->Width -
                           text_width(app, s)) / 2);
    it->TopEdge  = (WORD)((w->ag_Gadget->Height - app->a_FontH) / 2);

    /* a button fills its own interior, so a shorter title leaves nothing
     * of the old one behind */
    RefreshGList(w->ag_Gadget, app->a_Window, NULL, 1);
}

/* ------------------------------------------------------------------ */
/* list views                                                          */
/* ------------------------------------------------------------------ */

/* current scroll position, or 0 where the release cannot report it */
static LONG list_top(struct AGUIApp *app, struct AGWidget *w)
{
    LONG top = 0;

    if (w->ag_Gadget)
        GT_GetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTLV_Top, (ULONG)&top, TAG_END);
    return top < 0 ? 0 : top;
}

/*
 * Reattach the label list after it has been changed. top < 0 means follow
 * the tail, which is what a log wants: always show the newest line. A list
 * the user picks from wants the opposite -- rewriting an entry must not
 * scroll the view out from under them -- so it passes the position it read
 * before the rebuild.
 */
static void relink_list(struct AGUIApp *app, struct AGWidget *w, LONG top)
{
    struct AGList *l = (struct AGList *)w->ag_Private;
    LONG count = 0;
    struct Node *n;

    if (!w->ag_Gadget)
        return;
    for (n = l->al_List.lh_Head; n->ln_Succ; n = n->ln_Succ)
        count++;

    if (top < 0 || top >= count)
        top = count > 0 ? count - 1 : 0;

    /* detach before touching the list, as the autodocs require */
    GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                      GTLV_Labels, (ULONG)~0, TAG_END);
    GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                      GTLV_Labels, (ULONG)&l->al_List,
                      GTLV_Top,    (ULONG)top,
                      TAG_END);
}

void AGUI_ClearList(struct AGUIApp *app, UWORD id)
{
    struct AGWidget *w = find_widget(app, id);

    if (!w || !w->ag_Private)
        return;
    if (w->ag_Gadget)
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTLV_Labels, (ULONG)~0, TAG_END);
    list_empty((struct AGList *)w->ag_Private);
    if (w->ag_Gadget)
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTLV_Labels,
                          (ULONG)&((struct AGList *)w->ag_Private)->al_List,
                          TAG_END);
}

void AGUI_AddItem(struct AGUIApp *app, UWORD id, CONST_STRPTR text)
{
    struct AGWidget *w = find_widget(app, id);

    if (!w || !w->ag_Private)
        return;
    if (w->ag_Gadget)
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTLV_Labels, (ULONG)~0, TAG_END);
    list_add((struct AGList *)w->ag_Private, text);
    relink_list(app, w, -1);           /* a log follows its newest line */
}

void AGUI_SetList(struct AGUIApp *app, UWORD id, STRPTR *items)
{
    struct AGWidget *w = find_widget(app, id);
    STRPTR *p;
    LONG top;

    if (!w || !w->ag_Private)
        return;
    top = list_top(app, w);            /* read before detaching */
    if (w->ag_Gadget)
        GT_SetGadgetAttrs(w->ag_Gadget, app->a_Window, NULL,
                          GTLV_Labels, (ULONG)~0, TAG_END);
    list_empty((struct AGList *)w->ag_Private);
    if (items)
        for (p = items; *p; p++)
            list_add((struct AGList *)w->ag_Private, *p);
    relink_list(app, w, top);
}

STRPTR AGUI_GetItem(struct AGUIApp *app, UWORD id, LONG index)
{
    struct AGWidget *w = find_widget(app, id);
    struct Node *n;
    LONG i = 0;

    if (!w || !w->ag_Private || index < 0)
        return NULL;
    for (n = ((struct AGList *)w->ag_Private)->al_List.lh_Head;
         n->ln_Succ; n = n->ln_Succ, i++)
        if (i == index)
            return n->ln_Name;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* requesters                                                          */
/* ------------------------------------------------------------------ */

void AGUI_Message(struct AGUIApp *app, CONST_STRPTR title, CONST_STRPTR body)
{
    struct EasyStruct es;

    es.es_StructSize   = sizeof(struct EasyStruct);
    es.es_Flags        = 0;
    es.es_Title        = (STRPTR)title;
    es.es_TextFormat   = (STRPTR)body;
    es.es_GadgetFormat = (STRPTR)"OK";

    EasyRequestArgs(app ? app->a_Window : NULL, &es, NULL, NULL);
}

BOOL AGUI_Ask(struct AGUIApp *app, CONST_STRPTR title, CONST_STRPTR body)
{
    struct EasyStruct es;

    es.es_StructSize   = sizeof(struct EasyStruct);
    es.es_Flags        = 0;
    es.es_Title        = (STRPTR)title;
    es.es_TextFormat   = (STRPTR)body;
    es.es_GadgetFormat = (STRPTR)"Yes|No";

    return (BOOL)(EasyRequestArgs(app ? app->a_Window : NULL, &es, NULL, NULL) == 1);
}

/*
 * asl.library is opened per request rather than held: a drawer requester is
 * a once-in-a-while thing, and this keeps it out of the startup path of
 * applications that never ask for one.
 */
BOOL AGUI_RequestDir(struct AGUIApp *app, CONST_STRPTR title,
                     char *path, LONG len)
{
    struct FileRequester *fr;
    BOOL ok = FALSE;

    if (!path || len < 2)
        return FALSE;

    /* v37 so this still works on 2.04; ASLFR_DrawersOnly arrived with v38
     * and older releases simply ignore it and show files as well */
    AslBase = OpenLibrary("asl.library", 37);
    if (!AslBase)
        return FALSE;

    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_Window,        (ULONG)(app ? app->a_Window : NULL),
            ASLFR_TitleText,     (ULONG)(title ? title
                                               : (CONST_STRPTR)"Select a drawer"),
            ASLFR_InitialDrawer, (ULONG)path,
            ASLFR_DrawersOnly,   TRUE,
            ASLFR_SleepWindow,   TRUE,
            TAG_END);

    if (fr)
    {
        if (AslRequest(fr, NULL) && fr->fr_Drawer)
        {
            strncpy(path, (char *)fr->fr_Drawer, len - 1);
            path[len - 1] = 0;
            ok = TRUE;
        }
        FreeAslRequest(fr);
    }

    CloseLibrary(AslBase);
    AslBase = NULL;
    return ok;
}
