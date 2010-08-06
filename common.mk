# $Id: common.mk,v 1.4 2004/05/30 08:53:03 mann Exp $

CFLAGS= -O3 -Wall
##CFLAGS= -g -O -Wall
CC= gcc
CWEXE= cw2dmk$E dmk2cw$E testhist$E
EXE= $(CWEXE) dmk2jv3$E jv2dmk$E
TXT= cw2dmk.txt dmk2cw.txt dmk2jv3.txt jv2dmk.txt
NROFFFLAGS= -c -Tascii

.SUFFIXES: .man .txt

.man.txt:
	nroff -man $(NROFFFLAGS) $< | colcrt - | cat -s > $*.txt

all: $(EXE) $(TXT) cwsdpmi.exe

cw2dmk$E: cw2dmk.c catweasl.$O cwpci.$O crc.c cwfloppy.h kind.h dmk.h
	$(CC) $(CFLAGS) -o $@ $< catweasl.$O cwpci.$O $(PCILIB)

dmk2cw$E: dmk2cw.c catweasl.$O cwpci.$O crc.c cwfloppy.h kind.h dmk.h
	$(CC) $(CFLAGS) -o $@ $< catweasl.$O cwpci.$O $(PCILIB)

dmk2jv3$E: dmk2jv3.c crc.c dmk.h jv3.h
	$(CC) $(CFLAGS) -o $@ $<

jv2dmk$E: jv2dmk.c crc.c dmk.h jv3.h
	$(CC) $(CFLAGS) -o $@ $<

testhist$E: testhist.c catweasl.$O cwpci.$O cwfloppy.h
	$(CC) $(CFLAGS) -o $@ $< catweasl.$O cwpci.$O $(PCILIB) -lm

cwsdpmi.exe:
	cp $(HOME)/djgpp/csdpmi5b/bin/cwsdpmi.exe cwsdpmi.exe

clean:
	$(RM) $(EXE) *.$O *~

veryclean: clean
	$(RM) *.exe *.obj *.o $(TXT)

setuid:
	chown root $(CWEXE)
	chmod 4755 $(CWEXE)
