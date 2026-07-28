/*
 * nstest.c -- synthetic input for the self tests, see nstest.h.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <intuition/intuition.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include <string.h>

#include "nstest.h"

/*
 * The events go in as one chain rather than one call each: an emulator
 * puts the Amiga pointer back under the host's mouse whenever it gets the
 * chance, and anything it slips in between a press and the release lands
 * the click somewhere else entirely.
 */
static void send_events(struct InputEvent *ev, int n)
{
    struct MsgPort *port = CreateMsgPort();
    struct IOStdReq *io;
    int i;

    if (!port)
        return;

    for (i = 0; i < n - 1; i++)
        ev[i].ie_NextEvent = &ev[i + 1];
    ev[n - 1].ie_NextEvent = NULL;

    io = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
    if (io)
    {
        if (!OpenDevice("input.device", 0, (struct IORequest *)io, 0))
        {
            io->io_Command = IND_WRITEEVENT;
            io->io_Data    = ev;
            io->io_Length  = sizeof(struct InputEvent);
            DoIO((struct IORequest *)io);
            CloseDevice((struct IORequest *)io);
        }
        DeleteIORequest((struct IORequest *)io);
    }
    DeleteMsgPort(port);
    Delay(5);
}

void nstest_key(UWORD rawkey)
{
    struct InputEvent ie;

    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code  = rawkey;
    send_events(&ie, 1);
    Delay(3);
}
