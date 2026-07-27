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

#define ID_SERVER   1
#define ID_PAIRS    2
#define ID_STATUS   3
#define ID_LOG      4
#define ID_SYNC     5
#define ID_QUIT     6

#define MENU_ABOUT  101
#define MENU_SNAP   102
#define MENU_QUIT   103

struct gui_state
{
    nsconf  *conf;
    BOOL     busy;
    int      guitest;      /* 0 off, 1 sync pending, 2 snapshot pending */
};

static struct AGWidget widgets[] =
{
    { AG_TEXT,     ID_SERVER, "Server:",  0, NULL, 0, 0, 0, "-" },
    { AG_LISTVIEW, ID_PAIRS,  "Folders:", AGF_NOWEIGHT },
    { AG_TEXT,     ID_STATUS, "Status:",  0, NULL, 0, 0, 0, "idle" },
    { AG_LISTVIEW, ID_LOG,    "Log:",     AGF_GROW },
    { AG_BUTTON,   ID_SYNC,   "Sync now", 0 },
    { AG_BUTTON,   ID_QUIT,   "Quit",     AGF_SAMEROW },
    { AG_END }
};

static struct NewMenu menu[] =
{
    { NM_TITLE, "Project",    NULL, 0, 0, NULL },
    { NM_ITEM,  "About...",   "?",  0, 0, (APTR)MENU_ABOUT },
    { NM_ITEM,  "Snapshot",   "S",  0, 0, (APTR)MENU_SNAP },
    { NM_ITEM,  NM_BARLABEL,  NULL, 0, 0, NULL },
    { NM_ITEM,  "Quit",       "Q",  0, 0, (APTR)MENU_QUIT },
    { NM_END,   NULL,         NULL, 0, 0, NULL }
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
        {
            char buf[192];
            UWORD i;

            sprintf(buf, "%s:%ld (%s)", st->conf->server,
                    (long)st->conf->port, st->conf->user);
            AGUI_SetText(app, ID_SERVER, buf);
            for (i = 0; i < st->conf->npairs; i++)
            {
                sprintf(buf, "%s -> %s", st->conf->pair[i].remote,
                        st->conf->pair[i].local);
                AGUI_AddItem(app, ID_PAIRS, (CONST_STRPTR)buf);
            }
            gui_log(app, "ready.");
        }
        break;

    case AGE_TICK:
        /* GUITEST mode: one automated sync, one snapshot, then quit */
        if (st->guitest == 1)
        {
            st->guitest = 2;
            run_sync(app);
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
                         "NextSync 1.0\n\n"
                         "Nextcloud folder synchronisation\n"
                         "for AmigaOS 3.1 and later.\n\n"
                         "WebDAV over TLS via AmiSSL v5.");
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

int nextsync_gui_ex(nsconf *conf, int guitest)
{
    struct AGSpec spec;
    struct gui_state st;
    struct AGUIApp *app;

    memset(&st, 0, sizeof(st));
    st.conf = conf;
    st.guitest = guitest;

    memset(&spec, 0, sizeof(spec));
    spec.ag_Title    = "NextSync";
    spec.ag_Widgets  = widgets;
    spec.ag_Menu     = menu;
    spec.ag_Handler  = handler;
    spec.ag_UserData = &st;
    spec.ag_MinCols  = 52;
    if (guitest)
        spec.ag_Ticks = 10;

    app = AGUI_Open(&spec);
    if (!app)
        return 20;
    AGUI_Run(app);
    AGUI_Close(app);
    return 0;
}

int nextsync_gui(nsconf *conf)
{
    return nextsync_gui_ex(conf, 0);
}
