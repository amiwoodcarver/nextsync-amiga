/*
 * nsdav.c -- WebDAV/Nextcloud operations on top of nshttp + nsxml.
 */

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#include "nsutil.h"
#include "nshttp.h"
#include "nsxml.h"
#include "nsdav.h"

static const char propfind_body[] =
    "<?xml version=\"1.0\"?>"
    "<d:propfind xmlns:d=\"DAV:\">"
    "<d:prop>"
    "<d:resourcetype/><d:getetag/><d:getcontentlength/><d:getlastmodified/>"
    "</d:prop>"
    "</d:propfind>";

BOOL nsdav_init(nsdav *d, nshttp *h, const char *user)
{
    memset(d, 0, sizeof(*d));
    d->http = h;
    strncpy(d->user, user, sizeof(d->user) - 1);
    return TRUE;
}

/* /remote.php/dav/files/<user>/<path>, URL encoded */
static void build_url(nsdav *d, const char *path, char *dst, LONG dstlen)
{
    char raw[NSDAV_MAXPATH + 96];

    while (*path == '/')
        path++;
    sprintf(raw, "/remote.php/dav/files/%s/%s", d->user, path);
    nshttp_urlencode(raw, dst, dstlen);
}

static void url_decode(char *s)
{
    char *o = s;

    while (*s)
    {
        if (s[0] == '%' && s[1] && s[2])
        {
            int hi = s[1] >= 'a' ? s[1]-'a'+10 : s[1] >= 'A' ? s[1]-'A'+10 : s[1]-'0';
            int lo = s[2] >= 'a' ? s[2]-'a'+10 : s[2] >= 'A' ? s[2]-'A'+10 : s[2]-'0';
            *o++ = (char)(hi * 16 + lo);
            s += 3;
        }
        else
            *o++ = *s++;
    }
    *o = 0;
}

