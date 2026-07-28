/*
 * nshttp.c -- HTTP/1.1 over bsdsocket.library, TLS via AmiSSL v5.
 *
 * The handle owns its library bases so an application only links this file
 * and calls nshttp_open(). Connections persist across requests; a stale
 * keep-alive connection is detected on write failure and reopened once.
 */

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>

#define __NOLIBBASE__
#undef __NOLIBBASE__

#include <amissl/amissl.h>
#include <libraries/amisslmaster.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nsutil.h"
#include "nshttp.h"

#define RXBUF   16384
#define HDRMAX  8192

struct Library *AmiSSLMasterBase, *AmiSSLBase, *AmiSSLExtBase;
struct Library *SocketBase, *UtilityBase;

static LONG nshttp_users = 0;      /* how many handles share the bases */
static BOOL amissl_up = FALSE;

struct nshttp
{
    char  host[128];
    UWORD port;
    BOOL  tls;
    char  auth[256];               /* "Basic xxxx" or empty            */

    ULONG addr;                    /* resolved ipv4, network order     */
    LONG  sock;
    SSL_CTX *ctx;
    SSL  *ssl;
    BOOL  connected;

    /* receive buffering */
    UBYTE rx[RXBUF];
    LONG  rx_head, rx_tail;

    char *body;                    /* in-memory response body          */
    LONG  body_cap;

    nshttp_progress_fn progress;
    void *progress_user;

    char  err[256];
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static LONG str2long(const char *s)
{
    LONG v = 0;
    int neg = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static void b64(const UBYTE *src, LONG len, char *dst)
{
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    LONG i;

    for (i = 0; i < len; i += 3)
    {
        ULONG v = src[i] << 16;
        if (i + 1 < len) v |= src[i + 1] << 8;
        if (i + 2 < len) v |= src[i + 2];
        *dst++ = t[(v >> 18) & 63];
        *dst++ = t[(v >> 12) & 63];
        *dst++ = (i + 1 < len) ? t[(v >> 6) & 63] : '=';
        *dst++ = (i + 2 < len) ? t[v & 63] : '=';
    }
    *dst = 0;
}

void nshttp_urlencode(const char *src, char *dst, LONG dstlen)
{
    static const char hex[] = "0123456789ABCDEF";
    LONG o = 0;

    while (*src && o < dstlen - 4)
    {
        UBYTE c = (UBYTE)*src++;

        /* unreserved plus '/' which separates segments */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
            dst[o++] = c;
        else
        {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 15];
        }
    }
    dst[o] = 0;
}

/* debug trace, enabled when out/http.log exists at startup */
static BOOL dbg_on = FALSE;
static BOOL dbg_checked = FALSE;

static void dbg(const char *fmt, long a, long b, long c)
{
    BPTR fh;
    char buf[256];

    if (!dbg_checked)
    {
        fh = Open("out/http.log", MODE_OLDFILE);
        if (fh)
        {
            dbg_on = TRUE;
            Close(fh);
        }
        dbg_checked = TRUE;
    }
    if (!dbg_on)
        return;
    fh = Open("out/http.log", MODE_READWRITE);
    if (!fh)
        return;
    Seek(fh, 0, OFFSET_END);
    sprintf(buf, fmt, a, b, c);
    Write(fh, buf, strlen(buf));
    Write(fh, "\n", 1);
    Close(fh);
}

const char *nshttp_error(nshttp *h)
{
    return h ? h->err : "no handle";
}

/* ------------------------------------------------------------------ */
/* library bootstrap                                                   */
/* ------------------------------------------------------------------ */

static BOOL libs_up(BOOL need_tls, char *errbuf)
{
    if (nshttp_users++ > 0)
        return TRUE;
    if (SocketBase)
        return TRUE;               /* bases survived a previous handle */

    UtilityBase = OpenLibrary("utility.library", 37);
    SocketBase  = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase)
    {
        strcpy(errbuf, "no bsdsocket.library (is a TCP/IP stack running?)");
        return FALSE;
    }

    if (need_tls)
    {
        AmiSSLMasterBase = OpenLibrary("amisslmaster.library",
                                       AMISSLMASTER_MIN_VERSION);
        if (!AmiSSLMasterBase)
        {
            strcpy(errbuf, "no amisslmaster.library (install AmiSSL v5)");
            return FALSE;
        }
        if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                           AmiSSL_UsesOpenSSLStructs, FALSE,
                           AmiSSL_GetAmiSSLBase,      (ULONG)&AmiSSLBase,
                           AmiSSL_GetAmiSSLExtBase,   (ULONG)&AmiSSLExtBase,
                           AmiSSL_SocketBase,         (ULONG)SocketBase,
                           AmiSSL_ErrNoPtr,           (ULONG)&errno,
                           TAG_DONE) != 0)
        {
            strcpy(errbuf, "OpenAmiSSL failed (AmiSSL v5 installed? "
                           "mathieee libraries present?)");
            return FALSE;
        }
        amissl_up = TRUE;
        OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT, NULL);
    }
    return TRUE;
}

