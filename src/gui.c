/*
 * gui.c -- NextSync Workbench interface, built on agui.
 */

#include <exec/types.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "agui.h"
#include "nshttp.h"
#include "nsdav.h"
#include "nssync.h"
#include "nsconf.h"
#include "nsprefs.h"
#include "nstest.h"

#define ID_SERVER   1
#define ID_PAIRS    2
#define ID_STATUS   3
#define ID_LOG      4
#define ID_SYNC     5
#define ID_QUIT     6

#define MENU_ABOUT  101
#define MENU_SNAP   102
#define MENU_PREFS  103
#define MENU_QUIT   104

#define LABEL_SYNC   "Sync now"
#define LABEL_ABORT  "Stop sync"

struct gui_state
{
    nsconf     *conf;
    const char *conffile;
    BOOL        busy;
    BOOL        abort;     /* Stop sync was pressed                    */
    int         guitest;   /* 0 off, 1 sync pending, 2 snapshot pending */
};

static struct AGWidget widgets[] =
{
    { AG_TEXT,     ID_SERVER, "Server:",  0, NULL, 0, 0, 0, "-" },
    { AG_LISTVIEW, ID_PAIRS,  "Folders:", 0,            NULL, 0, 4 },
    { AG_TEXT,     ID_STATUS, "Status:",  0, NULL, 0, 0, 0, "idle" },
    { AG_LISTVIEW, ID_LOG,    "Log:",     AGF_GROW },
    /* ag_MaxChars reserves room so the title can change to LABEL_ABORT
     * without the button having to be re-laid-out under the user */
    { AG_BUTTON,   ID_SYNC,   LABEL_SYNC, 0, NULL, 0, 0, 0, NULL,
      sizeof(LABEL_ABORT) },
    { AG_BUTTON,   ID_QUIT,   "Quit",     AGF_SAMEROW },
    { AG_END }
};

static struct NewMenu menu[] =
{
    { NM_TITLE, "Project",       NULL, 0, 0, NULL },
    { NM_ITEM,  "About...",      "?",  0, 0, (APTR)MENU_ABOUT },
    { NM_ITEM,  "Preferences...","P",  0, 0, (APTR)MENU_PREFS },
    { NM_ITEM,  "Snapshot",      "S",  0, 0, (APTR)MENU_SNAP },
    { NM_ITEM,  NM_BARLABEL,     NULL, 0, 0, NULL },
    { NM_ITEM,  "Quit",          "Q",  0, 0, (APTR)MENU_QUIT },
    { NM_END,   NULL,            NULL, 0, 0, NULL }
};

/* sync callbacks push into the log list */
static void gui_log(void *user, const char *fmt, ...)
{
    struct AGUIApp *app = (struct AGUIApp *)user;
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    AGUI_AddItem(app, ID_LOG, (CONST_STRPTR)buf);
}

/*
 * Called between files. The sync runs inside the event handler, so the
 * window is not reading its own messages -- pumping them here is what
 * keeps it redrawing and what lets Stop sync be clicked at all.
 * Returning FALSE stops the run after the current file.
 */
static BOOL gui_file(void *user, const char *action, const char *path,
                     ULONG idx, ULONG total)
{
    struct AGUIApp *app = (struct AGUIApp *)user;
    struct gui_state *st = (struct gui_state *)AGUI_UserData(app);

    AGUI_SetTextF(app, ID_STATUS, "%s %s (%lu/%lu)",
                  action, path, (unsigned long)idx, (unsigned long)total);

    /* ABORTTEST: click Stop sync for real, from inside the sync, so the
     * event has to make the whole trip out to Intuition and back into a
     * handler that is already running */
    /*
     * ABORTTEST: press ESC for real, from inside the sync, so the event
     * has to travel out to Intuition and back into a handler that is
     * already running. Clicking Stop sync takes the same road from
     * AGUI_Poll() onwards; a synthetic mouse click is the one thing that
     * does not survive the emulator's input layer, so the button itself
     * is checked by hand.
     */
    if (st->guitest == 4 && idx == 2)
    {
        printf("abort: pressing ESC during file %lu\n", (unsigned long)idx);
        fflush(stdout);
        nstest_key(0x45);
    }

    AGUI_Poll(app);

    if (st->guitest == 4 && idx == 2)
    {
        printf("abort: after poll, abort=%ld\n", (long)st->abort);
        fflush(stdout);
    }
    return (BOOL)!st->abort;
}

