/*
 * nsdav.h -- WebDAV operations against a Nextcloud server.
 *
 * Remote paths are always relative to the user's file root ("/OpenClaw/x")
 * and stored UTF-8/ASCII as the server sends them, URL-decoded.
 */

#ifndef NSDAV_H
#define NSDAV_H

#include <exec/types.h>
#include <dos/dos.h>

#include "nshttp.h"

#define NSDAV_MAXPATH 512

typedef struct nsdav_entry
{
    struct nsdav_entry *next;
    char   path[NSDAV_MAXPATH];    /* relative to root, no trailing '/'  */
    BOOL   is_dir;
    ULONG  size;
    ULONG  mtime;                  /* unix seconds                       */
    char   etag[128];
} nsdav_entry;

typedef struct nsdav
{
    nshttp *http;
    char    user[64];              /* dav files/<user> root              */
    char    err[256];
} nsdav;

BOOL nsdav_init(nsdav *d, nshttp *h, const char *user);

/*
 * List one directory level (Depth 1). Returns a linked list of entries
 * (excluding the directory itself), or NULL with *ok=FALSE on error.
 * Free with nsdav_free_list.
 */
nsdav_entry *nsdav_list(nsdav *d, const char *dir, BOOL *ok);
void         nsdav_free_list(nsdav_entry *e);

/* single file/dir info; returns FALSE if missing (404) or on error
 * (check d->err[0] to distinguish) */
BOOL nsdav_stat(nsdav *d, const char *path, nsdav_entry *out);

BOOL nsdav_download(nsdav *d, const char *path, const char *localfile,
                    char *etag_out);
BOOL nsdav_upload(nsdav *d, const char *path, const char *localfile,
                  ULONG mtime, char *etag_out);
BOOL nsdav_mkcol(nsdav *d, const char *path);
BOOL nsdav_delete(nsdav *d, const char *path);

#endif
