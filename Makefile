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
  E = .exe
  O = obj
  PCILIB =
else
  E =
  O = o
  PCILIB = -lpci -lz
endif

CC     = gcc
CFLAGS = -O3 -g -Wall

CWEXE = cw2dmk$E dmk2cw$E testhist$E
EXE   = $(CWEXE) dmk2jv3$E jv2dmk$E
TXT   = cw2dmk.txt dmk2cw.txt dmk2jv3.txt jv2dmk.txt
NROFFFLAGS = -c -Tascii
FIRMWARE   = firmware/rel2f2.cw4

clean = $(EXE) *.$O *~
veryclean = $(clean) $(TXT) firmware.h *.exe *.obj *.o

.SUFFIXES: .man .txt

%.txt: %.man
	nroff -man $(NROFFFLAGS) $< | col -b | cat -s > $*.txt

ifeq ($(TARGET_OS),MSDOS)
EXE += cwsdpmi.exe
.SUFFIXES: .obj

%.obj: %.c
	$(CC) -c $(CFLAGS) -o $@ $<
endif


all: progs manpages

progs: $(EXE)

manpages: $(TXT)

catweasl.$O: catweasl.c cwfloppy.h firmware.h

cw2dmk$E: cw2dmk.c catweasl.$O cwpci.$O crc.c  \
    cwfloppy.h kind.h dmk.h version.h
	$(CC) $(CFLAGS) -o $@ $< catweasl.$O cwpci.$O $(PCILIB) -lm

dmk2cw$E: dmk2cw.c catweasl.$O cwpci.$O crc.c \
    cwfloppy.h kind.h dmk.h version.h
	$(CC) $(CFLAGS) -o $@ $< catweasl.$O cwpci.$O $(PCILIB)

dmk2jv3$E: dmk2jv3.c crc.c dmk.h jv3.h
	$(CC) $(CFLAGS) -o $@ $<

jv2dmk$E: jv2dmk.c crc.c dmk.h jv3.h
	$(CC) $(CFLAGS) -o $@ $<

testhist$E: testhist.c catweasl.$O cwpci.$O cwfloppy.h
	$(CC) $(CFLAGS) -o $@ $< catweasl.$O cwpci.$O $(PCILIB) -lm

firmware.h: $(FIRMWARE)
	(echo 'unsigned char firmware[] = { ' ;\
	 hexdump -v -e '12/1 "%#x, " "\n"' $< ;\
	 echo '};') | sed -e 's/ ,//g' > $@

cwsdpmi.exe: cwsdpmi/bin/CWSDPMI.EXE
	$(CP) $< $@

clean veryclean:
	$(if $(wildcard $($@)),$(RM) $(wildcard $($@)))

setuid: $(CWEXE)
	chown root $(CWEXE)
	chmod 4755 $(CWEXE)


.PHONY: all progs manpages clean veryclean setuid
.DELETE_ON_ERROR:
