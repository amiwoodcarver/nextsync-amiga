/*
 * nsxml.c -- minimal SAX parser for WebDAV responses. Portable C, no OS
 * calls, so it can be unit tested on the host.
 */

#include <string.h>

#include "nsxml.h"

#define MAXDEPTH 32
#define MAXTAG   64
#define MAXTEXT  1024

struct frame
{
    char tag[MAXTAG];
    char text[MAXTEXT];
    int  textlen;
};

/* strip namespace prefix and copy tag name */
static void copy_tag(char *dst, const char *src, int len)
{
    const char *colon = NULL;
    int i;

    for (i = 0; i < len; i++)
        if (src[i] == ':')
            colon = src + i + 1;
    if (colon)
    {
        len -= (int)(colon - src);
        src = colon;
    }
    if (len >= MAXTAG)
        len = MAXTAG - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static int decode_entity(const char *s, long avail, char *out)
{
    long i;

    if (avail >= 4 && !strncmp(s, "amp;", 4))  { *out = '&';  return 4; }
    if (avail >= 3 && !strncmp(s, "lt;", 3))   { *out = '<';  return 3; }
    if (avail >= 3 && !strncmp(s, "gt;", 3))   { *out = '>';  return 3; }
    if (avail >= 5 && !strncmp(s, "quot;", 5)) { *out = '"';  return 5; }
    if (avail >= 5 && !strncmp(s, "apos;", 5)) { *out = '\''; return 5; }
    if (avail >= 2 && s[0] == '#')
    {
        long v = 0;
        for (i = 1; i < avail && s[i] != ';' && i < 8; i++)
        {
            if (s[i] < '0' || s[i] > '9')
                return 0;
            v = v * 10 + (s[i] - '0');
        }
        if (i < avail && s[i] == ';')
        {
            *out = (char)(v > 255 ? '?' : v);
            return (int)(i + 1);
        }
    }
    return 0;
}

int nsxml_parse(const char *doc, long len,
                nsxml_start_fn start, nsxml_end_fn end, void *user)
{
    struct frame stack[MAXDEPTH];
    int depth = 0;
    long p = 0;

    while (p < len)
    {
        if (doc[p] == '<')
        {
            long q = p + 1;
            int closing = 0, selfclose = 0;
            long name0, name1;

            if (q < len && doc[q] == '?')          /* <?xml ... ?> */
            {
                while (q < len && doc[q] != '>')
                    q++;
                p = q + 1;
                continue;
            }
            if (q < len && doc[q] == '!')          /* comments, doctype */
            {
                while (q < len && doc[q] != '>')
                    q++;
                p = q + 1;
                continue;
            }
            if (q < len && doc[q] == '/')
            {
                closing = 1;
                q++;
            }
            name0 = q;
            while (q < len && doc[q] != '>' && doc[q] != ' ' &&
                   doc[q] != '\t' && doc[q] != '\r' && doc[q] != '\n' &&
                   doc[q] != '/')
                q++;
            name1 = q;
            while (q < len && doc[q] != '>')
            {
                if (doc[q] == '/' && q + 1 < len && doc[q + 1] == '>')
                    selfclose = 1;
                q++;
            }
            if (q >= len)
                return -1;

            if (closing)
            {
                if (depth <= 0)
                    return -1;
                depth--;
                stack[depth].text[stack[depth].textlen] = 0;
                if (end)
                {
                    /* trim */
                    char *t = stack[depth].text;
                    int   n = stack[depth].textlen;
                    while (n > 0 && (t[n-1]==' '||t[n-1]=='\n'||t[n-1]=='\r'||t[n-1]=='\t'))
                        t[--n] = 0;
                    while (*t==' '||*t=='\n'||*t=='\r'||*t=='\t')
                        t++;
                    end(user, stack[depth].tag, t);
                }
            }
            else
            {
                if (depth >= MAXDEPTH)
                    return -1;
                copy_tag(stack[depth].tag, doc + name0, (int)(name1 - name0));
                stack[depth].textlen = 0;
                stack[depth].text[0] = 0;
                if (start)
                    start(user, stack[depth].tag);
                depth++;
                if (selfclose)
                {
                    depth--;
                    if (end)
                        end(user, stack[depth].tag, "");
                }
            }
            p = q + 1;
        }
        else
        {
            /* character data goes to the innermost open element */
            if (depth > 0)
            {
                struct frame *f = &stack[depth - 1];
                char c = doc[p];

                if (c == '&')
                {
                    char dec;
                    int used = decode_entity(doc + p + 1, len - p - 1, &dec);
                    if (used > 0)
                    {
                        c = dec;
                        p += used;
                    }
                }
                if (f->textlen < MAXTEXT - 1)
                    f->text[f->textlen++] = c;
            }
            p++;
        }
    }
    return depth == 0 ? 0 : -1;
}
