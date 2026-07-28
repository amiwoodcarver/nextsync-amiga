/*
 * nsprefs.h -- the Preferences dialog.
 */

#ifndef NSPREFS_H
#define NSPREFS_H

#include <exec/types.h>

#include "agui.h"
#include "nsconf.h"

/*
 * Opens Preferences as a modal window. parent may be NULL, which is the
 * first run case: there is no main window yet.
 *
 * On Save the configuration is written to conffile, conf is updated in
 * place and TRUE is returned. On Cancel conf is left exactly as it was and
 * FALSE is returned.
 */
BOOL nsprefs_show(struct AGUIApp *parent, nsconf *conf, const char *conffile);

/* test != 0 drives the dialog from code instead of from the mouse; see
 * the PREFSTEST section in nsprefs.c */
BOOL nsprefs_show_ex(struct AGUIApp *parent, nsconf *conf,
                     const char *conffile, int test);

#endif
