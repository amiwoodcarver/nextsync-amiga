/*
 * nssync.c -- the sync engine: scan both sides, compare against the last
 * synced state, transfer what changed.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#include "nsdav.h"
#include "nssync.h"

#define MAXENTRIES  2048
#define AMIGA_EPOCH 252460800UL     /* 1978-01-01 in unix seconds */

/* what we know about one path, from up to three sources */
struct item
{
    char  path[NSDAV_MAXPATH];      /* relative, '/' separated            */
    UBYTE r_present, l_present, s_present;
    UBYTE r_dir, l_dir, s_dir;
    UBYTE rmdir;                    /* deferred, see RMDIR_ below         */
    ULONG r_size, l_size, s_size;
    ULONG r_mtime, l_mtime, s_mtime;
    char  r_etag[128], s_etag[128];
};

#define RMDIR_LOCAL  1
#define RMDIR_REMOTE 2

struct table
{
    struct item **v;
    LONG          n;
    BOOL          full;             /* hit MAXENTRIES, results are partial */
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static ULONG ds2unix(const struct DateStamp *ds)
{
    return AMIGA_EPOCH + (ULONG)ds->ds_Days * 86400UL
         + (ULONG)ds->ds_Minute * 60UL + (ULONG)ds->ds_Tick / 50UL;
}

static void unix2ds(ULONG t, struct DateStamp *ds)
{
    if (t < AMIGA_EPOCH)
        t = AMIGA_EPOCH;
    t -= AMIGA_EPOCH;
    ds->ds_Days   = t / 86400UL;
    ds->ds_Minute = (t % 86400UL) / 60UL;
    ds->ds_Tick   = (t % 60UL) * 50UL;
}

/* local path join: AmigaDOS separators, remote uses '/' */
static void local_path(const char *root, const char *rel, char *dst)
{
    LONG n = strlen(root);

    strcpy(dst, root);
    if (n && root[n - 1] != ':' && root[n - 1] != '/')
        strcat(dst, "/");
    strcat(dst, rel);
}

static struct item *tab_get(struct table *t, const char *path)
{
    LONG i;

    for (i = 0; i < t->n; i++)
        if (!strcmp(t->v[i]->path, path))
            return t->v[i];

