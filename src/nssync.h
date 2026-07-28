/*
 * nssync.h -- two way folder synchronisation engine.
 *
 * State model: after every successful transfer the file's server etag,
 * local size and local mtime are recorded. On the next run:
 *
 *   remote etag != state etag              -> changed on the server
 *   local size/mtime != state size/mtime   -> changed locally
 *   in state but missing on one side       -> deleted on that side
 *
 * Both-changed files are conflicts: the local copy is renamed to
 * "<name>.conflict" and the server version is downloaded.
 */

#ifndef NSSYNC_H
#define NSSYNC_H

#include <exec/types.h>

#include "nsdav.h"

typedef void (*nssync_log_fn)(void *user, const char *fmt, ...);
typedef BOOL (*nssync_file_fn)(void *user, const char *action,
                               const char *path, ULONG idx, ULONG total);

typedef struct nssync_stats
{
    ULONG downloaded, uploaded, deleted_local, deleted_remote;
    ULONG dirs_created, conflicts, skipped, failed, unchanged;
    BOOL  aborted;              /* file_cb asked to stop */
} nssync_stats;

/*
 * Synchronise remote directory (e.g. "OpenClaw") with local directory
 * (e.g. "DH0:Sync/OpenClaw"). The local directory is created if needed.
 * State lives in "<local>/.nextsync.state".
 *
 * file_cb is invoked before every transfer; returning FALSE aborts.
 * Returns TRUE if the run completed (individual failures are counted).
 */
BOOL nssync_run(nsdav *dav, const char *remote_root, const char *local_root,
                nssync_log_fn log, nssync_file_fn file_cb, void *user,
                nssync_stats *stats);

#endif
