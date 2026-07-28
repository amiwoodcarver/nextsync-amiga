# ---------------------------------------------------------------------------
# NextSync -- Nextcloud client for AmigaOS 3.1+
#
#   make                    build NextSync
#   make TOOLCHAIN=/opt/amiga
#   make CPU=68030          (default 68020, the AmiSSL minimum)
#   make DEBUG=1            symbols, no optimisation
#   make clean
#
# Prerequisites, both fetched by scripts in tools/:
#   TOOLCHAIN    m68k-amigaos-gcc      tools/build-toolchain.sh
#   AMISSL_SDK   AmiSSL v5 SDK         tools/fetch-deps.sh
# ---------------------------------------------------------------------------

ROOT       := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
TOOLCHAIN  ?= $(ROOT)/toolchain
AMISSL_SDK ?= $(ROOT)/vendor/sdk/AmiSSL/Developer

CROSS  := $(TOOLCHAIN)/bin/m68k-amigaos-
CC     := $(CROSS)gcc
AR     := $(CROSS)ar
RANLIB := $(CROSS)ranlib

# AmiSSL v5 is 68020 or better, so there is no point targeting 68000 here
CPU ?= 68020

TARGET := NextSync

SRCS := src/main.c src/gui.c src/nsprefs.c src/nstest.c src/nshttp.c src/nsdav.c \
        src/nsxml.c src/nssync.c src/nsconf.c
AGUI := agui/src/agui.c agui/src/aguishot.c

OBJS := $(SRCS:.c=.o) $(AGUI:.c=.o)

WARN := -Wall -Wno-pointer-sign -Wno-attributes

ifeq ($(DEBUG),1)
  OPT     := -O0 -g
  STRIPLD :=
else
  OPT     := -Os -fomit-frame-pointer
  STRIPLD := -s
endif

CFLAGS  += $(OPT) $(WARN) -m$(CPU) -msmall-code -noixemul \
           -Iagui/include -Isrc -I$(AMISSL_SDK)/include
LDFLAGS += -noixemul -m$(CPU) $(STRIPLD) -L$(AMISSL_SDK)/lib/AmigaOS3

# -lnix before -lamiga: amiga.lib carries a RawDoFmt based sprintf whose
# return value is not the byte count, and it would otherwise shadow the
# real one for every object linked after it.
LIBS += -lnix -lamiga -lamisslstubs

.PHONY: all clean check-prereqs

all: check-prereqs $(TARGET)

check-prereqs:
	@test -x $(CC) || { \
	  echo "no cross compiler at $(CC)"; \
	  echo "  run tools/build-toolchain.sh, or pass TOOLCHAIN=/path"; \
	  exit 1; }
	@test -d $(AMISSL_SDK)/include || { \
	  echo "no AmiSSL SDK at $(AMISSL_SDK)"; \
	  echo "  run tools/fetch-deps.sh, or pass AMISSL_SDK=/path"; \
	  exit 1; }

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@echo "  built $@ ($$(stat -f%z $@ 2>/dev/null || stat -c%s $@) bytes, $(CPU))"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