/* "Tue, 09 Jun 2026 21:27:05 GMT" -> unix seconds */
static ULONG parse_http_date(const char *s)
{
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const UWORD mdays[12] =
        { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    LONG day, year, hh, mm, ss, m = -1, i;
    char mname[4];
    const char *p = strchr(s, ',');

    if (!p)
        return 0;
    p++;
    while (*p == ' ')
        p++;
    day = 0;
    while (*p >= '0' && *p <= '9')
        day = day * 10 + (*p++ - '0');
    while (*p == ' ')
        p++;
    for (i = 0; i < 3 && p[i]; i++)
        mname[i] = p[i];
    mname[3] = 0;
    for (i = 0; i < 12; i++)
        if (!strncmp(mname, mon + i * 3, 3))
        {
            m = i;
            break;
        }
    if (m < 0)
        return 0;
    p += 3;
    while (*p == ' ')
        p++;
    year = 0;
    while (*p >= '0' && *p <= '9')
        year = year * 10 + (*p++ - '0');
    if (*p++ != ' ')
        return 0;
    hh = (p[0]-'0')*10 + (p[1]-'0');
    mm = (p[3]-'0')*10 + (p[4]-'0');
    ss = (p[6]-'0')*10 + (p[7]-'0');

    {
        LONG days = (year - 1970) * 365
                  + (year - 1969) / 4          /* leap days before year   */
                  - (year - 1901) / 100
                  + (year - 1601) / 400
                  + mdays[m] + day - 1;
        if (m >= 2 && ((year%4==0 && year%100!=0) || year%400==0))
            days++;
        return (ULONG)days * 86400UL + hh * 3600 + mm * 60 + ss;
    }
}

/* ------------------------------------------------------------------ */
/* PROPFIND parsing                                                    */
/* ------------------------------------------------------------------ */

struct pf_ctx
{
    nsdav       *dav;
    nsdav_entry *head, *tail;
    nsdav_entry  cur;
    BOOL         have_href;
    char         self[NSDAV_MAXPATH];   /* the listed dir itself        */
    BOOL         first;
    BOOL         oom;
    nsdav_entry *single;                /* nsdav_stat mode              */
};

static void pf_start(void *user, const char *tag)
{
    struct pf_ctx *c = (struct pf_ctx *)user;

    if (!ns_stricmp(tag, "response"))
    {
        memset(&c->cur, 0, sizeof(c->cur));
        c->have_href = FALSE;
    }
    else if (!ns_stricmp(tag, "collection"))
        c->cur.is_dir = TRUE;
}

static void pf_end(void *user, const char *tag, const char *text)
{
    struct pf_ctx *c = (struct pf_ctx *)user;

    if (!ns_stricmp(tag, "href"))
    {
        /* /remote.php/dav/files/<user>/a/b/ -> a/b */
        char buf[NSDAV_MAXPATH + 96];
        char pfx[96];
        LONG pl;

        strncpy(buf, text, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        url_decode(buf);
        sprintf(pfx, "/remote.php/dav/files/%s/", c->dav->user);
        pl = strlen(pfx);
        if (!strncmp(buf, pfx, pl))
        {
            char *rel = buf + pl;
            LONG  n = strlen(rel);
            while (n > 0 && rel[n - 1] == '/')
                rel[--n] = 0;
            strncpy(c->cur.path, rel, NSDAV_MAXPATH - 1);
            c->have_href = TRUE;
        }
    }
    else if (!ns_stricmp(tag, "getetag"))
    {
        char *e = c->cur.etag;
        LONG  n = 0;
        while (*text && n < (LONG)sizeof(c->cur.etag) - 1)
        {
            if (*text != '"')
                e[n++] = *text;
            text++;
        }
        e[n] = 0;
    }
    else if (!ns_stricmp(tag, "getcontentlength"))
    {
        ULONG v = 0;
        while (*text >= '0' && *text <= '9')
            v = v * 10 + (*text++ - '0');
        c->cur.size = v;
    }
    else if (!ns_stricmp(tag, "getlastmodified"))
        c->cur.mtime = parse_http_date(text);
    else if (!ns_stricmp(tag, "response"))
    {
        if (!c->have_href)
            return;

        if (c->single)
        {
            /* stat mode: only one response expected */
            *c->single = c->cur;
            c->single->next = NULL;
            return;
        }
        if (c->first)
        {
            /* the first response is the directory itself: skip */
            c->first = FALSE;
            strcpy(c->self, c->cur.path);
            return;
        }
        {
            nsdav_entry *e = AllocVec(sizeof(nsdav_entry), MEMF_PUBLIC);
            if (!e)
            {
                c->oom = TRUE;
                return;
            }
            *e = c->cur;
            e->next = NULL;
            if (c->tail)
                c->tail->next = e;
            else
                c->head = e;
            c->tail = e;
        }
    }
}

nsdav_entry *nsdav_list(nsdav *d, const char *dir, BOOL *ok)
{
    char url[NSDAV_MAXPATH * 3];
    nshttp_response resp;
    struct pf_ctx ctx;

    *ok = FALSE;
    d->err[0] = 0;

    build_url(d, dir, url, sizeof(url));
    if (nshttp_request(d->http, "PROPFIND", url,
                       "Depth: 1\r\nContent-Type: application/xml\r\n",
                       propfind_body, sizeof(propfind_body) - 1, &resp) != 0)
    {
        sprintf(d->err, "PROPFIND %s: %s", dir, nshttp_error(d->http));
        return NULL;
    }
    if (resp.status != 207)
    {
        sprintf(d->err, "PROPFIND %s: HTTP %ld", dir, (long)resp.status);
        return NULL;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.dav = d;
    ctx.first = TRUE;

    if (nsxml_parse(resp.body, resp.body_len, pf_start, pf_end, &ctx) != 0 ||
        ctx.oom)
    {
        nsdav_free_list(ctx.head);
        sprintf(d->err, "PROPFIND %s: bad XML", dir);
        return NULL;
    }
    *ok = TRUE;
    return ctx.head;
}

void nsdav_free_list(nsdav_entry *e)
{
    while (e)
    {
        nsdav_entry *n = e->next;
        FreeVec(e);
        e = n;
    }
}

BOOL nsdav_stat(nsdav *d, const char *path, nsdav_entry *out)
{
    char url[NSDAV_MAXPATH * 3];
    nshttp_response resp;
    struct pf_ctx ctx;

    d->err[0] = 0;
    memset(out, 0, sizeof(*out));

    build_url(d, path, url, sizeof(url));
    if (nshttp_request(d->http, "PROPFIND", url,
                       "Depth: 0\r\nContent-Type: application/xml\r\n",
                       propfind_body, sizeof(propfind_body) - 1, &resp) != 0)
    {
        sprintf(d->err, "PROPFIND %s: %s", path, nshttp_error(d->http));
        return FALSE;
    }
    if (resp.status == 404)
        return FALSE;
    if (resp.status != 207)
    {
        sprintf(d->err, "PROPFIND %s: HTTP %ld", path, (long)resp.status);
        return FALSE;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.dav = d;
    ctx.single = out;
    if (nsxml_parse(resp.body, resp.body_len, pf_start, pf_end, &ctx) != 0)
    {
        sprintf(d->err, "PROPFIND %s: bad XML", path);
        return FALSE;
    }
    return TRUE;
}

/* download to a temp name, rename into place on success */
BOOL nsdav_download(nsdav *d, const char *path, const char *localfile,
                    char *etag_out)
{
    char url[NSDAV_MAXPATH * 3];
    char tmp[NSDAV_MAXPATH];
    nshttp_response resp;
    BPTR fh;

    d->err[0] = 0;
    build_url(d, path, url, sizeof(url));

    sprintf(tmp, "%s.nspart", localfile);
    fh = Open((STRPTR)tmp, MODE_NEWFILE);
    if (!fh)
    {
        sprintf(d->err, "cannot create %s", tmp);
        return FALSE;
    }

    if (nshttp_get_file(d->http, url, fh, &resp) != 0)
    {
        /* the transport gave up. The status may well say 200 -- the
         * headers arrived, the body did not -- so report what actually
         * went wrong rather than the status. */
        Close(fh);
        DeleteFile((STRPTR)tmp);
        sprintf(d->err, "GET %s: %s", path, nshttp_error(d->http));
        return FALSE;
    }
    if (resp.status != 200)
    {
        Close(fh);
        DeleteFile((STRPTR)tmp);
        sprintf(d->err, "GET %s: HTTP %ld", path, (long)resp.status);
        return FALSE;
    }

    /*
     * The transport already fails a transfer that is cut short, but only
     * when it knows how long the body was meant to be. Checking the bytes
     * that actually reached the disk against the header is the last thing
     * standing between a truncated file and being renamed into place and
     * recorded as a good copy -- after which nothing would ever notice.
     */
    if (resp.content_length > 0 &&
        (ULONG)resp.body_len != resp.content_length)
    {
        Close(fh);
        DeleteFile((STRPTR)tmp);
        sprintf(d->err, "GET %s: short transfer, %lu of %lu bytes",
                path, (unsigned long)resp.body_len,
                (unsigned long)resp.content_length);
        return FALSE;
    }
    Close(fh);

    DeleteFile((STRPTR)localfile);          /* may not exist, fine */
    if (!Rename((STRPTR)tmp, (STRPTR)localfile))
    {
        sprintf(d->err, "cannot rename %s into place", tmp);
        DeleteFile((STRPTR)tmp);
        return FALSE;
    }
    if (etag_out)
        strcpy(etag_out, resp.etag);
    return TRUE;
}

BOOL nsdav_upload(nsdav *d, const char *path, const char *localfile,
                  ULONG mtime, char *etag_out)
{
    char url[NSDAV_MAXPATH * 3];
    char extra[128];
    nshttp_response resp;
    BPTR fh;
    LONG size;

    d->err[0] = 0;
    build_url(d, path, url, sizeof(url));

    fh = Open((STRPTR)localfile, MODE_OLDFILE);
    if (!fh)
    {
        sprintf(d->err, "cannot open %s", localfile);
        return FALSE;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);

    sprintf(extra, "X-OC-MTime: %lu\r\n", (unsigned long)mtime);

    if (nshttp_put_file(d->http, url, extra, fh, (ULONG)size, &resp) != 0 ||
        (resp.status != 200 && resp.status != 201 && resp.status != 204))
    {
        Close(fh);
        if (resp.status > 0)
            sprintf(d->err, "PUT %s: HTTP %ld", path, (long)resp.status);
        else
            sprintf(d->err, "PUT %s: %s", path, nshttp_error(d->http));
        return FALSE;
    }
    Close(fh);

    if (etag_out)
    {
        strcpy(etag_out, resp.etag);
        if (!etag_out[0])
        {
            /* server didn't hand the etag back: ask for it */
            nsdav_entry e;
            if (nsdav_stat(d, path, &e))
                strcpy(etag_out, e.etag);
        }
    }
    return TRUE;
}

BOOL nsdav_mkcol(nsdav *d, const char *path)
{
    char url[NSDAV_MAXPATH * 3];
    nshttp_response resp;

    d->err[0] = 0;
    build_url(d, path, url, sizeof(url));
    if (nshttp_request(d->http, "MKCOL", url, NULL, NULL, 0, &resp) != 0)
    {
        sprintf(d->err, "MKCOL %s: %s", path, nshttp_error(d->http));
        return FALSE;
    }
    if (resp.status != 201 && resp.status != 405)   /* 405 = exists */
    {
        sprintf(d->err, "MKCOL %s: HTTP %ld", path, (long)resp.status);
        return FALSE;
    }
    return TRUE;
}

BOOL nsdav_delete(nsdav *d, const char *path)
{
    char url[NSDAV_MAXPATH * 3];
    nshttp_response resp;

    d->err[0] = 0;
    build_url(d, path, url, sizeof(url));
    if (nshttp_request(d->http, "DELETE", url, NULL, NULL, 0, &resp) != 0)
    {
        sprintf(d->err, "DELETE %s: %s", path, nshttp_error(d->http));
        return FALSE;
    }
    if (resp.status != 204 && resp.status != 404)
    {
        sprintf(d->err, "DELETE %s: HTTP %ld", path, (long)resp.status);
        return FALSE;
    }
    return TRUE;
}
