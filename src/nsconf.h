/*
 * nsconf.h -- NextSync configuration, read from PROGDIR:NextSync.conf.
 *
 *   server  cloud.example.com
 *   port    443
 *   user    agent
 *   pass    secret
 *   pair    /Documents DH0:Sync/Documents
 *   pair    /Documents DH0:Sync/Documents
 */

#ifndef NSCONF_H
#define NSCONF_H

#include <exec/types.h>

#define NSCONF_MAXPAIRS 16

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
    nsconf_pair pair[NSCONF_MAXPAIRS];
    UWORD npairs;
} nsconf;

BOOL nsconf_load(nsconf *c, const char *filename);
BOOL nsconf_save(nsconf *c, const char *filename);

#endif
