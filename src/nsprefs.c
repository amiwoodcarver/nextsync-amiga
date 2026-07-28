/*
 * nsprefs.c -- the Preferences dialog: server details, a live listing of
 * the folders on the account, and where they should land locally.
 *
 * Connecting is deliberately an explicit button rather than something that
 * happens while you type: on a 68020 with TLS a connection costs a few
 * seconds, and the GUI is single threaded.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#include "agui.h"
#include "nshttp.h"
#include "nsdav.h"
#include "nsconf.h"
#include "nsprefs.h"

#define ID_SERVER   1
#define ID_CONNECT  2
#define ID_USER     3
#define ID_PASS     4
#define ID_PORT     5
#define ID_STATUS   6
#define ID_FOLDERS  7
#define ID_LOCAL    8
#define ID_BROWSE   9
#define ID_SAVE     10
#define ID_CANCEL   11

#define MAXLIST     64
#define MAXROWS     (MAXLIST + NSCONF_MAXEXTRA)
#define ROWLEN      140

struct prefs_state
{
    nsconf     *conf;
    nsconf      backup;                  /* what Cancel goes back to     */
    const char *conffile;
    BOOL        saved;
    BOOL        connected;

    int         test;                    /* PREFSTEST: drive it from code*/
    int         step;

    UWORD       nnames;                  /* toggleable entries, first    */
    char        name[MAXLIST][128];      /* folder names on the server   */
    char        row[MAXROWS][ROWLEN];    /* how they are shown, "[x] ..."*/
    STRPTR      rowp[MAXROWS + 1];
};

static struct AGWidget widgets[] =
{
    { AG_STRING,   ID_SERVER,  "Server:",   0,            NULL, 0, 0, 0, NULL, 127 },
    { AG_BUTTON,   ID_CONNECT, "Connect",   AGF_SAMEROW },
    { AG_STRING,   ID_USER,    "User:",     0,            NULL, 0, 0, 0, NULL, 63 },
    { AG_STRING,   ID_PASS,    "Password:", 0,            NULL, 0, 0, 0, NULL, 127 },
    { AG_INTEGER,  ID_PORT,    "Port:",     AGF_NOWEIGHT, NULL, 0, 0, 443 },
    { AG_TEXT,     ID_STATUS,  NULL,        0,            NULL, 0, 0, 0,
      (STRPTR)"Enter your server details, then Connect." },
    { AG_LISTVIEW, ID_FOLDERS, "Folders:",  AGF_GROW,     NULL, 0, 7 },
    { AG_STRING,   ID_LOCAL,   "Store in:", 0,            NULL, 0, 0, 0, NULL, 255 },
    { AG_BUTTON,   ID_BROWSE,  "Browse...", AGF_SAMEROW },
    { AG_BUTTON,   ID_SAVE,    "Save",      0 },
    { AG_BUTTON,   ID_CANCEL,  "Cancel",    AGF_SAMEROW },
    { AG_END }
};

/* ------------------------------------------------------------------ */
/* the folder list                                                     */
/* ------------------------------------------------------------------ */

/* server paths are relative to the account root; at depth 1 that is just
 * a name, but never trust it */
