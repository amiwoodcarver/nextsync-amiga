/* nsutil.h -- tiny helpers shared by the NextSync modules */
#ifndef NSUTIL_H
#define NSUTIL_H

/* ASCII case insensitive compare; no library base needed */
static int ns_stricmp(const char *a, const char *b)
{
    for (;;)
    {
        int ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

#endif
