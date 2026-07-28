/*
 * nsconf.h -- NextSync configuration, read from PROGDIR:NextSync.conf.
 *
 * The file the Preferences dialog writes:
 *
 *   server  cloud.example.com
 *   port    443
 *   user    yourname
 *   pass    app-password
 *   local   DH0:Nextcloud
 *   folder  Documents
 *   folder  Photos
 *
 * "local" is the drawer the synced folders live in; each "folder" is a
 * folder in the account's file root, synced into local/<folder>.
 *
 * NextSync 1.0 wrote explicit pairs instead:
 *
 *   pair    /Documents DH0:Sync/Documents
 *
 * Those are still read, still synced, and written back out unchanged when
 * the configuration is saved, so an existing setup keeps working.
 */

#ifndef NSCONF_H
#define NSCONF_H

#include <exec/types.h>

#define NSCONF_MAXFOLDERS 16
#define NSCONF_MAXEXTRA   16
#define NSCONF_MAXPAIRS   (NSCONF_MAXFOLDERS + NSCONF_MAXEXTRA)

typedef struct nsconf_pair
{
    char remote[256];
    char local[256];
} nsconf_pair;

typedef struct nsconf
{
    char  server[128];
    UWORD port;
    char  user[64];
    char  pass[128];

    char  local[256];                          /* base drawer            */
    char  folder[NSCONF_MAXFOLDERS][128];      /* chosen remote folders  */
    UWORD nfolders;

    nsconf_pair extra[NSCONF_MAXEXTRA];        /* verbatim 1.0 pairs     */
    UWORD nextra;

    /* what the sync engine works from, rebuilt from the above */
    nsconf_pair pair[NSCONF_MAXPAIRS];
    UWORD npairs;
} nsconf;

void nsconf_defaults(nsconf *c);

/* TRUE if the file existed and was read; the contents may still be
 * incomplete, ask nsconf_complete(). */
BOOL nsconf_load(nsconf *c, const char *filename);
BOOL nsconf_save(nsconf *c, const char *filename);

/* enough filled in to attempt a sync? */
BOOL nsconf_complete(const nsconf *c);

/* derive pair[] -- call after changing local/folder/extra by hand */
void nsconf_rebuild(nsconf *c);

/* folder selection, used by the Preferences dialog */
BOOL nsconf_has_folder(const nsconf *c, const char *name);
void nsconf_add_folder(nsconf *c, const char *name);
void nsconf_del_folder(nsconf *c, const char *name);

#endif