static const char *leaf(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

static BOOL have_name(struct prefs_state *ps, const char *name)
{
    UWORD i;

    for (i = 0; i < ps->nnames; i++)
        if (!strcmp(ps->name[i], name))
            return TRUE;
    return FALSE;
}

static void add_name(struct prefs_state *ps, const char *name)
{
    if (!name[0] || ps->nnames >= MAXLIST || have_name(ps, name))
        return;
    strncpy(ps->name[ps->nnames], name, sizeof(ps->name[0]) - 1);
    ps->name[ps->nnames][sizeof(ps->name[0]) - 1] = 0;
    ps->nnames++;
}

static void refresh_list(struct AGUIApp *app)
{
    struct prefs_state *ps = (struct prefs_state *)AGUI_UserData(app);
    UWORD i, n = 0;

    for (i = 0; i < ps->nnames; i++, n++)
    {
        sprintf(ps->row[n], "%s %s",
                nsconf_has_folder(ps->conf, ps->name[i]) ? "[x]" : "[ ]",
                ps->name[i]);
        ps->rowp[n] = (STRPTR)ps->row[n];
    }

    /* pairs written by hand are synced too, so show them -- they are not
     * a folder plus a base drawer, so they are not toggleable here */
    for (i = 0; i < ps->conf->nextra && n < MAXROWS; i++, n++)
    {
        sprintf(ps->row[n], "--> %s in %s (pair line)",
                ps->conf->extra[i].remote, ps->conf->extra[i].local);
        ps->rowp[n] = (STRPTR)ps->row[n];
    }

    ps->rowp[n] = NULL;
    AGUI_SetList(app, ID_FOLDERS, ps->rowp);
}

static void toggle_folder(struct AGUIApp *app, LONG index)
{
    struct prefs_state *ps = (struct prefs_state *)AGUI_UserData(app);

    if (index < 0 || index >= (LONG)ps->nnames)
        return;

    if (nsconf_has_folder(ps->conf, ps->name[index]))
        nsconf_del_folder(ps->conf, ps->name[index]);
    else
    {
        nsconf_add_folder(ps->conf, ps->name[index]);
        if (!nsconf_has_folder(ps->conf, ps->name[index]))
        {
            AGUI_Message(app, "Too many folders",
                         "NextSync syncs at most 16 folders.\n"
                         "Deselect one first.");
            return;
        }
    }
    refresh_list(app);
}

/* ------------------------------------------------------------------ */
/* fields                                                              */
/* ------------------------------------------------------------------ */

static void copy_field(struct AGUIApp *app, UWORD id, char *dst, LONG max)
{
    STRPTR s = AGUI_GetString(app, id);

    strncpy(dst, s ? (char *)s : "", max - 1);
    dst[max - 1] = 0;
}

static void read_fields(struct AGUIApp *app)
{
    struct prefs_state *ps = (struct prefs_state *)AGUI_UserData(app);
    nsconf *c = ps->conf;
    LONG port;

    copy_field(app, ID_SERVER, c->server, sizeof(c->server));
    copy_field(app, ID_USER,   c->user,   sizeof(c->user));
    copy_field(app, ID_PASS,   c->pass,   sizeof(c->pass));
    copy_field(app, ID_LOCAL,  c->local,  sizeof(c->local));

    port = AGUI_GetValue(app, ID_PORT);
    c->port = (UWORD)((port > 0 && port < 65536) ? port : 443);

    nsconf_rebuild(c);
}

static void show_fields(struct AGUIApp *app)
{
    struct prefs_state *ps = (struct prefs_state *)AGUI_UserData(app);
    nsconf *c = ps->conf;

    AGUI_SetString(app, ID_SERVER, (CONST_STRPTR)c->server);
    AGUI_SetString(app, ID_USER,   (CONST_STRPTR)c->user);
    AGUI_SetString(app, ID_PASS,   (CONST_STRPTR)c->pass);
    AGUI_SetString(app, ID_LOCAL,  (CONST_STRPTR)c->local);
    AGUI_SetValue(app, ID_PORT, c->port);
}

/* ------------------------------------------------------------------ */
/* connecting                                                          */
/* ------------------------------------------------------------------ */

static void do_connect(struct AGUIApp *app)
{
    struct prefs_state *ps = (struct prefs_state *)AGUI_UserData(app);
    nsconf *c = ps->conf;
    char err[256];
    nshttp *h;
    nsdav dav;
    nsdav_entry *list, *e;
    BOOL ok = FALSE;
    UWORD i;

    read_fields(app);

    if (!c->server[0] || !c->user[0])
    {
        AGUI_SetText(app, ID_STATUS, "server and user are both required");
        AGUI_Message(app, "Preferences",
                     "Fill in the server address and your user name first.");
        return;
    }

    AGUI_SetTextF(app, ID_STATUS, "connecting to %s...", c->server);
    AGUI_Disable(app, ID_CONNECT, TRUE);
    AGUI_Disable(app, ID_SAVE, TRUE);

    h = nshttp_open(c->server, c->port, (BOOL)(c->port == 443),
                    c->user, c->pass, err);
    if (!h)
    {
        AGUI_SetTextF(app, ID_STATUS, "not connected: %s", err);
        AGUI_Message(app, "Connection failed", (CONST_STRPTR)err);
    }
    else
    {
        nsdav_init(&dav, h, c->user);
        list = nsdav_list(&dav, "", &ok);

        if (!ok)
        {
            AGUI_SetTextF(app, ID_STATUS, "not connected: %s", dav.err);
            AGUI_Message(app, "Connection failed", (CONST_STRPTR)dav.err);
        }
        else
        {
            ps->nnames = 0;
            for (e = list; e; e = e->next)
                if (e->is_dir)
                    add_name(ps, leaf(e->path));
            nsdav_free_list(list);

            /* anything already configured that the server did not list
             * stays visible, so Save cannot silently drop it */
            for (i = 0; i < c->nfolders; i++)
                add_name(ps, c->folder[i]);

            ps->connected = TRUE;
            AGUI_SetTextF(app, ID_STATUS,
                          "connected. %ld folders, click to select.",
                          (long)ps->nnames);
            refresh_list(app);
        }
        nshttp_close(h);
    }

    AGUI_Disable(app, ID_CONNECT, FALSE);
    AGUI_Disable(app, ID_SAVE, FALSE);
}

/* ------------------------------------------------------------------ */
/* saving                                                              */
/* ------------------------------------------------------------------ */

static void do_save(struct AGUIApp *app)
{
    struct prefs_state *ps = (struct prefs_state *)AGUI_UserData(app);
    nsconf *c = ps->conf;

    read_fields(app);

    if (!c->server[0] || !c->user[0])
    {
        AGUI_Message(app, "Preferences",
                     "The server address and user name cannot be empty.");
        return;
    }
    if (!c->local[0])
    {
        AGUI_Message(app, "Preferences",
                     "Choose a drawer for the synced folders,\n"
                     "with Browse or by typing a path.");
        return;
    }
    if (c->npairs == 0 &&
        !AGUI_Ask(app, "Preferences",
                  "No folders are selected, so nothing\n"
                  "will be synchronised. Save anyway?"))
        return;

    if (!nsconf_save(c, ps->conffile))
    {
        AGUI_Message(app, "Preferences",
                     "Could not write the configuration file.\n"
                     "Is the drawer NextSync lives in writable?");
        return;
    }

    ps->saved = TRUE;
    AGUI_Quit(app);
}

static void do_browse(struct AGUIApp *app)
{
    char path[256];
    STRPTR cur = AGUI_GetString(app, ID_LOCAL);

    strncpy(path, cur ? (char *)cur : "", sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;

    if (AGUI_RequestDir(app, "Where should the synced folders go?",
                        path, sizeof(path)))
        AGUI_SetString(app, ID_LOCAL, (CONST_STRPTR)path);
}

/* ------------------------------------------------------------------ */
/* event handling                                                      */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* PREFSTEST                                                           */
/* ------------------------------------------------------------------ */

/*
 * Walks the dialog from code, one step per tick: connect, select two
 * folders, snapshot, save. It exercises the same functions the buttons
 * call, which is everything about this dialog that can be checked without
 * a hand on the mouse.
 */
static LONG name_index(struct prefs_state *ps, const char *want)
{
    UWORD i;

    for (i = 0; i < ps->nnames; i++)
        if (!strcmp(ps->name[i], want))
            return (LONG)i;
    return -1;
}

static void test_step(struct AGUIApp *app)
{
    struct prefs_state *ps = (struct prefs_state *)AGUI_UserData(app);

    switch (++ps->step)
    {
    case 1:
        printf("prefs: connect\n");
        do_connect(app);
        printf("prefs: %ld folders listed\n", (long)ps->nnames);
        break;

    case 2:
        {
            static const char *want[] = { "Documents", "Photos", NULL };
            int k;

            for (k = 0; want[k]; k++)
            {
                LONG idx = name_index(ps, want[k]);
                printf("prefs: select %s (index %ld)\n", want[k], (long)idx);
                toggle_folder(app, idx);
            }
        }
        break;

    case 3:
        AGUI_Snapshot(app, "out/nextsync-prefs.ags");
        break;

    case 4:
        printf("prefs: save\n");
        do_save(app);
        if (!ps->saved)
        {
            printf("prefs: save did not go through\n");
            AGUI_Quit(app);
        }
        break;
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* event handling                                                      */
/* ------------------------------------------------------------------ */

static void handler(struct AGUIApp *app, struct AGEvent *ev)
{
    struct prefs_state *ps = (struct prefs_state *)AGUI_UserData(app);

    switch (ev->ev_Type)
    {
    case AGE_TICK:
        if (ps->test)
            test_step(app);
        break;

    case AGE_OPEN:
        show_fields(app);
        /* before the first Connect, show what is already configured */
        {
            UWORD i;
            for (i = 0; i < ps->conf->nfolders; i++)
                add_name(ps, ps->conf->folder[i]);
        }
        refresh_list(app);
        break;

    case AGE_CLICK:
        switch (ev->ev_ID)
        {
        case ID_CONNECT: do_connect(app); break;
        case ID_BROWSE:  do_browse(app);  break;
        case ID_SAVE:    do_save(app);    break;
        case ID_CANCEL:  AGUI_Quit(app);  break;
        }
        break;

    case AGE_CHANGE:
        if (ev->ev_ID == ID_FOLDERS)
            toggle_folder(app, ev->ev_Value);
        break;

    case AGE_CLOSE:
        AGUI_Quit(app);
        break;
    }
}

BOOL nsprefs_show_ex(struct AGUIApp *parent, nsconf *conf,
                     const char *conffile, int test)
{
    struct AGSpec spec;
    struct prefs_state *ps;
    struct AGUIApp *app;
    BOOL saved;

    (void)parent;      /* modal: the parent simply stops responding */

    ps = AllocVec(sizeof(struct prefs_state), MEMF_CLEAR | MEMF_PUBLIC);
    if (!ps)
        return FALSE;

    ps->conf     = conf;
    ps->backup   = *conf;
    ps->conffile = conffile;
    ps->test     = test;

    memset(&spec, 0, sizeof(spec));
    spec.ag_Title    = (STRPTR)"NextSync Preferences";
    spec.ag_Widgets  = widgets;
    spec.ag_Handler  = handler;
    spec.ag_UserData = ps;
    spec.ag_MinCols  = 58;
    if (test)
        spec.ag_Ticks = 10;

    app = AGUI_Open(&spec);
    if (!app)
    {
        FreeVec(ps);
        return FALSE;
    }
    AGUI_Run(app);
    AGUI_Close(app);

    saved = ps->saved;
    if (!saved)
        *conf = ps->backup;            /* Cancel undoes every edit */
    FreeVec(ps);
    return saved;
}

BOOL nsprefs_show(struct AGUIApp *parent, nsconf *conf, const char *conffile)
{
    return nsprefs_show_ex(parent, conf, conffile, 0);
}
