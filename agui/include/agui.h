/*
 * agui.h -- a small Intuition/GadTools application framework for AmigaOS 3.1+
 *
 * Design goals:
 *   - zero runtime dependencies: everything used lives in Kickstart 3.1 ROM
 *     (intuition.library, gadtools.library, graphics.library v39+)
 *   - font sensitive: all layout is derived from the public screen's font
 *   - declarative: describe widgets in a table, get a laid out, resizable
 *     window with a working event loop
 *
 * The application describes its window with an AGSpec, opens it with
 * AGUI_Open(), and calls AGUI_Run() to enter the event loop. Events are
 * delivered to a single handler callback.
 */

#ifndef AGUI_H
#define AGUI_H

#include <exec/types.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>

/* ------------------------------------------------------------------ */
/* widget kinds                                                        */
/* ------------------------------------------------------------------ */

#define AG_END       0   /* terminates the widget table                */
#define AG_BUTTON    1   /* push button, label drawn inside            */
#define AG_TEXT      2   /* read only text display                     */
#define AG_STRING    3   /* editable string                            */
#define AG_INTEGER   4   /* editable number                            */
#define AG_CHECKBOX  5   /* boolean toggle                             */
#define AG_CYCLE     6   /* cycle through ag_Labels                    */
#define AG_SLIDER    7   /* horizontal slider, ag_Min..ag_Max          */
#define AG_LISTVIEW  8   /* scrolling list, filled with AGUI_SetList() */
#define AG_SPACE     9   /* blank filler, useful to push things around */
#define AG_PASSWORD  10  /* like AG_STRING, shown as asterisks          */

/* ------------------------------------------------------------------ */
/* widget flags                                                        */
/* ------------------------------------------------------------------ */

#define AGF_SAMEROW  (1<<0) /* place next to the previous widget       */
#define AGF_NOWEIGHT (1<<1) /* keep natural width, do not stretch      */
#define AGF_GROW     (1<<2) /* absorb spare vertical space             */
#define AGF_DISABLED (1<<3) /* start out ghosted                       */

/* ------------------------------------------------------------------ */
/* widget description                                                  */
/* ------------------------------------------------------------------ */

struct AGWidget
{
    UWORD   ag_Kind;        /* AG_xxx                                  */
    UWORD   ag_ID;          /* application supplied id, 1..n, 0 = none */
    STRPTR  ag_Label;       /* button text, or label to the left       */
    UWORD   ag_Flags;       /* AGF_xxx                                 */

    STRPTR *ag_Labels;      /* AG_CYCLE: NULL terminated choices       */
    LONG    ag_Min;         /* AG_SLIDER                               */
    LONG    ag_Max;         /* AG_SLIDER; AG_LISTVIEW: visible rows    */
    LONG    ag_Value;       /* initial value / checked / active        */
    STRPTR  ag_Text;        /* AG_TEXT initial contents                */
    ULONG   ag_MaxChars;    /* AG_STRING/AG_PASSWORD buffer size, 128; */
                            /* AG_BUTTON: width to reserve, in chars   */

    /* -- filled in by the framework, do not touch -- */
    struct Gadget *ag_Gadget;
    APTR    ag_Private;
};

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

#define AGE_CLICK   1   /* button pressed                              */
#define AGE_CHANGE  2   /* value of a gadget changed                   */
#define AGE_MENU    3   /* menu item picked, ev_Code = user data       */
#define AGE_CLOSE   4   /* close gadget, or the app asked to quit      */
#define AGE_KEY     5   /* raw key, ev_Code = vanilla key              */
#define AGE_OPEN    6   /* sent once, after the window is on screen    */
#define AGE_TICK    7   /* periodic, if ag_Ticks was requested         */

struct AGEvent
{
    UWORD   ev_Type;        /* AGE_xxx                                 */
    UWORD   ev_ID;          /* widget id, for CLICK / CHANGE           */
    LONG    ev_Value;       /* new value: level, checked, active, ...  */
    ULONG   ev_Code;        /* raw IntuiMessage Code / menu user data  */
    ULONG   ev_Qualifier;
};

struct AGUIApp;

typedef void (*AGHandlerFunc)(struct AGUIApp *app, struct AGEvent *ev);

/* ------------------------------------------------------------------ */
/* application description                                             */
/* ------------------------------------------------------------------ */

struct AGSpec
{
    STRPTR             ag_Title;      /* window title                  */
    STRPTR             ag_ScreenTitle;/* optional screen title         */
    struct AGWidget   *ag_Widgets;    /* AG_END terminated table       */
    struct NewMenu    *ag_Menu;       /* optional GadTools menu tree   */
    AGHandlerFunc      ag_Handler;    /* event callback                */
    APTR               ag_UserData;   /* yours, see AGUI_UserData()    */
    UWORD              ag_MinCols;    /* width hint, in characters     */
    UWORD              ag_Ticks;      /* tick interval, 1/10 s, 0=off  */
};