/*
 * The library bases stay open until nshttp_shutdown(): tearing AmiSSL
 * down mid-run corrupts the process (see nshttp.h), and keeping it open
 * also means later connections skip the expensive re-initialisation.
 */
static void libs_down(void)
{
    if (nshttp_users > 0)
        nshttp_users--;
}

void nshttp_shutdown(void)
{
    if (amissl_up)
    {
        CloseAmiSSL();
        amissl_up = FALSE;
    }
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
    if (SocketBase)       { CloseLibrary(SocketBase);       SocketBase = NULL; }
    if (UtilityBase)      { CloseLibrary(UtilityBase);      UtilityBase = NULL; }
}

/* ------------------------------------------------------------------ */
/* connection                                                          */
/* ------------------------------------------------------------------ */

static void conn_close(nshttp *h)
{
    if (h->ssl)
    {
        SSL_shutdown(h->ssl);
        SSL_free(h->ssl);
        h->ssl = NULL;
    }
    if (h->sock >= 0)
    {
        CloseSocket(h->sock);
        h->sock = -1;
    }
    h->connected = FALSE;
    h->rx_head = h->rx_tail = 0;
}

static BOOL conn_open(nshttp *h)
{
    struct sockaddr_in sa;

    conn_close(h);

    if (!h->addr)
    {
        struct hostent *he = gethostbyname(h->host);
        if (!he)
        {
            sprintf(h->err, "cannot resolve %s", h->host);
            return FALSE;
        }
        memcpy(&h->addr, he->h_addr, 4);
    }

    h->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (h->sock < 0)
    {
        strcpy(h->err, "socket() failed");
        return FALSE;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(h->port);
    memcpy(&sa.sin_addr, &h->addr, 4);

    if (connect(h->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        sprintf(h->err, "cannot connect to %s:%ld", h->host, (long)h->port);
        conn_close(h);
        return FALSE;
    }

    if (h->tls)
    {
        h->ssl = SSL_new(h->ctx);
        if (!h->ssl)
        {
            strcpy(h->err, "SSL_new failed");
            conn_close(h);
            return FALSE;
        }
        SSL_set_tlsext_host_name(h->ssl, h->host);
        SSL_set_fd(h->ssl, h->sock);
        if (SSL_connect(h->ssl) != 1)
        {
            LONG vr = SSL_get_verify_result(h->ssl);
            if (vr != X509_V_OK)
                sprintf(h->err, "TLS certificate verify failed (%ld)", (long)vr);
            else
                strcpy(h->err, "TLS handshake failed");
            conn_close(h);
            return FALSE;
        }
    }
    h->connected = TRUE;
    dbg("conn_open ok tls=%ld sock=%ld", h->tls, h->sock, 0);
    return TRUE;
}

static LONG raw_write(nshttp *h, const void *buf, LONG len)
{
    LONG r;

    if (h->tls)
    {
        r = SSL_write(h->ssl, (void *)buf, len);
        if (r <= 0)
            dbg("SSL_write=%ld len=%ld sslerr=%ld", r, len,
                SSL_get_error(h->ssl, r));
    }
    else
    {
        r = send(h->sock, (void *)buf, len, 0);
        if (r <= 0)
            dbg("send=%ld len=%ld errno=%ld", r, len, errno);
    }
    return r;
}

static LONG raw_read(nshttp *h, void *buf, LONG len)
{
    if (h->tls)
        return SSL_read(h->ssl, buf, len);
    return recv(h->sock, buf, len, 0);
}

static BOOL send_all(nshttp *h, const void *buf, LONG len)
{
    const char *p = (const char *)buf;

    while (len > 0)
    {
        LONG n = raw_write(h, p, len);
        if (n <= 0)
            return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

/* buffered single byte / line reads */
static LONG rx_byte(nshttp *h)
{
    if (h->rx_head >= h->rx_tail)
    {
        LONG n = raw_read(h, h->rx, RXBUF);
        if (n <= 0)
            return -1;
        h->rx_head = 0;
        h->rx_tail = n;
    }
    return h->rx[h->rx_head++];
}

static LONG rx_block(nshttp *h, UBYTE *dst, LONG want)
{
    LONG got = 0;

    /* drain the lookahead buffer first */
    while (got < want && h->rx_head < h->rx_tail)
        dst[got++] = h->rx[h->rx_head++];
    while (got < want)
    {
        LONG n = raw_read(h, dst + got, want - got);
        if (n <= 0)
            break;
        got += n;
    }
    return got;
}

static BOOL rx_line(nshttp *h, char *dst, LONG max)
{
    LONG o = 0, c;

    for (;;)
    {
        c = rx_byte(h);
        if (c < 0)
            return FALSE;
        if (c == '\n')
            break;
        if (c != '\r' && o < max - 1)
            dst[o++] = (char)c;
    }
    dst[o] = 0;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

nshttp *nshttp_open(const char *host, UWORD port, BOOL tls,
                    const char *user, const char *pass, char *errbuf)
{
    nshttp *h;

    errbuf[0] = 0;
    if (!libs_up(tls, errbuf))
    {
        libs_down();
        return NULL;
    }

    h = AllocVec(sizeof(nshttp), MEMF_CLEAR | MEMF_PUBLIC);
    if (!h)
    {
        strcpy(errbuf, "out of memory");
        libs_down();
        return NULL;
    }
    strncpy(h->host, host, sizeof(h->host) - 1);
    h->port = port;
    h->tls  = tls;
    h->sock = -1;

    if (user && *user)
    {
        char plain[192], enc[256];
        sprintf(plain, "%s:%s", user, pass ? pass : "");
        b64((UBYTE *)plain, strlen(plain), enc);
        sprintf(h->auth, "Basic %s", enc);
    }

    if (tls)
    {
        h->ctx = SSL_CTX_new(TLS_client_method());
        if (!h->ctx)
        {
            strcpy(errbuf, "SSL_CTX_new failed");
            nshttp_close(h);
            return NULL;
        }
        /* AmiSSL:Certs is the hashed CA directory of the AmiSSL install */
        if (SSL_CTX_load_verify_locations(h->ctx, NULL, "AmiSSL:Certs") == 1)
            SSL_CTX_set_verify(h->ctx, SSL_VERIFY_PEER, NULL);
        else
            SSL_CTX_set_verify(h->ctx, SSL_VERIFY_NONE, NULL);
        SSL_CTX_set_mode(h->ctx, SSL_MODE_AUTO_RETRY);
    }
    return h;
}

void nshttp_close(nshttp *h)
{
    if (!h)
        return;
    conn_close(h);
    if (h->ctx)
        SSL_CTX_free(h->ctx);
    if (h->body)
        FreeVec(h->body);
    FreeVec(h);
    libs_down();
}

void nshttp_set_progress(nshttp *h, nshttp_progress_fn fn, void *user)
{
    h->progress = fn;
    h->progress_user = user;
}

/* ------------------------------------------------------------------ */
/* request core                                                        */
/* ------------------------------------------------------------------ */

static BOOL body_reserve(nshttp *h, LONG need)
{
    if (need <= h->body_cap)
        return TRUE;
    {
        LONG ncap = h->body_cap ? h->body_cap : 65536;
        char *nb;

        while (ncap < need)
            ncap *= 2;
        nb = AllocVec(ncap, MEMF_PUBLIC);
        if (!nb)
            return FALSE;
        if (h->body)
        {
            memcpy(nb, h->body, h->body_cap);
            FreeVec(h->body);
        }
        h->body = nb;
        h->body_cap = ncap;
    }
    return TRUE;
}

static BOOL send_request_head(nshttp *h, const char *method, const char *path,
                              const char *extra, LONG content_len)
{
    char head[HDRMAX];
    LONG o;

    o = sprintf(head,
                "%s %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: NextSync/1.1 (AmigaOS 3; 68k)\r\n",
                method, path, h->host);
    if (h->auth[0])
        o += sprintf(head + o, "Authorization: %s\r\n", h->auth);
    if (content_len >= 0)
        o += sprintf(head + o, "Content-Length: %ld\r\n", (long)content_len);
    if (extra && *extra)
        o += sprintf(head + o, "%s", extra);
    o += sprintf(head + o, "\r\n");

    return send_all(h, head, o);
}

/*
 * Reads the status line and headers. Fills resp fields, returns body
 * framing via *chunked / *clen (-1 = read until close), and whether the
 * server will close the connection.
 */
static BOOL read_head(nshttp *h, nshttp_response *resp,
                      BOOL *chunked, LONG *clen, BOOL *closing)
{
    char line[1024];

    *chunked = FALSE;
    *clen = -1;
    *closing = FALSE;
    resp->status = -1;
    resp->etag[0] = 0;
    resp->content_length = 0;

    if (!rx_line(h, line, sizeof(line)))
        return FALSE;
    if (strncmp(line, "HTTP/1.", 7) || strlen(line) < 12)
    {
        sprintf(h->err, "bad status line");
        return FALSE;
    }
    if (line[5] == '1' && line[7] == '0')
        *closing = TRUE;                       /* HTTP/1.0 default */
    resp->status = str2long(line + 8);

    for (;;)
    {
        char *v;

        if (!rx_line(h, line, sizeof(line)))
            return FALSE;
        if (!line[0])
            break;
        v = strchr(line, ':');
        if (!v)
            continue;
        *v++ = 0;
        while (*v == ' ')
            v++;

        if (!ns_stricmp(line, "Content-Length"))
        {
            *clen = str2long(v);
            resp->content_length = (ULONG)*clen;
        }
        else if (!ns_stricmp(line, "Transfer-Encoding"))
        {
            if (strstr(v, "chunked") || strstr(v, "Chunked"))
                *chunked = TRUE;
        }
        else if (!ns_stricmp(line, "Connection"))
        {
            if (!ns_stricmp(v, "close"))
                *closing = TRUE;
            else if (!ns_stricmp(v, "keep-alive"))
                *closing = FALSE;
        }
        else if (!ns_stricmp(line, "ETag") ||
                 !ns_stricmp(line, "OC-ETag"))
        {
            char *e = resp->etag;
            LONG  n = 0;
            while (*v && n < (LONG)sizeof(resp->etag) - 1)
            {
                if (*v != '"')
                    e[n++] = *v;
                v++;
            }
            e[n] = 0;
        }
    }
    return TRUE;
}

/* sink for body data: either memory or a file */
struct sink
{
    nshttp *h;
    BPTR    fh;
    LONG    written;
    BOOL    failed;
    ULONG   total;
};

static void sink_put(struct sink *s, const UBYTE *data, LONG len)
{
    if (s->failed || len <= 0)
        return;
    if (s->fh)
    {
        if (Write(s->fh, (APTR)data, len) != len)
            s->failed = TRUE;
    }
    else
    {
        if (!body_reserve(s->h, s->written + len + 1))
        {
            s->failed = TRUE;
            return;
        }
        memcpy(s->h->body + s->written, data, len);
    }
    s->written += len;

    if (s->h->progress)
        if (!s->h->progress(s->h->progress_user, "rx",
                            (ULONG)s->written, s->total))
            s->failed = TRUE;
}

static BOOL read_body(nshttp *h, BOOL chunked, LONG clen, struct sink *s)
{
    UBYTE buf[RXBUF];

    if (chunked)
    {
        char line[64];

        for (;;)
        {
            LONG chunk, got;

            if (!rx_line(h, line, sizeof(line)))
                return FALSE;
            chunk = 0;
            {
                char *p = line;
                while (*p)
                {
                    char c = *p++;
                    LONG d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else break;
                    chunk = chunk * 16 + d;
                }
            }
            if (chunk == 0)
            {
                /* trailing headers until blank line */
                while (rx_line(h, line, sizeof(line)) && line[0])
                    ;
                break;
            }
            while (chunk > 0)
            {
                LONG want = chunk > (LONG)sizeof(buf) ? (LONG)sizeof(buf) : chunk;
                got = rx_block(h, buf, want);
                if (got <= 0)
                    return FALSE;
                sink_put(s, buf, got);
                chunk -= got;
            }
            if (!rx_line(h, line, sizeof(line)))    /* CRLF after chunk */
                return FALSE;
            if (s->failed)
                return FALSE;
        }
    }
    else if (clen >= 0)
    {
        LONG left = clen;

        while (left > 0)
        {
            LONG want = left > (LONG)sizeof(buf) ? (LONG)sizeof(buf) : left;
            LONG got = rx_block(h, buf, want);
            if (got <= 0)
                return FALSE;
            sink_put(s, buf, got);
            left -= got;
            if (s->failed)
                return FALSE;
        }
    }
    else
    {
        /* read until the server closes */
        for (;;)
        {
            LONG got = rx_block(h, buf, sizeof(buf));
            if (got <= 0)
                break;
            sink_put(s, buf, got);
            if (s->failed)
                return FALSE;
        }
    }
    return !s->failed;
}

/*
 * The common request path. body_fh -> streaming download, else memory.
 * req_fh -> streaming upload of req_size bytes, else req_body/req_len.
 */
static LONG do_request(nshttp *h, const char *method, const char *path,
                       const char *extra, const char *req_body, LONG req_len,
                       BPTR req_fh, ULONG req_size,
                       BPTR body_fh, nshttp_response *resp)
{
    int attempt;

    for (attempt = 0; attempt < 2; attempt++)
    {
        BOOL fresh = FALSE;
        BOOL chunked, closing;
        LONG clen;
        struct sink s;

        if (!h->connected)
        {
            if (!conn_open(h))
                return -1;
            fresh = TRUE;
        }
        h->rx_head = h->rx_tail = 0;

        {
            LONG content_len = req_fh ? (LONG)req_size
                                      : (req_body ? req_len : -1);
            if (!strcmp(method, "PUT") || !strcmp(method, "PROPFIND") ||
                !strcmp(method, "MKCOL") || !strcmp(method, "DELETE") ||
                !strcmp(method, "MOVE"))
                if (content_len < 0)
                    content_len = 0;

            if (!send_request_head(h, method, path, extra, content_len))
            {
                sprintf(h->err, "send failed (%s)", method);
                goto stale;
            }

            if (req_body && req_len > 0)
                if (!send_all(h, req_body, req_len))
                    goto stale;

            if (req_fh)
            {
                UBYTE buf[RXBUF];
                ULONG left = req_size, done = 0;

                while (left > 0)
                {
                    LONG want = left > sizeof(buf) ? (LONG)sizeof(buf) : (LONG)left;
                    LONG got = Read(req_fh, buf, want);
                    if (got <= 0)
                    {
                        strcpy(h->err, "local read error during upload");
                        conn_close(h);
                        return -1;
                    }
                    if (!send_all(h, buf, got))
                        goto stale;
                    left -= got;
                    done += got;
                    if (h->progress)
                        if (!h->progress(h->progress_user, "tx", done, req_size))
                        {
                            strcpy(h->err, "aborted");
                            conn_close(h);
                            return -1;
                        }
                }
            }
        }

        if (!read_head(h, resp, &chunked, &clen, &closing))
        {
            if (!h->err[0])
                sprintf(h->err, "no response (%s, tls_err=%ld)",
                        method, h->tls && h->ssl
                            ? (long)SSL_get_error(h->ssl, 0) : 0L);
            goto stale;
        }

        /* 204/304 and HEAD never carry a body */
        memset(&s, 0, sizeof(s));
        s.h = h;
        s.fh = body_fh;
        s.total = (clen > 0) ? (ULONG)clen : 0;

        if (resp->status == 204 || resp->status == 304)
            clen = 0;

        if (!read_body(h, chunked, clen, &s))
        {
            conn_close(h);
            if (s.failed)
            {
                strcpy(h->err, "transfer failed");
                return -1;
            }
            /* server closed mid-body on a reused connection: retry once */
            if (!fresh && attempt == 0)
                continue;
            strcpy(h->err, "connection lost mid-transfer");
            return -1;
        }

        if (!body_fh)
        {
            if (!body_reserve(h, s.written + 1))
            {
                strcpy(h->err, "out of memory");
                return -1;
            }
            h->body[s.written] = 0;
            resp->body = h->body;
            resp->body_len = s.written;
        }
        else
        {
            resp->body = NULL;
            resp->body_len = s.written;
        }

        if (closing)
            conn_close(h);
        return 0;

    stale:
        conn_close(h);
        if (fresh || attempt > 0)
        {
            if (!h->err[0])
                strcpy(h->err, "connection failed");
            return -1;
        }
        /* fall through: reopen and retry once */
    }
    return -1;
}

LONG nshttp_request(nshttp *h, const char *method, const char *path,
                    const char *extra_headers,
                    const char *req_body, LONG req_len,
                    nshttp_response *resp)
{
    return do_request(h, method, path, extra_headers,
                      req_body, req_len, 0, 0, 0, resp);
}

LONG nshttp_get_file(nshttp *h, const char *path, BPTR fh,
                     nshttp_response *resp)
{
    return do_request(h, "GET", path, NULL, NULL, 0, 0, 0, fh, resp);
}

LONG nshttp_put_file(nshttp *h, const char *path, const char *extra_headers,
                     BPTR fh, ULONG size, nshttp_response *resp)
{
    return do_request(h, "PUT", path, extra_headers,
                      NULL, 0, fh, size, 0, resp);
}
