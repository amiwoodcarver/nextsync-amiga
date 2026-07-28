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

/* the rest of the line, trimmed: passwords, paths and folder names may
 * all contain spaces */
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

/* join a base drawer and a name the way AmigaDOS wants it */
static void join(const char *base, const char *name, char *dst, LONG max)
{
    LONG n = strlen(base);

    if (n >= max)
        n = max - 1;
    memcpy(dst, base, n);
    dst[n] = 0;
    if (n && base[n - 1] != ':' && base[n - 1] != '/')
        strncat(dst, "/", max - strlen(dst) - 1);
    strncat(dst, name, max - strlen(dst) - 1);
}

void nsconf_defaults(nsconf *c)
{
    memset(c, 0, sizeof(*c));
    c->port = 443;
    strcpy(c->local, "DH0:Nextcloud");
}

void nsconf_rebuild(nsconf *c)
{
    UWORD i;

    c->npairs = 0;

    for (i = 0; i < c->nfolders && c->npairs < NSCONF_MAXPAIRS; i++)
    {
        if (!c->folder[i][0] || !c->local[0])
            continue;
        sprintf(c->pair[c->npairs].remote, "/%s", c->folder[i]);
        join(c->local, c->folder[i], c->pair[c->npairs].local,
             sizeof(c->pair[0].local));
        c->npairs++;
    }

    for (i = 0; i < c->nextra && c->npairs < NSCONF_MAXPAIRS; i++)
    {
        c->pair[c->npairs] = c->extra[i];
        c->npairs++;
    }
}

BOOL nsconf_complete(const nsconf *c)
{
    return (BOOL)(c->server[0] && c->user[0] && c->npairs > 0);
}

BOOL nsconf_has_folder(const nsconf *c, const char *name)
{
    UWORD i;

    for (i = 0; i < c->nfolders; i++)
        if (!strcmp(c->folder[i], name))
            return TRUE;
    return FALSE;
}

void nsconf_add_folder(nsconf *c, const char *name)
{
    if (!name || !name[0] || c->nfolders >= NSCONF_MAXFOLDERS)
        return;
    if (nsconf_has_folder(c, name))
        return;
    strncpy(c->folder[c->nfolders], name, sizeof(c->folder[0]) - 1);
    c->folder[c->nfolders][sizeof(c->folder[0]) - 1] = 0;
    c->nfolders++;
    nsconf_rebuild(c);
}

void nsconf_del_folder(nsconf *c, const char *name)
{
    UWORD i, j;

    for (i = 0; i < c->nfolders; i++)
        if (!strcmp(c->folder[i], name))
        {
            for (j = i; j + 1 < c->nfolders; j++)
                strcpy(c->folder[j], c->folder[j + 1]);
            c->nfolders--;
            nsconf_rebuild(c);
            return;
        }
}

BOOL nsconf_load(nsconf *c, const char *filename)
{
    BPTR fh;
    char line[600];

    nsconf_defaults(c);
    c->local[0] = 0;             /* only defaulted for a fresh setup */

    fh = Open((STRPTR)filename, MODE_OLDFILE);
    if (!fh)
    {
        nsconf_defaults(c);
        return FALSE;
    }

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
        else if (!strcmp(key, "local"))
            take_rest(&p, c->local, sizeof(c->local));
        else if (!strcmp(key, "folder") && c->nfolders < NSCONF_MAXFOLDERS)
        {
            take_rest(&p, c->folder[c->nfolders], sizeof(c->folder[0]));
            if (c->folder[c->nfolders][0])
                c->nfolders++;
        }
        else if (!strcmp(key, "pair") && c->nextra < NSCONF_MAXEXTRA)
        {
            /* 1.0 syntax: two space separated paths */
            take_word(&p, c->extra[c->nextra].remote,
                      sizeof(c->extra[0].remote));
            take_word(&p, c->extra[c->nextra].local,
                      sizeof(c->extra[0].local));
            if (c->extra[c->nextra].remote[0] && c->extra[c->nextra].local[0])
                c->nextra++;
        }
    }
    Close(fh);

    if (!c->local[0])
        strcpy(c->local, "DH0:Nextcloud");

    nsconf_rebuild(c);
    return TRUE;
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
                  "server %s\nport %ld\nuser %s\npass %s\n"
                  "\n# where the synced folders live\nlocal %s\n",
            c->server, (long)c->port, c->user, c->pass, c->local);
    Write(fh, line, strlen(line));

    if (c->nfolders)
    {
        sprintf(line, "\n# folders picked in Preferences\n");
        Write(fh, line, strlen(line));
        for (i = 0; i < c->nfolders; i++)
        {
            sprintf(line, "folder %s\n", c->folder[i]);
            Write(fh, line, strlen(line));
        }
    }

    if (c->nextra)
    {
        sprintf(line, "\n# explicit pairs, kept as written\n");
        Write(fh, line, strlen(line));
        for (i = 0; i < c->nextra; i++)
        {
            sprintf(line, "pair %s %s\n", c->extra[i].remote,
                    c->extra[i].local);
            Write(fh, line, strlen(line));
        }
    }

    Close(fh);
    return TRUE;
}
