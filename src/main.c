/*
 * NextSync -- Nextcloud folder synchronisation for AmigaOS 3.1+
 *
 * CLI:
 *   NextSync SYNC            sync every configured folder
 *   NextSync LIST [path]     list a server directory
 *   NextSync PREFS           open Preferences on its own, then exit
 *   NextSync                 open the GUI (see gui.c)
 *
 * Any argument may be followed by SNAPSHOT to dump the screen to out/ once
 * the window has drawn itself, which is how the host side takes pictures.
 *
 * Configuration in PROGDIR:NextSync.conf, see nsconf.h. When it is missing
 * or incomplete the GUI opens Preferences first.
 */

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nsutil.h"
#include "nshttp.h"
#include "nsdav.h"
#include "nssync.h"
#include "nsconf.h"
#include "nsprefs.h"
#include "agui.h"

#define CONFFILE "PROGDIR:NextSync.conf"

/*
 * What the AmigaDOS Version command reads out of the file. Kept in step
 * with aminet/NextSync.readme, which tools/mkdist.sh checks.
 *
 * Deliberately no date. The usual "(dd.mm.yyyy)" cannot be represented by
 * Version on 3.1: a two digit year puts this release in 1926, before the
 * Amiga epoch, so it gives up and prints dashes, and a four digit one is
 * misparsed into a different date entirely. A release date that is simply
 * wrong is worse than none, and the readme and the guide both carry it.
 */
static const char version_tag[] __attribute__((used)) =
    "$VER: NextSync 1.1";

unsigned long __stack = 100000;    /* OpenSSL needs room */

int nextsync_gui(nsconf *conf, const char *conffile);           /* gui.c */
int nextsync_gui_ex(nsconf *conf, const char *conffile, int t);

static void cli_log(void *user, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    (void)user;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
}

static BOOL cli_file(void *user, const char *action, const char *path,
                     ULONG idx, ULONG total)
{
    (void)user;
    printf("  [%lu/%lu] %s %s\n", (unsigned long)idx, (unsigned long)total,
           action, path);
    fflush(stdout);
    /* CTRL-C aborts between files */
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
    {
        SetSignal(0, SIGBREAKF_CTRL_C);
        printf("  *** break: stopping after this file\n");
        return FALSE;
    }
    return TRUE;
}

static nshttp *open_server(nsconf *conf)
{
    char err[256];
    nshttp *h = nshttp_open(conf->server, conf->port, conf->port == 443,
                            conf->user, conf->pass, err);

    if (!h)
        printf("NextSync: %s\n", err);
    return h;
}

static int cmd_sync(nsconf *conf)
{
    nshttp *h;
    nsdav dav;
    UWORD i;
    int rc = 0;

    h = open_server(conf);
    if (!h)
        return 10;
    nsdav_init(&dav, h, conf->user);

    for (i = 0; i < conf->npairs; i++)
    {
        nssync_stats st;

        printf("sync %s <-> %s\n", conf->pair[i].remote, conf->pair[i].local);
        if (nssync_run(&dav, conf->pair[i].remote, conf->pair[i].local,
                       cli_log, cli_file, NULL, &st))
        {
            printf("  done: %lu down, %lu up, %lu deleted, "
                   "%lu dirs, %lu conflicts, %lu failed, %lu unchanged\n",
                   (unsigned long)st.downloaded, (unsigned long)st.uploaded,
                   (unsigned long)(st.deleted_local + st.deleted_remote),
                   (unsigned long)st.dirs_created,
                   (unsigned long)st.conflicts, (unsigned long)st.failed,
                   (unsigned long)st.unchanged);
            if (st.failed)
                rc = 5;
            if (st.aborted)
            {
                printf("  stopped. Files already transferred are kept.\n");
                rc = 5;
                break;
            }
        }
        else
        {
            printf("  sync failed\n");
            rc = 10;
        }
    }
    nshttp_close(h);
    return rc;
}

