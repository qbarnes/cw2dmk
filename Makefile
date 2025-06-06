#
# Makefile for cw2dmk family of tools
#
# Makefile will guess if it's building on MS-DOS.  Passing in
# "HOST_OS=MSDOS" or "HOST_OS=" overrides the guess.
#
# To build MS-DOS binaries on Linux using DJGPP, pass in
# "TARGET_OS=MSDOS" and set $(CC) to point to the DJGPP compiler.
#

ifneq ($(COMSPEC),)
  HOST_OS ?= MSDOS
endif

ifeq ($(HOST_OS),MSDOS)
  TARGET_OS = MSDOS
  RM = deltree /y
  CP = rem cp
else
  TARGET_OS =
  RM = rm -f --
  CP = cp --
endif

ifeq ($(TARGET_OS),MSDOS)
  CC = i586-pc-msdosdjgpp-gcc
  E = .exe
  O = obj
  PCILIB =
else
  CC = gcc
  E =
  O = o
  PCILIB = -lpci -lz
endif

CFLAGS = -O3 -g -Wall -std=gnu99

version := $(shell grep VERSION version.h | sed -re 's/^.*"([^"]+)".*$$/\1/')

cc_dump_mach := $(shell $(CC) -dumpmachine)
target_arch   = $(firstword $(subst -, ,$(cc_dump_mach)))

TAR_PREFIX  = cw2dmk-$(version)
TAR_SRC     = $(TAR_PREFIX)-src.tar.gz 
TAR_LINUX   = $(TAR_PREFIX)-linux-$(target_arch).tar.gz
TAR_MSDOS   = $(TAR_PREFIX)-msdos.tar.gz
TAR_TARGETS = $(TAR_SRC) \
		$(if $(subst MSDOS,,$(TARGET_OS)),$(TAR_MSDOS),$(TAR_LINUX))

CWEXE = cw2dmk$E dmk2cw$E cwhist$E
EXE   = $(CWEXE) dmk2jv3$E jv2dmk$E
TXT   = cw2dmk.txt dmk2cw.txt dmk2jv3.txt jv2dmk.txt
NROFFFLAGS = -c -Tascii
FIRMWARE   = firmware/rel2f2.cw4

.SUFFIXES: .man .txt

%.txt: %.man
	nroff -man $(NROFFFLAGS) $< | col -b | cat -s > $*.txt

ifneq ($(TARGET_OS),MSDOS)
else
EXE += cwsdpmi.exe
.SUFFIXES: .obj

%.obj: %.c
	$(CC) -c $(CFLAGS) -o $@ $<
endif

BUILD_TARGETS   = firmware.h $(TXT) $(EXE)
RELEASE_TARGETS = COPYING README ChangeLog $(TXT) $(EXE)

clean = $(EXE) $(TAR_TARGETS) crc$E parselog$E *.$O *~
veryclean = $(clean) $(TXT) firmware.h *.exe *.obj *.o *.tar.gz

all: progs manpages

progs: $(EXE)

manpages: $(TXT)

catweasl.$O: catweasl.c cwfloppy.h firmware.h

cw2dmk$E: cw2dmk.c catweasl.$O cwpci.$O parselog.$O crc.c  \
    cwfloppy.h kind.h dmk.h version.h
	$(CC) $(CFLAGS) -o $@ $< catweasl.$O cwpci.$O parselog.$O $(PCILIB) -lm

dmk2cw$E: dmk2cw.c catweasl.$O cwpci.$O crc.c \
    cwfloppy.h kind.h dmk.h version.h
	$(CC) $(CFLAGS) -o $@ $< catweasl.$O cwpci.$O $(PCILIB)

dmk2jv3$E: dmk2jv3.c crc.c dmk.h jv3.h
	$(CC) $(CFLAGS) -o $@ $<

jv2dmk$E: jv2dmk.c crc.c dmk.h jv3.h
	$(CC) $(CFLAGS) -o $@ $<

cwhist$E: cwhist.c catweasl.$O cwpci.$O parselog.$O cwfloppy.h
	$(CC) $(CFLAGS) -o $@ $< catweasl.$O cwpci.$O parselog.$O $(PCILIB) -lm

crc$E: crc.c
	$(CC) $(CFLAGS) -DTEST -o $@ $<

parselog$E: parselog.c
	$(CC) $(CFLAGS) -DTEST -o $@ $<

firmware.h: $(FIRMWARE)
	(echo 'unsigned char firmware[] = { ' ;\
	 hexdump -v -e '12/1 "%#x, " "\n"' $< ;\
	 echo '};') | sed -e 's/ ,//g' > $@

cwsdpmi.exe: cwsdpmi/bin/CWSDPMI.EXE
	$(CP) "$<" "$@"
	chmod -- +x "$@"

clean veryclean:
	$(if $(wildcard $($@)),$(RM) $(wildcard $($@)))

$(TAR_SRC): firmware.h
	git archive \
		--format=tar.gz \
		--prefix="$(TAR_PREFIX)/" \
		$(addprefix --add-file=,$+) \
		HEAD > "$@"

ifneq ($(TARGET_OS),MSDOS)
  $(TAR_LINUX): $(RELEASE_TARGETS)
	tar -czf "$@" --transform='s:^:$(TAR_PREFIX)/:' -- $+

  $(TAR_MSDOS):
	+$(MAKE) TARGET_OS=MSDOS "$@"
else
  $(TAR_MSDOS): $(RELEASE_TARGETS)
	tar -czf "$@" --transform='s:^:$(TAR_PREFIX)/:' -- $+
endif

ifneq ($(HOST_OS),MSDOS)
  release: $(TAR_TARGETS)
  fullrelease: release $(TAR_PREFIX)-msdos.tar.gz
endif

setuid: $(CWEXE)
	chown root $(CWEXE)
	chmod 4755 $(CWEXE)


.PHONY: all progs manpages clean veryclean release fullrelease setuid
.DELETE_ON_ERROR:
