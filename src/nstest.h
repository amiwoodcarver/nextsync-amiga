/*
 * nstest.h -- synthetic input, for the built in self tests.
 *
 * Some of the interesting paths in a GUI cannot be reached from the
 * program itself: a string gadget's edit hook only ever sees real
 * keystrokes, and a key pressed during a sync has to travel out through
 * Intuition and back into a handler that is already running. Pushing an
 * event into input.device drives that exactly as a person would, so the
 * test covers the real path rather than a stand-in for it.
 *
 * Synthetic mouse events were tried too and do not survive the emulator's
 * input layer, so there is deliberately no click here.
 *
 * Used by the PREFSTEST and ABORTTEST modes only.
 */

#ifndef NSTEST_H
#define NSTEST_H

#include <exec/types.h>
#include <intuition/intuition.h>

/* one keypress, by Amiga raw key code */
void nstest_key(UWORD rawkey);

#endif