/* the same, during one long transfer: keeps the window alive and lets a
 * single enormous file be given up on */
static BOOL gui_progress(void *user, const char *phase, ULONG done,
                         ULONG total)
{
    struct AGUIApp *app = (struct AGUIApp *)user;
    struct gui_state *st = (struct gui_state *)AGUI_UserData(app);

    (void)phase;
    (void)done;
    (void)total;
    AGUI_Poll(app);
    return (BOOL)!st->abort;
}

/* redraw everything that comes out of the configuration */
static void refresh_config(struct AGUIApp *app)
{
    struct gui_state *st = (struct gui_state *)AGUI_UserData(app);
    nsconf *c = st->conf;
    static char server[192];         /* the text gadget keeps the pointer */
    static char row[NSCONF_MAXPAIRS][160];
    static STRPTR rowp[NSCONF_MAXPAIRS + 1];
    UWORD i, n = 0;

    if (c->server[0])
        sprintf(server, "%s:%ld (%s)", c->server, (long)c->port, c->user);
    else
        strcpy(server, "not set up yet -- see Project/Preferences");
    AGUI_SetText(app, ID_SERVER, (CONST_STRPTR)server);

    /* SetList rather than AddItem: this list is read from the top, it is
     * not a log that should follow its newest line */
    for (i = 0; i < c->npairs && n < NSCONF_MAXPAIRS; i++, n++)
    {
        sprintf(row[n], "%s -> %s", c->pair[i].remote, c->pair[i].local);
        rowp[n] = (STRPTR)row[n];
    }
    if (!n)
    {
        strcpy(row[0], "no folders selected");
        rowp[n++] = (STRPTR)row[0];
    }
    rowp[n] = NULL;
    AGUI_SetList(app, ID_PAIRS, rowp);

    AGUI_Disable(app, ID_SYNC, (BOOL)!nsconf_complete(c));
}

static void run_sync(struct AGUIApp *app)
{
    struct gui_state *st = (struct gui_state *)AGUI_UserData(app);
    nsconf *conf = st->conf;
    char err[256];
    nshttp *h;
    nsdav dav;
    UWORD i;

    if (st->busy)
        return;
    st->busy  = TRUE;
    st->abort = FALSE;
    AGUI_SetLabel(app, ID_SYNC, (CONST_STRPTR)LABEL_ABORT);

    AGUI_SetText(app, ID_STATUS, "connecting...");
    h = nshttp_open(conf->server, conf->port, conf->port == 443,
                    conf->user, conf->pass, err);
    if (!h)
    {
        AGUI_SetText(app, ID_STATUS, "connection failed");
        gui_log(app, "%s", err);
    }
    else
    {
        nsdav_init(&dav, h, conf->user);
        nshttp_set_progress(h, gui_progress, app);

        for (i = 0; i < conf->npairs && !st->abort; i++)
        {
            nssync_stats stats;

            gui_log(app, "sync %s <-> %s",
                    conf->pair[i].remote, conf->pair[i].local);
            if (nssync_run(&dav, conf->pair[i].remote, conf->pair[i].local,
                           gui_log, gui_file, app, &stats))
                gui_log(app, "done: %lu down, %lu up, %lu del, "
                        "%lu conflicts, %lu failed",
                        (unsigned long)stats.downloaded,
                        (unsigned long)stats.uploaded,
                        (unsigned long)(stats.deleted_local +
                                        stats.deleted_remote),
                        (unsigned long)stats.conflicts,
                        (unsigned long)stats.failed);
            else
                gui_log(app, "sync failed");
        }
        nshttp_set_progress(h, NULL, NULL);
        nshttp_close(h);
        AGUI_SetText(app, ID_STATUS, st->abort ? "stopped" : "idle");
    }

    if (st->abort)
        gui_log(app, "stopped. Files already transferred are kept.");

    AGUI_SetLabel(app, ID_SYNC, (CONST_STRPTR)LABEL_SYNC);
    st->busy  = FALSE;
    st->abort = FALSE;
}