static int cmd_list(nsconf *conf, const char *path)
{
    nshttp *h;
    nsdav dav;
    nsdav_entry *list, *e;
    BOOL ok;

    h = open_server(conf);
    if (!h)
        return 10;
    nsdav_init(&dav, h, conf->user);

    list = nsdav_list(&dav, path ? path : "", &ok);
    if (!ok)
    {
        printf("NextSync: %s\n", dav.err);
        nshttp_close(h);
        return 10;
    }
    for (e = list; e; e = e->next)
        printf("  %s %10lu  %s\n", e->is_dir ? "DIR" : "   ",
               (unsigned long)e->size, e->path);
    nsdav_free_list(list);
    nshttp_close(h);
    return 0;
}

/* strip the SNAPSHOT modifier out, wherever it appears, and keep the rest */
static int filter_args(int argc, char **argv, const char **out, int max,
                       BOOL *snapshot)
{
    int i, n = 0;

    *snapshot = FALSE;
    for (i = 1; i < argc; i++)
    {
        if (!ns_stricmp(argv[i], "SNAPSHOT"))
            *snapshot = TRUE;
        else if (n < max)
            out[n++] = argv[i];
    }
    return n;
}

static void usage(void)
{
    printf("NextSync: PROGDIR:NextSync.conf is missing or incomplete.\n"
           "  Run NextSync with no arguments and use Preferences,\n"
           "  or write the file by hand:\n"
           "    server <host>\n    port <443>\n    user <name>\n"
           "    pass <password>\n    local <DH0:Nextcloud>\n"
           "    folder <Documents>\n");
}

int main(int argc, char **argv)
{
    nsconf conf;
    const char *arg[4];
    const char *cmd;
    BOOL snapshot, existed;
    int nargs, rc;

    existed = nsconf_load(&conf, CONFFILE);
    nargs = filter_args(argc, argv, arg, 4, &snapshot);
    cmd = nargs > 0 ? arg[0] : NULL;

    if (cmd && !ns_stricmp(cmd, "SYNC"))
    {
        if (!nsconf_complete(&conf))
        {
            usage();
            rc = 20;
        }
        else
            rc = cmd_sync(&conf);
    }
    else if (cmd && !ns_stricmp(cmd, "LIST"))
    {
        if (!conf.server[0] || !conf.user[0])
        {
            usage();
            rc = 20;
        }
        else
            rc = cmd_list(&conf, nargs > 1 ? arg[1] : NULL);
    }
    else if (cmd && !ns_stricmp(cmd, "PREFS"))
    {
        if (snapshot)
            AGUI_AutoSnapshot("out/nextsync-prefs.ags");
        rc = nsprefs_show(NULL, &conf, CONFFILE) ? 0 : 5;
    }
    else if (cmd && !ns_stricmp(cmd, "PREFSTEST"))
    {
        rc = nsprefs_show_ex(NULL, &conf, CONFFILE, 1) ? 0 : 5;
    }
    else
    {
        if (snapshot)
            AGUI_AutoSnapshot("out/nextsync.ags");

        /*
         * First run, or a configuration that cannot sync anything yet:
         * go straight to Preferences. Cancelling still opens the main
         * window, where Project/Preferences is one menu pick away.
         */
        if (!existed || !nsconf_complete(&conf))
            nsprefs_show(NULL, &conf, CONFFILE);

        if (cmd && !ns_stricmp(cmd, "GUITEST"))
            rc = nextsync_gui_ex(&conf, CONFFILE, 1);
        else if (cmd && !ns_stricmp(cmd, "NESTTEST"))
            rc = nextsync_gui_ex(&conf, CONFFILE, 3);
        else if (cmd && !ns_stricmp(cmd, "ABORTTEST"))
            rc = nextsync_gui_ex(&conf, CONFFILE, 4);
        else
            rc = nextsync_gui(&conf, CONFFILE);
    }

    /* last thing before exit, see nshttp.h */
    nshttp_shutdown();
    return rc;
}