/* ------------------------------------------------------------------ */
/* api                                                                 */
/* ------------------------------------------------------------------ */

/*
 * A modal dialog is just a second application opened from inside the first
 * one's handler: AGUI_Open() a window with its own widget table, AGUI_Run()
 * it, AGUI_Close() it, and the outer window's loop picks up where it left
 * off. The outer window stays on screen but does not respond while the
 * inner loop runs, which is what "modal" means here. Each window needs its
 * own widget table -- the framework keeps per widget state in it.
 */
struct AGUIApp *AGUI_Open(struct AGSpec *spec);
void            AGUI_Run(struct AGUIApp *app);
void            AGUI_Close(struct AGUIApp *app);
void            AGUI_Quit(struct AGUIApp *app);

APTR            AGUI_UserData(struct AGUIApp *app);
struct Window  *AGUI_Window(struct AGUIApp *app);

/*
 * The Intuition gadget behind a widget, for the occasions the framework
 * has no wrapper for -- ActivateGadget(), say. NULL between a resize and
 * the rebuild that follows it, so check it every time rather than keeping
 * the pointer.
 */
struct Gadget  *AGUI_Gadget(struct AGUIApp *app, UWORD id);

/*
 * Runs any events already waiting and returns immediately, unlike
 * AGUI_Run() which waits for them. Call it from inside a long job so the
 * window still redraws and its buttons still work -- that is how a job
 * started from the handler can offer a way to stop it. The handler has to
 * expect to be called while it is already running.
 */
void            AGUI_Poll(struct AGUIApp *app);

/* value access, all by widget id */
STRPTR          AGUI_GetString(struct AGUIApp *app, UWORD id);
void            AGUI_SetString(struct AGUIApp *app, UWORD id, CONST_STRPTR s);
LONG            AGUI_GetValue(struct AGUIApp *app, UWORD id);
void            AGUI_SetValue(struct AGUIApp *app, UWORD id, LONG value);
void            AGUI_SetText(struct AGUIApp *app, UWORD id, CONST_STRPTR s);
void            AGUI_SetTextF(struct AGUIApp *app, UWORD id, CONST_STRPTR fmt, ...);
void            AGUI_Disable(struct AGUIApp *app, UWORD id, BOOL disabled);

/*
 * Retitles a button in place. The gadget is not re-laid-out, so reserve
 * room for the longest text it will carry with ag_MaxChars.
 */
void            AGUI_SetLabel(struct AGUIApp *app, UWORD id, CONST_STRPTR s);

/* list views */
void            AGUI_SetList(struct AGUIApp *app, UWORD id, STRPTR *items);
void            AGUI_ClearList(struct AGUIApp *app, UWORD id);
void            AGUI_AddItem(struct AGUIApp *app, UWORD id, CONST_STRPTR text);
STRPTR          AGUI_GetItem(struct AGUIApp *app, UWORD id, LONG index);

/* requesters */
void            AGUI_Message(struct AGUIApp *app, CONST_STRPTR title,
                             CONST_STRPTR body);
BOOL            AGUI_Ask(struct AGUIApp *app, CONST_STRPTR title,
                         CONST_STRPTR body);

/*
 * ASL drawer requester. path is both the drawer it starts in and, on TRUE,
 * where the chosen drawer is written back. Returns FALSE if the user
 * cancelled or asl.library is unavailable, leaving path untouched.
 */
BOOL            AGUI_RequestDir(struct AGUIApp *app, CONST_STRPTR title,
                                char *path, LONG len);

/* diagnostics: appends a line to a file, works when started from Workbench */
void            AGUI_LogTo(CONST_STRPTR filename);
void            AGUI_Log(CONST_STRPTR fmt, ...);

/*
 * Dumps the whole public screen to an .ags file (see tools/ags2png.py).
 * This is how the host side build system takes screenshots of a running
 * application without needing any support from the emulator.
 */
BOOL            AGUI_Snapshot(struct AGUIApp *app, CONST_STRPTR filename);

/*
 * Arms a one shot: once the window has been up long enough to have drawn
 * itself, the framework saves a snapshot and quits. Call it before
 * AGUI_Open(). This is what tools/shot.sh drives, so any application can be
 * screenshotted from the host without writing test code for it:
 *
 *     if (argc > 1 && strcmp(argv[1], "SNAPSHOT") == 0)
 *         AGUI_AutoSnapshot("out/shot.ags");
 */
void            AGUI_AutoSnapshot(CONST_STRPTR filename);

#endif /* AGUI_H */