    if (t->n >= MAXENTRIES)
    {
        t->full = TRUE;
        return NULL;
    }
    {
        struct item *it = AllocVec(sizeof(struct item), MEMF_CLEAR | MEMF_PUBLIC);
        if (!it)
            return NULL;
        strncpy(it->path, path, NSDAV_MAXPATH - 1);
        t->v[t->n++] = it;
        return it;
    }
}

static void tab_free(struct table *t)
{
    LONG i;

    for (i = 0; i < t->n; i++)
        FreeVec(t->v[i]);
    FreeVec(t->v);
}

/*
 * A name the Amiga side can even attempt: no ':' (device separator).
 * Length limits differ per filesystem (FFS 30, PFS/SFS ~100, the
 * emulator's directory handler 106), so length errors are left to the
 * filesystem itself and show up as per-file failures instead.
 */
static BOOL name_ok(const char *seg)
{
    LONG n = 0;

    while (seg[n])
    {
        if (seg[n] == ':')
            return FALSE;
        n++;
    }
    return n > 0 && n <= 255;
}

static BOOL path_ok(const char *path)
{
    char seg[NSDAV_MAXPATH];
    LONG i = 0;

    while (*path)
    {
        i = 0;
        while (*path && *path != '/')
        {
            if (i < NSDAV_MAXPATH - 1)
                seg[i++] = *path;
            path++;
        }
        seg[i] = 0;
        if (!name_ok(seg))
            return FALSE;
        if (*path == '/')
            path++;
    }
    return TRUE;
}

/* does name end in the given suffix? */
static BOOL ends_with(const char *name, const char *suffix)
{
    LONG n = strlen(name), s = strlen(suffix);

    return (BOOL)(n >= s && !strcmp(name + n - s, suffix));
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */
/* ------------------------------------------------------------------ */

static BOOL scan_remote(nsdav *dav, struct table *t, const char *dir,
                        nssync_log_fn log, void *user, int depth)
{
    nsdav_entry *list, *e;
    BOOL ok;

    if (depth > 16)
        return TRUE;

    list = nsdav_list(dav, dir, &ok);
    if (!ok)
    {
        log(user, "  remote scan: %s", dav->err);
        return FALSE;
    }

    for (e = list; e; e = e->next)
    {
        struct item *it = tab_get(t, e->path);

        if (!it)
            continue;
        it->r_present = 1;
        it->r_dir   = e->is_dir ? 1 : 0;
        it->r_size  = e->size;
        it->r_mtime = e->mtime;
        strcpy(it->r_etag, e->etag);

        if (e->is_dir)
            if (!scan_remote(dav, t, e->path, log, user, depth + 1))
            {
                nsdav_free_list(list);
                return FALSE;
            }
    }
    nsdav_free_list(list);
    return TRUE;
}

#define MAXSTALE 16

static void scan_local_dir(struct table *t, const char *root,
                           const char *rel, int depth,
                           nssync_log_fn log, void *user)
{
    char full[NSDAV_MAXPATH];
    BPTR lock;
    struct FileInfoBlock *fib;
    char stale[MAXSTALE][108];       /* leftovers to remove after ExNext */
    LONG nstale = 0, k;

    if (depth > 16)
        return;

    if (rel[0])
        local_path(root, rel, full);
    else
        strcpy(full, root);

    lock = Lock((STRPTR)full, ACCESS_READ);
    if (!lock)
        return;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib)
    {
        UnLock(lock);
        return;
    }

    if (Examine(lock, fib))
    {
        while (ExNext(lock, fib))
        {
            char childrel[NSDAV_MAXPATH];

            if (fib->fib_FileName[0] == '.')
                continue;                    /* state file, temp files */

            /*
             * A .nspart is the half of a download that never finished --
             * the machine lost power, or NextSync was killed. It is not
             * the user's file and must never be mistaken for one: left in
             * the table it would look like something new on this side and
             * be uploaded to the server. Note it and delete it below,
             * once ExNext has finished walking this directory.
             */
            if (ends_with(fib->fib_FileName, ".nspart"))
            {
                if (nstale < MAXSTALE)
                    strcpy(stale[nstale++], fib->fib_FileName);
                continue;
            }

            if (rel[0])
                sprintf(childrel, "%s/%s", rel, fib->fib_FileName);
            else
                strcpy(childrel, fib->fib_FileName);

            {
                struct item *it = tab_get(t, childrel);
                if (it)
                {
                    it->l_present = 1;
                    it->l_dir   = (fib->fib_DirEntryType > 0) ? 1 : 0;
                    it->l_size  = fib->fib_Size;
                    it->l_mtime = ds2unix(&fib->fib_Date);
                }
            }
            if (fib->fib_DirEntryType > 0)
                scan_local_dir(t, root, childrel, depth + 1, log, user);
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);

    for (k = 0; k < nstale; k++)
    {
        char victim[NSDAV_MAXPATH];

        if (rel[0])
            sprintf(victim, "%s/%s", rel, stale[k]);
        else
            strcpy(victim, stale[k]);
        {
            char path[NSDAV_MAXPATH];
            local_path(root, victim, path);
            if (DeleteFile((STRPTR)path))
                log(user, "  discarded unfinished download %s", victim);
        }
    }
}

/* ------------------------------------------------------------------ */
/* state file                                                          */
/* ------------------------------------------------------------------ */

static void state_name(const char *root, char *dst)
{
    local_path(root, ".nextsync.state", dst);
}

static void load_state(struct table *t, const char *root)
{
    char fn[NSDAV_MAXPATH];
    BPTR fh;
    char line[NSDAV_MAXPATH + 256];

    state_name(root, fn);
    fh = Open((STRPTR)fn, MODE_OLDFILE);
    if (!fh)
        return;

    while (FGets(fh, (STRPTR)line, sizeof(line)))
    {
        /* etag|size|mtime|isdir|path */
        char *f[5];
        char *p = line;
        LONG  i;

        for (i = 0; i < 4; i++)
        {
            f[i] = p;
            p = strchr(p, '|');
            if (!p)
                break;
            *p++ = 0;
        }
        if (i < 4)
            continue;
        f[4] = p;
        {
            LONG n = strlen(f[4]);
            while (n && (f[4][n-1] == '\n' || f[4][n-1] == '\r'))
                f[4][--n] = 0;
        }
        if (!f[4][0])
            continue;
        {
            struct item *it = tab_get(t, f[4]);
            ULONG v;
            const char *s;

            if (!it)
                continue;
            it->s_present = 1;
            strncpy(it->s_etag, f[0], sizeof(it->s_etag) - 1);
            v = 0; s = f[1]; while (*s>='0'&&*s<='9') v = v*10 + (*s++-'0');
            it->s_size = v;
            v = 0; s = f[2]; while (*s>='0'&&*s<='9') v = v*10 + (*s++-'0');
            it->s_mtime = v;
            it->s_dir = (f[3][0] == '1');
        }
    }
    Close(fh);
}

static void save_state(struct table *t, const char *root)
{
    char fn[NSDAV_MAXPATH];
    BPTR fh;
    LONG i;
    char line[NSDAV_MAXPATH + 256];

    state_name(root, fn);
    fh = Open((STRPTR)fn, MODE_NEWFILE);
    if (!fh)
        return;

    for (i = 0; i < t->n; i++)
    {
        struct item *it = t->v[i];

        if (!it->s_present)
            continue;
        sprintf(line, "%s|%lu|%lu|%ld|%s\n",
                it->s_etag, (unsigned long)it->s_size,
                (unsigned long)it->s_mtime, (long)(it->s_dir ? 1 : 0),
                it->path);
        Write(fh, line, strlen(line));
    }
    Close(fh);
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

static BOOL ensure_local_dirtree(const char *root, const char *rel)
{
    /* create every directory component of rel (rel itself included) */
    char partial[NSDAV_MAXPATH];
    char full[NSDAV_MAXPATH];
    LONG i = 0, o = 0;

    while (rel[i])
    {
        while (rel[i] && rel[i] != '/')
            partial[o++] = rel[i++];
        partial[o] = 0;
        local_path(root, partial, full);
        {
            BPTR lock = Lock((STRPTR)full, ACCESS_READ);
            if (lock)
                UnLock(lock);
            else
            {
                BPTR nl = CreateDir((STRPTR)full);
                if (!nl)
                    return FALSE;
                UnLock(nl);
            }
        }
        if (rel[i] == '/')
            partial[o++] = rel[i++];
    }
    return TRUE;
}

/* make sure the parent directories of file path rel exist */
static void ensure_parent(const char *root, const char *rel)
{
    char parent[NSDAV_MAXPATH];
    char *slash;

    strcpy(parent, rel);
    slash = strrchr(parent, '/');
    if (!slash)
        return;
    *slash = 0;
    ensure_local_dirtree(root, parent);
}

static BOOL delete_local_tree(const char *root, const char *rel)
{
    char full[NSDAV_MAXPATH];

    local_path(root, rel, full);
    return DeleteFile((STRPTR)full) ? TRUE : FALSE;
}

static void set_local_mtime(const char *root, const char *rel, ULONG mtime)
{
    char full[NSDAV_MAXPATH];
    struct DateStamp ds;

    if (!mtime)
        return;
    local_path(root, rel, full);
    unix2ds(mtime, &ds);
    SetFileDate((STRPTR)full, &ds);
}

static ULONG get_local_mtime(const char *root, const char *rel, ULONG *size)
{
    char full[NSDAV_MAXPATH];
    BPTR lock;
    struct FileInfoBlock *fib;
    ULONG t = 0;

    *size = 0;
    local_path(root, rel, full);
    lock = Lock((STRPTR)full, ACCESS_READ);
    if (!lock)
        return 0;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib)
    {
        if (Examine(lock, fib))
        {
            t = ds2unix(&fib->fib_Date);
            *size = fib->fib_Size;
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return t;
}

/* ------------------------------------------------------------------ */
/* the run                                                             */
/* ------------------------------------------------------------------ */

/* depth-first ordering so directories come before their contents */
static int cmp_paths(const void *a, const void *b)
{
    return strcmp((*(struct item **)a)->path, (*(struct item **)b)->path);
}

static void sort_items(struct table *t)
{
    /* insertion sort, n is small */
    LONG i, j;

    for (i = 1; i < t->n; i++)
    {
        struct item *key = t->v[i];
        for (j = i - 1; j >= 0 && cmp_paths(&t->v[j], &key) > 0; j--)
            t->v[j + 1] = t->v[j];
        t->v[j + 1] = key;
    }
}

BOOL nssync_run(nsdav *dav, const char *remote_root, const char *local_root,
                nssync_log_fn log, nssync_file_fn file_cb, void *user,
                nssync_stats *st)
{
    struct table t;
    LONG i;
    ULONG total_work = 0, done_work = 0;
    char lpath[NSDAV_MAXPATH];
    char rpath[NSDAV_MAXPATH];
    const char *rroot = remote_root;

    while (*rroot == '/')
        rroot++;

    memset(st, 0, sizeof(*st));
    t.v = AllocVec(sizeof(void *) * MAXENTRIES, MEMF_CLEAR | MEMF_PUBLIC);
    t.n = 0;
    t.full = FALSE;
    if (!t.v)
        return FALSE;

    /* make sure both roots exist; the local one may be several levels
     * deep ("DH0:Sync/OpenClaw"), so create it component by component */
    {
        BPTR lock = Lock((STRPTR)local_root, ACCESS_READ);
        if (lock)
            UnLock(lock);
        else
        {
            char head[NSDAV_MAXPATH];
            const char *rest = strchr(local_root, ':');
            BOOL created = FALSE;

            if (rest)
            {
                LONG base = rest - local_root + 1;
                const char *p = local_root + base;

                while (*p)
                {
                    while (*p && *p != '/')
                        p++;
                    memcpy(head, local_root, p - local_root);
                    head[p - local_root] = 0;
                    {
                        BPTR l2 = Lock((STRPTR)head, ACCESS_READ);
                        if (l2)
                            UnLock(l2);
                        else
                        {
                            BPTR nl = CreateDir((STRPTR)head);
                            if (!nl)
                                break;
                            UnLock(nl);
                        }
                    }
                    if (*p == '/')
                        p++;
                }
                lock = Lock((STRPTR)local_root, ACCESS_READ);
                if (lock)
                {
                    UnLock(lock);
                    created = TRUE;
                }
            }
            if (!created)
            {
                log(user, "cannot create local dir %s", local_root);
                FreeVec(t.v);
                return FALSE;
            }
        }
    }
    {
        nsdav_entry e;
        if (!nsdav_stat(dav, rroot, &e))
        {
            if (dav->err[0])
            {
                log(user, "%s", dav->err);
                FreeVec(t.v);
                return FALSE;
            }
            log(user, "creating remote /%s", rroot);
            if (!nsdav_mkcol(dav, rroot))
            {
                log(user, "%s", dav->err);
                FreeVec(t.v);
                return FALSE;
            }
        }
    }

    log(user, "scanning server...");
    if (!scan_remote(dav, &t, rroot, log, user, 0))
    {
        tab_free(&t);
        return FALSE;
    }

    /* strip the remote root prefix from paths so both sides align */
    {
        LONG pl = strlen(rroot);

        for (i = 0; i < t.n; i++)
        {
            struct item *it = t.v[i];
            if (!strncmp(it->path, rroot, pl) && it->path[pl] == '/')
                memmove(it->path, it->path + pl + 1,
                        strlen(it->path + pl + 1) + 1);
        }
    }

    log(user, "scanning local...");
    scan_local_dir(&t, local_root, "", 0, log, user);
    load_state(&t, local_root);
    sort_items(&t);

    /* count the work for progress reporting */
    for (i = 0; i < t.n; i++)
    {
        struct item *x = t.v[i];
        if (x->r_present != x->l_present ||
            (x->r_present && x->l_present && !x->r_dir && !x->l_dir))
            total_work++;
    }

    for (i = 0; i < t.n; i++)
    {
        struct item *x = t.v[i];
        BOOL rch, lch;

        sprintf(rpath, "%s/%s", rroot, x->path);

        if (!path_ok(x->path))
        {
            log(user, "  skip (name unusable on Amiga): %s", x->path);
            st->skipped++;
            continue;
        }

        /* ---------------- directories ---------------- */
        if ((x->r_present && x->r_dir) || (x->l_present && x->l_dir))
        {
            if (x->r_present && !x->l_present)
            {
                if (x->s_present)
                    x->rmdir = RMDIR_REMOTE;   /* deepest first, below */
                else if (ensure_local_dirtree(local_root, x->path))
                {
                    st->dirs_created++;
                    x->s_present = 1;
                    x->s_dir = 1;
                    x->s_etag[0] = 0;
                }
                else
                {
                    log(user, "  cannot create dir %s", x->path);
                    st->failed++;
                }
            }
            else if (!x->r_present && x->l_present)
            {
                if (x->s_present)
                    x->rmdir = RMDIR_LOCAL;    /* deepest first, below */
                else
                {
                    log(user, "  mkdir remote %s", x->path);
                    if (nsdav_mkcol(dav, rpath))
                    {
                        st->dirs_created++;
                        x->s_present = 1;
                        x->s_dir = 1;
                        x->s_etag[0] = 0;
                    }
                    else
                    {
                        log(user, "  %s", dav->err);
                        st->failed++;
                    }
                }
            }
            else if (x->r_present && x->l_present)
            {
                x->s_present = 1;
                x->s_dir = 1;
            }
            continue;
        }

        /* ---------------- files ---------------- */
        local_path(local_root, x->path, lpath);

        rch = x->r_present &&
              (!x->s_present || strcmp(x->r_etag, x->s_etag) != 0);
        lch = x->l_present &&
              (!x->s_present || x->l_size != x->s_size ||
               (x->l_mtime > x->s_mtime + 2) ||
               (x->s_mtime > x->l_mtime + 2));

        if (x->r_present && !x->l_present)
        {
            if (x->s_present && !rch)
            {
                /* unchanged remotely, gone locally -> delete remote */
                if (file_cb && !file_cb(user, "delete", x->path,
                                        ++done_work, total_work))
                {
                    st->aborted = TRUE;
                    break;
                }
                log(user, "  delete remote %s", x->path);
                if (nsdav_delete(dav, rpath))
                {
                    st->deleted_remote++;
                    x->s_present = 0;
                }
                else
                {
                    log(user, "  %s", dav->err);
                    st->failed++;
                }
            }
            else
            {
                /* new (or changed) on server -> download */
                if (file_cb && !file_cb(user, "download", x->path,
                                        ++done_work, total_work))
                {
                    st->aborted = TRUE;
                    break;
                }
                ensure_parent(local_root, x->path);
                if (nsdav_download(dav, rpath, lpath, x->s_etag))
                {
                    set_local_mtime(local_root, x->path, x->r_mtime);
                    st->downloaded++;
                    x->s_present = 1;
                    x->s_dir = 0;
                    x->s_mtime = get_local_mtime(local_root, x->path,
                                                 &x->s_size);
                }
                else
                {
                    log(user, "  %s", dav->err);
                    st->failed++;
                }
            }
        }
        else if (!x->r_present && x->l_present)
        {
            if (x->s_present && !lch)
            {
                if (file_cb && !file_cb(user, "delete local", x->path,
                                        ++done_work, total_work))
                {
                    st->aborted = TRUE;
                    break;
                }
                log(user, "  delete local %s", x->path);
                if (delete_local_tree(local_root, x->path))
                {
                    st->deleted_local++;
                    x->s_present = 0;
                }
                else
                    st->failed++;
            }
            else
            {
                if (file_cb && !file_cb(user, "upload", x->path,
                                        ++done_work, total_work))
                {
                    st->aborted = TRUE;
                    break;
                }
                if (nsdav_upload(dav, rpath, lpath, x->l_mtime, x->s_etag))
                {
                    st->uploaded++;
                    x->s_present = 1;
                    x->s_dir = 0;
                    x->s_size = x->l_size;
                    x->s_mtime = x->l_mtime;
                }
                else
                {
                    log(user, "  %s", dav->err);
                    st->failed++;
                }
            }
        }
        else if (x->r_present && x->l_present)
        {
            if (rch && lch)
            {
                char cpath[NSDAV_MAXPATH];
                char cfull[NSDAV_MAXPATH];

                st->conflicts++;
                sprintf(cpath, "%s.conflict", x->path);
                local_path(local_root, cpath, cfull);
                log(user, "  CONFLICT %s (local copy -> %s)",
                    x->path, cpath);
                DeleteFile((STRPTR)cfull);
                Rename((STRPTR)lpath, (STRPTR)cfull);

                if (file_cb && !file_cb(user, "download", x->path,
                                        ++done_work, total_work))
                {
                    st->aborted = TRUE;
                    break;
                }
                if (nsdav_download(dav, rpath, lpath, x->s_etag))
                {
                    set_local_mtime(local_root, x->path, x->r_mtime);
                    st->downloaded++;
                    x->s_present = 1;
                    x->s_dir = 0;
                    x->s_mtime = get_local_mtime(local_root, x->path,
                                                 &x->s_size);
                }
                else
                {
                    log(user, "  %s", dav->err);
                    st->failed++;
                }
            }
            else if (rch)
            {
                if (file_cb && !file_cb(user, "download", x->path,
                                        ++done_work, total_work))
                {
                    st->aborted = TRUE;
                    break;
                }
                if (nsdav_download(dav, rpath, lpath, x->s_etag))
                {
                    set_local_mtime(local_root, x->path, x->r_mtime);
                    st->downloaded++;
                    x->s_present = 1;
                    x->s_dir = 0;
                    x->s_mtime = get_local_mtime(local_root, x->path,
                                                 &x->s_size);
                }
                else
                {
                    log(user, "  %s", dav->err);
                    st->failed++;
                }
            }
            else if (lch)
            {
                if (file_cb && !file_cb(user, "upload", x->path,
                                        ++done_work, total_work))
                {
                    st->aborted = TRUE;
                    break;
                }
                if (nsdav_upload(dav, rpath, lpath, x->l_mtime, x->s_etag))
                {
                    st->uploaded++;
                    x->s_present = 1;
                    x->s_dir = 0;
                    x->s_size = x->l_size;
                    x->s_mtime = x->l_mtime;
                }
                else
                {
                    log(user, "  %s", dav->err);
                    st->failed++;
                }
            }
            else
            {
                st->unchanged++;
                x->s_present = 1;      /* refresh state entry */
            }
        }
        else if (x->s_present)
        {
            /* gone on both sides */
            x->s_present = 0;
        }
    }

    /*
     * Directory removals, deepest first. They have to come after the file
     * pass -- a drawer can only be deleted once it is empty -- and in
     * reverse path order, so "a/b" goes before "a". Doing it in the main
     * loop instead would leave the empty drawers behind and need one more
     * run per level of nesting to clear them.
     */
    for (i = t.n - 1; i >= 0 && !st->aborted; i--)
    {
        struct item *x = t.v[i];

        if (!x->rmdir)
            continue;

        sprintf(rpath, "%s/%s", rroot, x->path);

        if (x->rmdir == RMDIR_REMOTE)
        {
            log(user, "  rmdir remote %s", x->path);
            if (nsdav_delete(dav, rpath))
            {
                st->deleted_remote++;
                x->s_present = 0;
            }
            else
            {
                log(user, "  %s", dav->err);
                st->failed++;
            }
        }
        else
        {
            log(user, "  rmdir local %s", x->path);
            if (delete_local_tree(local_root, x->path))
            {
                st->deleted_local++;
                x->s_present = 0;
            }
            else
            {
                /* still not empty: something in it was skipped or failed.
                 * Keep the state entry so the next run retries. */
                log(user, "  %s not empty, left in place", x->path);
                x->s_present = 1;
            }
        }
    }

    if (t.full)
        log(user, "  WARNING: more than %ld entries, sync is incomplete",
            (long)MAXENTRIES);

    save_state(&t, local_root);
    tab_free(&t);
    return TRUE;
}
