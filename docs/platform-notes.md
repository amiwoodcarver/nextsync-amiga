# Platform notes

Six AmigaOS specifics that each cost a real debugging session while
building NextSync. Written down because none of them announce themselves
and all of them look like something else at first.

## 1. AmiSSL crashes at init without the mathieee libraries

**Symptom.** `OpenAmiSSLTags()` never returns. No requester, no Guru — the
program simply stops, and under emulation the CPU can halt outright.

**Cause.** AmiSSL's OS3 build uses clib2, whose startup opens
`mathieeedoubbas.library` and `mathieeedoubtrans.library` and then calls
through the returned bases without checking them. On a system where those
are missing, that is a jump through NULL inside library init, before any of
your code runs and before AmiSSL can report anything.

**Fix.** Make sure both are in `LIBS:`. They ship with every Workbench
install, so this only bites hand-built boot setups — which is exactly what
a minimal emulator test drive is. `tools/setup-sysdrive.sh` builds a real
Workbench drive and warns if either library is absent.

This is worth knowing generally: a bare Kickstart boot directory is enough
for Intuition and GadTools programs, but not for anything that links
AmiSSL.

## 2. CloseAmiSSL() must be the last thing before exit

**Symptom.** A sync completes perfectly, then the program dies a fraction
of a second later — during unrelated work, on an allocation that has
nothing to do with networking. Moving code around moves the crash, which
makes it look like memory corruption in your own code.

**Cause.** On OS3 (confirmed on AmiSSL 5.15 and 5.27) tearing AmiSSL down
leaves the process's memory in a state that the next allocation trips over.
A process that closes AmiSSL and keeps running is living on borrowed time.

**Fix.** Open AmiSSL once and keep it open for the lifetime of the process;
call `CloseAmiSSL()` immediately before exiting and do nothing afterwards.
In NextSync that is `nshttp_shutdown()`, called from `main()` as the last
statement. `nshttp_close()` deliberately only drops the connection, not the
library.

This also happens to be faster: AmiSSL's initialisation is expensive, so a
long-running program that syncs repeatedly should pay it once.

## 3. amiga.lib's sprintf is not C's sprintf

**Symptom.** TLS connects, the handshake succeeds, and then the very first
`send()` fails with a length in the hundreds of thousands of bytes for a
request that should be about 300.

**Cause.** `amiga.lib` provides a `sprintf` that is a thin `RawDoFmt`
wrapper. It formats correctly but does **not** return the number of
characters written. If `-lamiga` appears before libnix on the link line, it
satisfies the `sprintf` symbol first and silently shadows the real one for
every object linked after it. Code that does the ordinary thing —

```c
LONG n = sprintf(buf, "...");
send(sock, buf, n, 0);
```

— then sends garbage length. Nothing warns you; the linker is doing exactly
what it was told.

**Fix.** Put `-lnix` before `-lamiga`. The Makefile here pins that order
with a comment so it does not get "tidied" later.

The general lesson: on this toolchain, be suspicious of any C standard
library function that amiga.lib also happens to export.

## 4. FS-UAE 3.x cannot run AmiSSL

**Symptom.** Under FS-UAE, AmiSSL's init hangs and the emulator logs
`CPU halted: reason = 3`. The identical binary and identical system drive
work on real hardware and on other emulators.

**Cause.** FS-UAE 3.2's CPU core derives from WinUAE 3300b2 (2016) and
mis-executes something in AmiSSL's 68020+ library init. It is an emulator
bug, not an AmiSSL or program bug. Changing CPU model, FPU model, memory
layout, JIT, and AmiSSL version all fail to help.

**Fix.** Use [Amiberry](https://amiberry.com/), whose core is current.
`tools/run-emu.sh` uses it. FS-UAE remains fine for non-networked Amiga
work.

## 5. There is no password gadget, only an edit hook

**Symptom.** You want a string gadget that shows asterisks. GadTools has
no tag for it, Intuition's string gadget has no secret mode, and a string
gadget draws whatever is in its buffer — so anything you keep there for
safe keeping is exactly what the user sees.

**Cause.** By design. The buffer *is* the display.

**Fix.** Keep two buffers and join them with a string edit hook
(`GTST_EditHook`, `struct SGWork`, `intuition/sghooks.h`). Intuition
applies each keystroke to a work buffer and then calls the hook; the hook
works out what changed, makes the same change to the real text it holds
privately, and writes a row of asterisks back to be displayed.

Working out what changed is less work than it sounds, because the length
and the cursor position are enough:

- longer by n: n characters were inserted ending at the cursor, so they
  sit at `BufferPos - n`
- shorter by n: n characters were removed *at* the cursor — backspace
  leaves the cursor where the deleted character was and delete never
  moves it, so one case covers both, and clear and delete-to-end-of-line
  fall out of it as well

Set `SGA_USE | SGA_REDISPLAY` in `SGWork.Actions` and return non-zero, or
Intuition keeps its own version of the buffer. Return 0 for any command
other than `SGH_KEY`.

The hook needs amiga.lib's `HookEntry` in `h_Entry` with the C function in
`h_SubEntry`: Intuition calls hooks with their arguments in registers.

In agui this is `AG_PASSWORD`. Leaving the character just typed legible
until the next keystroke costs nothing extra — the hook is rewriting the
buffer on every key anyway — and it beats a timer poking at a gadget
somebody is in the middle of editing.

## 6. Synthetic mouse clicks do not survive an emulator

**Symptom.** Raw key events written to `input.device` with
`IND_WRITEEVENT` drive a real Amiga program perfectly. The same approach
for a mouse click — `IECLASS_POINTERPOS` or `IECLASS_NEWPOINTERPOS` with
`IESUBCLASS_PIXEL` to place the pointer, then `IECLASS_RAWMOUSE` for the
button — never lands on the gadget. Chaining the events through
`ie_NextEvent` so nothing can be interleaved does not help either, and
neither does turning the emulator's pointer integration off.

**Cause.** Not established. Keyboard injection works, so the mechanism is
sound; the pointer position an emulator maintains for the host mouse
appears to win over the one an injected event asks for.

**Fix.** None found. For automated tests, reach the same handler through
the keyboard — which is a good idea regardless, since a keyboard route
into an action is worth having for its own sake. Buttons stay a
by-hand check.

---

## A smaller one: libnix runtime surprises

`atol()` crashed in one small program in a way that a hand-rolled digit
loop did not. It was not worth chasing to a root cause at the time, but it
is a reminder that the C library on this platform is not the one you are
used to. In Amiga-side code paths that run before much else is set up,
parsing numbers by hand costs four lines and removes a variable.
