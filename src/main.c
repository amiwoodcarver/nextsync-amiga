/*
 * NextSync -- Nextcloud folder synchronisation for AmigaOS 3.1+
 *
 * CLI:
 *   NextSync SYNC            sync every configured pair
 *   NextSync LIST [path]     list a server directory
 *   NextSync                 open the GUI (see gui.c)
 *   NextSync SNAPSHOT        open the GUI, save a screen dump, exit
 *
 * Configuration in PROGDIR:NextSync.conf, see nsconf.h.
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
#include "agui.h"

unsigned long __stack = 100000;    /* OpenSSL needs room */

int nextsync_gui(nsconf *conf);            /* gui.c */
int nextsync_gui_ex(nsconf *conf, int t);

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

int main(int argc, char **argv)
{
    nsconf conf;

    if (!nsconf_load(&conf, "PROGDIR:NextSync.conf"))
    {
        printf("NextSync: no usable PROGDIR:NextSync.conf\n"
               "  server <host>\n  port <443>\n  user <name>\n"
               "  pass <password>\n  pair </remote/path> <local:path>\n");
        return 20;
    }

    {
        int rc;

        if (argc > 1 && !ns_stricmp(argv[1], "SNAPSHOT"))
            AGUI_AutoSnapshot("out/nextsync.ags");

        if (argc > 1 && !ns_stricmp(argv[1], "SYNC"))
            rc = cmd_sync(&conf);
        else if (argc > 1 && !ns_stricmp(argv[1], "GUITEST"))
            rc = nextsync_gui_ex(&conf, 1);
        else if (argc > 1 && !ns_stricmp(argv[1], "LIST"))
            rc = cmd_list(&conf, argc > 2 ? argv[2] : NULL);
        else
            rc = nextsync_gui(&conf);

        /* last thing before exit, see nshttp.h */
        nshttp_shutdown();
        return rc;
    }
}