static void handler(struct AGUIApp *app, struct AGEvent *ev)
{
    struct gui_state *st = (struct gui_state *)AGUI_UserData(app);

    switch (ev->ev_Type)
    {
    case AGE_OPEN:
        refresh_config(app);
        if (nsconf_complete(st->conf))
            gui_log(app, "ready.");
        else
            gui_log(app, "not configured: open Project/Preferences.");
        break;

    case AGE_TICK:
        /* GUITEST: one automated sync, one snapshot, then quit.
         * ABORTTEST (4) is the same but stops itself part way. */
        if (st->guitest == 1 || st->guitest == 4)
        {
            int mode = st->guitest;
            run_sync(app);
            st->guitest = (mode == 4) ? 5 : 2;
        }
        else if (st->guitest == 5)
        {
            printf("abort: sync returned, window still alive\n");
            fflush(stdout);
            AGUI_Snapshot(app, "out/nextsync.ags");
            AGUI_Quit(app);
        }
        /* NESTTEST: open Preferences from inside this window's own event
         * loop, which is the case the modal dialog has to survive */
        else if (st->guitest == 3)
        {
            BOOL saved = nsprefs_show_ex(app, st->conf, st->conffile, 1);

            printf("nest: preferences returned %s\n", saved ? "saved" : "cancelled");
            fflush(stdout);
            refresh_config(app);
            gui_log(app, "back from preferences.");
            st->guitest = 2;
        }
        else if (st->guitest == 2)
        {
            AGUI_Snapshot(app, "out/nextsync.ags");
            AGUI_Quit(app);
        }
        break;

    case AGE_KEY:
        /* ESC stops a running sync, the way ESC cancels anything else */
        if (ev->ev_Code == 0x1B && st->busy)
        {
            st->abort = TRUE;
            AGUI_SetText(app, ID_STATUS, "stopping after this file...");
        }
        break;

    case AGE_CLOSE:
        /* mid-sync the close gadget stops the sync; close again to leave */
        if (st->busy)
        {
            st->abort = TRUE;
            AGUI_SetText(app, ID_STATUS, "stopping after this file...");
        }
        else
            AGUI_Quit(app);
        break;

    case AGE_CLICK:
        if (ev->ev_ID == ID_SYNC)
        {
            /* the same button: start when idle, stop when running. This
             * arrives from AGUI_Poll() inside the sync, so the handler is
             * re-entered here -- set the flag and unwind, do not sync */
            if (st->busy)
            {
                st->abort = TRUE;
                AGUI_SetText(app, ID_STATUS, "stopping after this file...");
            }
            else
                run_sync(app);
        }
        else if (ev->ev_ID == ID_QUIT && !st->busy)
            AGUI_Quit(app);
        break;

    case AGE_MENU:
        switch (ev->ev_Code)
        {
        case MENU_ABOUT:
            AGUI_Message(app, "About NextSync",
                         "NextSync 1.1\n\n"
                         "Nextcloud folder synchronisation\n"
                         "for AmigaOS 3.1 and later.\n\n"
                         "WebDAV over TLS via AmiSSL v5.");
            break;
        case MENU_PREFS:
            if (st->busy)
                break;
            if (nsprefs_show(app, st->conf, st->conffile))
            {
                refresh_config(app);
                gui_log(app, "preferences saved.");
            }
            break;
        case MENU_SNAP:
            AGUI_Snapshot(app, "out/nextsync.ags");
            break;
        case MENU_QUIT:
            if (!st->busy)
                AGUI_Quit(app);
            break;
        }
        break;
    }
}

int nextsync_gui_ex(nsconf *conf, const char *conffile, int guitest)
{
    struct AGSpec spec;
    struct gui_state st;
    struct AGUIApp *app;

    memset(&st, 0, sizeof(st));
    st.conf     = conf;
    st.conffile = conffile;
    st.guitest  = guitest;

    memset(&spec, 0, sizeof(spec));
    spec.ag_Title    = "NextSync";
    spec.ag_Widgets  = widgets;
    spec.ag_Menu     = menu;
    spec.ag_Handler  = handler;
    spec.ag_UserData = &st;
    spec.ag_MinCols  = 68;
    if (guitest)
        spec.ag_Ticks = 10;

    app = AGUI_Open(&spec);
    if (!app)
        return 20;
    AGUI_Run(app);
    AGUI_Close(app);
    return 0;
}

int nextsync_gui(nsconf *conf, const char *conffile)
{
    return nextsync_gui_ex(conf, conffile, 0);
}
