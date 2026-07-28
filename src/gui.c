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

struct gui_state
{
    nsconf     *conf;
    const char *conffile;
    BOOL        busy;
    int         guitest;   /* 0 off, 1 sync pending, 2 snapshot pending */
};

static struct AGWidget widgets[] =
{
    { AG_TEXT,     ID_SERVER, "Server:",  0, NULL, 0, 0, 0, "-" },
    { AG_LISTVIEW, ID_PAIRS,  "Folders:", 0,            NULL, 0, 4 },
    { AG_TEXT,     ID_STATUS, "Status:",  0, NULL, 0, 0, 0, "idle" },
    { AG_LISTVIEW, ID_LOG,    "Log:",     AGF_GROW },
    { AG_BUTTON,   ID_SYNC,   "Sync now", 0 },
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

static BOOL gui_file(void *user, const char *action, const char *path,
                     ULONG idx, ULONG total)
{
    struct AGUIApp *app = (struct AGUIApp *)user;

    AGUI_SetTextF(app, ID_STATUS, "%s %s (%lu/%lu)",
                  action, path, (unsigned long)idx, (unsigned long)total);
    return TRUE;
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
    st->busy = TRUE;
    AGUI_Disable(app, ID_SYNC, TRUE);

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

        for (i = 0; i < conf->npairs; i++)
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
        nshttp_close(h);
        AGUI_SetText(app, ID_STATUS, "idle");
    }

    AGUI_Disable(app, ID_SYNC, FALSE);
    st->busy = FALSE;
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
        /* GUITEST: one automated sync, one snapshot, then quit */
        if (st->guitest == 1)
        {
            st->guitest = 2;
            run_sync(app);
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

    case AGE_CLOSE:
        if (!st->busy)
            AGUI_Quit(app);
        break;

    case AGE_CLICK:
        if (ev->ev_ID == ID_SYNC)
            run_sync(app);
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
