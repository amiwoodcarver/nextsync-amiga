/*
 * nsconf.c -- read/write the plain text configuration.
 */

#include <exec/types.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#include "nsconf.h"

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static void take_word(char **pp, char *dst, LONG max)
{
    char *p = skip_ws(*pp);
    LONG  n = 0;

    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
    {
        if (n < max - 1)
            dst[n++] = *p;
        p++;
    }
    dst[n] = 0;
    *pp = p;
}

/* the rest of the line, trimmed (passwords may contain spaces) */
static void take_rest(char **pp, char *dst, LONG max)
{
    char *p = skip_ws(*pp);
    LONG  n = strlen(p);

    while (n > 0 && (p[n-1] == '\n' || p[n-1] == '\r' ||
                     p[n-1] == ' ' || p[n-1] == '\t'))
        n--;
    if (n >= max)
        n = max - 1;
    memcpy(dst, p, n);
    dst[n] = 0;
    *pp += strlen(*pp);
}

BOOL nsconf_load(nsconf *c, const char *filename)
{
    BPTR fh;
    char line[600];

    memset(c, 0, sizeof(*c));
    c->port = 443;

    fh = Open((STRPTR)filename, MODE_OLDFILE);
    if (!fh)
        return FALSE;

    while (FGets(fh, (STRPTR)line, sizeof(line)))
    {
        char key[32];
        char *p = line;

        p = skip_ws(p);
        if (*p == '#' || *p == ';' || *p == '\n' || !*p)
            continue;
        take_word(&p, key, sizeof(key));

        if (!strcmp(key, "server"))
            take_word(&p, c->server, sizeof(c->server));
        else if (!strcmp(key, "port"))
        {
            char num[16];
            const char *s = num;
            UWORD v = 0;
            take_word(&p, num, sizeof(num));
            while (*s >= '0' && *s <= '9')
                v = v * 10 + (*s++ - '0');
            if (v)
                c->port = v;
        }
        else if (!strcmp(key, "user"))
            take_word(&p, c->user, sizeof(c->user));
        else if (!strcmp(key, "pass"))
            take_rest(&p, c->pass, sizeof(c->pass));
        else if (!strcmp(key, "pair") && c->npairs < NSCONF_MAXPAIRS)
        {
            take_word(&p, c->pair[c->npairs].remote,
                      sizeof(c->pair[0].remote));
            take_word(&p, c->pair[c->npairs].local,
                      sizeof(c->pair[0].local));
            if (c->pair[c->npairs].remote[0] && c->pair[c->npairs].local[0])
                c->npairs++;
        }
    }
    Close(fh);
    return c->server[0] && c->user[0];
}

BOOL nsconf_save(nsconf *c, const char *filename)
{
    BPTR fh;
    char line[600];
    UWORD i;

    fh = Open((STRPTR)filename, MODE_NEWFILE);
    if (!fh)
        return FALSE;

    sprintf(line, "# NextSync configuration\n"
                  "server %s\nport %ld\nuser %s\npass %s\n",
            c->server, (long)c->port, c->user, c->pass);
    Write(fh, line, strlen(line));
    for (i = 0; i < c->npairs; i++)
    {
        sprintf(line, "pair %s %s\n", c->pair[i].remote, c->pair[i].local);
        Write(fh, line, strlen(line));
    }
    Close(fh);
    return TRUE;
}
