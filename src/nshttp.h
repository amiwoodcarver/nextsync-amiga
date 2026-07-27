/*
 * nshttp.h -- HTTP/1.1 client over bsdsocket.library with TLS via AmiSSL.
 *
 * One nshttp handle = one server. Connections are kept alive across
 * requests and transparently reopened when the server closes them.
 */

#ifndef NSHTTP_H
#define NSHTTP_H

#include <exec/types.h>
#include <dos/dos.h>

typedef struct nshttp nshttp;

/* progress callback: phase "rx"/"tx", done/total bytes (total 0 = unknown).
 * Return FALSE to abort the transfer. */
typedef BOOL (*nshttp_progress_fn)(void *user, const char *phase,
                                   ULONG done, ULONG total);

typedef struct nshttp_response
{
    LONG   status;              /* 207, 404, ...; -1 on transport error   */
    char   etag[128];           /* ETag / OC-ETag header, quotes stripped */
    ULONG  content_length;      /* 0 if unknown                           */
    char  *body;                /* set by nshttp_request, owner: handle   */
    LONG   body_len;
} nshttp_response;

/* host: "cloud.example.com". port 443 = TLS. user/pass for Basic auth. */
nshttp *nshttp_open(const char *host, UWORD port, BOOL tls,
                    const char *user, const char *pass, char *errbuf);
void    nshttp_close(nshttp *h);

/*
 * Releases AmiSSL and the socket library. Call once, immediately before
 * the program exits. This is deliberately NOT done in nshttp_close():
 * AmiSSL's CloseAmiSSL() on OS3 (verified on 5.15 and 5.27) leaves the
 * heap in a state that crashes a process that keeps running, so the only
 * safe place for it is right before exit.
 */
void    nshttp_shutdown(void);

void    nshttp_set_progress(nshttp *h, nshttp_progress_fn fn, void *user);

/*
 * In-memory request. extra_headers may be NULL or "Header: v\r\n..." lines.
 * The response body is buffered in the handle, valid until the next call.
 * Returns 0 on transport success (check resp->status), -1 on failure.
 */
LONG nshttp_request(nshttp *h, const char *method, const char *path,
                    const char *extra_headers,
                    const char *req_body, LONG req_len,
                    nshttp_response *resp);

/* Streaming GET straight into an open file. */
LONG nshttp_get_file(nshttp *h, const char *path, BPTR fh,
                     nshttp_response *resp);

/* Streaming PUT from an open file (size bytes). */
LONG nshttp_put_file(nshttp *h, const char *path, const char *extra_headers,
                     BPTR fh, ULONG size, nshttp_response *resp);

/* URL-encode one path segment worth of text into dst (dstlen incl NUL). */
void nshttp_urlencode(const char *src, char *dst, LONG dstlen);

const char *nshttp_error(nshttp *h);

#endif
