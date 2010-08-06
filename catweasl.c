/*
 * Catweasel -- Advanced Floppy Controller
 * Linux device driver
 * Low-level routines
 *
 * Copyright (C) 1998 Michael Krause
 * Modifications by Timothy Mann for use with cw2dmk
 * $Id: catweasl.c,v 1.19 2005/03/29 07:13:40 mann Exp $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#define CHECK_DISK_CHANGED 0  /* older drives don't provide this signal */

#include "cwfloppy.h"
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#if linux
#include <sys/io.h>
#endif
#if __DJGPP__
#include <pc.h>
#include <dpmi.h>
#define inb(p) inportb(p)
#define outb(v, p) outportb(p, v) /* args in opposite order, arrgh */
#endif

#if DEBUG1
unsigned char INB(unsigned short port)
{
  unsigned char b = inb(port);
  printf("      %04x => %02x\n", port, b);
  return b;
}
void OUTB(unsigned char b, unsigned short port)
{
  printf("%02x => %04x\n", b, port);
  outb(b, port);
}
#undef inb
#undef outb
#define inb INB
#define outb OUTB
#endif /*DEBUG*/


unsigned int
catweasel_usleep(unsigned int _useconds)
{
#if __DJGPP__
  /* DJGPP's usleep is based on a clock with a 55 ms granularity, so
     it's not usable for short sleeps!  Fortunately DJGPP also makes
     a fine-grained clock available.  This clock works only on
     MS-DOS (with a DPMI host) or Windows 9X, not Windows 3.1 or NT.
     Of course cw2dmk won't work on NT anyway, because it accesses
     I/O ports directly.
  */
  uclock_t wakeup = uclock() +
    ((uclock_t)_useconds) * ((uclock_t)UCLOCKS_PER_SEC) / 1000000LL;
  while (uclock() <= wakeup) __dpmi_yield();
  return 0;
#else
  (void) usleep(_useconds);
  return 0;
#endif
}


/* Register names */
enum CatRegister { JoyDat, PaddleSelect, Joybutton, Joybuttondir, KeyDat,
		   KeyStatus, SidDat, SidCommand, CatMem, CatAbort, CatControl,
		   CatOption, CatStartA, CatStartB, CatIRQ };

/* Register addresses */
unsigned char Mk1Reg[] = {
    /* JoyDat */       -1,
    /* PaddleSelect */ -1,
    /* Joybutton */    -1,
    /* Joybuttondir */ -1,
    /* KeyDat */       -1,
    /* KeyStatus */    -1,
    /* SidDat */       -1,
    /* SidCommand */   -1,
    /* CatMem */       0,
    /* CatAbort */     1,    /* reading/writing this reg reversed on MK3 */
    /* CatControl */   2,
    /* CatOption */    3,
    /* CatStartA */    7,
    /* CatStartB */    5,
    /* CatIRQ */       -1
};

unsigned char Mk3Reg[] = {
    /* JoyDat */       0xc0,
    /* PaddleSelect */ 0xc4,
    /* Joybutton */    0xc8,
    /* Joybuttondir */ 0xcc,
    /* KeyDat */       0xd0,
    /* KeyStatus */    0xd4,
    /* SidDat */       0xd8,
    /* SidCommand */   0xdc,
    /* CatMem */       0xe0,
    /* CatAbort */     0xe4,
    /* CatControl */   0xe8,
    /* CatOption */    0xec,
    /* CatStartA */    0xf0,
    /* CatStartB */    0xf4,
    /* CatIRQ */       0xfc
};

/* Status bit position names (read CatControl) */
enum CatStatusBit { CatReading, CatWriting, CatDiskChange, CatUnused,
		    CatWProtect, CatTrack0, CatIndex, CatDensityIn
};

unsigned char Mk1StatusBit[] = {
    /* CatReading */    1<<0,
    /* CatWriting */    1<<1,
    /* CatDiskChange */ 1<<5,
    /* CatUnused */     1<<7,
    /* CatWProtect */   1<<3,
    /* CatTrack0 */     1<<4,
    /* CatIndex */      1<<6,
    /* CatDensityIn */  1<<2,
};

unsigned char Mk3StatusBit[] = {
    /* CatReading */    1<<7,
    /* CatWriting */    1<<6,
    /* CatDiskChange */ 1<<5,
    /* CatUnused */     1<<4,
    /* CatWProtect */   1<<3,
    /* CatTrack0 */     1<<2,
    /* CatIndex */      1<<1,
    /* CatDensityIn */  1<<0,
};

/* Control bit position names (write CatControl) */
enum CatControlBit { 
    CatStep, CatSideSelect, CatMotor0, CatDirection,
    CatSelect0, CatSelect1, CatMotor1, CatDensityOut
};

unsigned char Mk1ControlBit[] = {
    /* CatStep */        1<<0,
    /* CatSideSelect */  1<<2,
    /* CatMotor0 */      1<<7,
    /* CatDirection */   1<<1,
    /* CatSelect0 */     1<<4,
    /* CatSelect1 */     1<<5,
    /* CatMotor1 */      1<<3,
    /* CatDensityOut */  1<<6,
};

unsigned char Mk3ControlBit[] = {
    /* CatStep */        1<<7,
    /* CatSideSelect */  1<<6,
    /* CatMotor0 */      1<<5,
    /* CatDirection */   1<<4,
    /* CatSelect0 */     1<<3,
    /* CatSelect1 */     1<<2,
    /* CatMotor1 */      1<<1,
    /* CatDensityOut */  1<<0,
};

#define MEMSIZE 131072
#define CREG(c) (c->private[0])
#define INREG(c, name) inb((c)->iobase + (c)->reg[name])
#define OUTREG(c, name, val) outb((val), (c)->iobase + (c)->reg[name])
#define SBIT(c, name) ((c)->stat[name])
#define CBIT(c, name) ((c)->ctrl[name])

void
catweasel_reset_pointer(catweasel_contr *c)
{
    if (c->mk == 1) {
	INREG(c, CatAbort);
    } else {
	OUTREG(c, CatAbort, 0);
    }
}

void
catweasel_abort(catweasel_contr *c)
{
    if (c->mk == 1) {
	OUTREG(c, CatAbort, 0);
    } else {
	INREG(c, CatAbort);
    }
}

/* Await particular bit value(s) in the control register.  Return 1 if
   it appears, 0 if 5 seconds pass and it doesn't.  Bits that are 1
   in "clear" must be 0; bits that are 1 in "set" must be 1; others
   are don't-cares. */
int
CWAwaitCReg(catweasel_contr *c, int clear, int set)
{
  int i;
  struct timeval tv1, tv2;

  gettimeofday(&tv1, NULL);
  for (;;) {
      i = 1000000;
      while (i--) {
	  int v = INREG(c, CatControl);
	  if ((v & clear) == 0 && (v & set) == set) return 1;
      }
      gettimeofday(&tv2, NULL);
      if ((tv2.tv_sec - tv1.tv_sec)*1000000 + tv2.tv_usec - tv2.tv_usec
	  > 5000000) return 0;
  }
}

/* Trivial memory test.  Returns number of seemingly good bytes. */
int
catweasel_memtest(catweasel_contr *c)
{
    int i;

    catweasel_reset_pointer(c);
    for (i=0; i<MEMSIZE; i++) {
	OUTREG(c, CatMem, i%4093);
    }
    catweasel_reset_pointer(c);
    for (i=0; i<MEMSIZE; i++) {
	if (INREG(c, CatMem) != (unsigned char) (i%4093)) {
	    return i;
	}
    }
    catweasel_reset_pointer(c);
    for (i=0; i<MEMSIZE; i++) {
	OUTREG(c, CatMem, ~(i%4093));
    }
    catweasel_reset_pointer(c);
    for (i=0; i<MEMSIZE; i++) {
	if (INREG(c, CatMem) != (unsigned char) ~(i%4093)) {
	    return i;
	}
    }
    return i;
}

/* Fill Catweasel's memory with specified value, and check it is there. */
void
catweasel_fillmem(catweasel_contr *c, unsigned char byte)
{
    int i;

    catweasel_reset_pointer(c);
    for (i=0; i<MEMSIZE; i++) {
	OUTREG(c, CatMem, byte);
    }
    catweasel_reset_pointer(c);
    for (i=0; i<MEMSIZE; i++) {
	if (INREG(c, CatMem) != byte) {
	    printf("catweasel memory fill error at address %#x\n", i);
	}
    }
}

static __inline__ void
CWSetCReg(catweasel_contr *c, unsigned char clear, unsigned char set)
{
    CREG(c) = (CREG(c) & ~clear) | set;
    OUTREG(c, CatControl, CREG(c));
}

static void
CWTriggerStep(catweasel_contr *c)
{
    CWSetCReg(c, CBIT(c, CatStep), 0);
    catweasel_usleep(3000);
    CWSetCReg(c, 0, CBIT(c, CatStep));
    catweasel_usleep(3000);
}

static int
CWTrack0(catweasel_contr *c)
{
  int bit = (INREG(c, CatControl) & SBIT(c, CatTrack0));
  return bit == 0;
}

void
catweasel_init_controller(catweasel_contr *c, int iobase, int mk)
{
    int i;

    c->iobase = iobase;
    c->mk = mk;

    switch (mk) {
    case 1:
	c->reg = Mk1Reg;
	c->stat = Mk1StatusBit;
	c->ctrl = Mk1ControlBit;
	break;
    case 3:
	c->reg = Mk3Reg;
	c->stat = Mk3StatusBit;
	c->ctrl = Mk3ControlBit;
	/* Initialize PCI bridge */
	outb(0xf1, iobase+0x00);
	outb(0x00, iobase+0x01);
	outb(0x00, iobase+0x02);
	outb(0x00, iobase+0x04);
	outb(0x00, iobase+0x05);
	outb(0x00, iobase+0x29);
	outb(0x00, iobase+0x2b);
	break;
    }

    for (i=0; i<2; i++) {
	c->drives[i].number = i;
	c->drives[i].contr = c;
	c->drives[i].diskindrive = 0;
    }

    CREG(c) = 255;
    catweasel_abort(c);
}

void
catweasel_detect_drive(catweasel_drive *d)
{
    int i, j;
    catweasel_contr *c = d->contr;

    if (!c->iobase) {
	return;
    }
	
    /* select drive and start motor */
    catweasel_select(c, d->number == 0, d->number == 1);
    catweasel_set_motor(d, 1);

    /* assume track 0 sensor works until proven otherwise below */
    d->type = 1;

    /* if track 0 sensor is active, step in to turn it off */
    if (CWTrack0(c)) {
	CWSetCReg(c, CBIT(c, CatDirection), 0);
	for (i=0; i<3; i++) {
	    CWTriggerStep(c);
	}
	if (CWTrack0(c)) {
	    /* drive without working track 0 sensor, or no drive */
	    d->type = 0;
	}
    }

    /* step out to track 0 */
    CWSetCReg(c, 0, CBIT(c, CatDirection));
    for (j=0; j<90 && (d->type == 0 || !CWTrack0(c)); j++) {
	CWTriggerStep(c);
    }
    d->track = 0;
	
    if (j == 90) {
	/* drive without working track 0 sensor, or no drive */
	d->type = 0;
    }
    
    /* deselect all drives, stop motor */
    catweasel_set_motor(d, 0);
    catweasel_select(c, 0, 0);
}

void
catweasel_free_controller(catweasel_contr *c)
{
    if(!c->iobase) {
	return;
    }

    /* stop all operations */
    catweasel_abort(c);

    /* all motors off, deselect all drives */
    CWSetCReg(c, 0, (CBIT(c, CatSelect0) | CBIT(c, CatSelect1) |
		     CBIT(c, CatMotor0) | CBIT(c, CatMotor1)));
}

void
catweasel_select(catweasel_contr *c, int dr0, int dr1)
{
    CWSetCReg(c, ((dr0 ? CBIT(c, CatSelect0) : 0) |
		  (dr1 ? CBIT(c, CatSelect1) : 0)),
	         ((!dr0 ? CBIT(c, CatSelect0) : 0) |
	          (!dr1 ? CBIT(c, CatSelect1) : 0)));
}

void
catweasel_set_motor(catweasel_drive *d, int on)
{
    int mask = CBIT(d->contr, d->number ? CatMotor1 : CatMotor0);
    
    if (on) {
	CWSetCReg(d->contr, mask, 0);
    } else {
	CWSetCReg(d->contr, 0, mask);
    }
}

void
catweasel_seek(catweasel_drive *d, int t)
{
    int x;

    if (t >= 85) {
	return;
    }

    x = t - d->track;
    if (!x) {
	return;
    }

    if (x >= 0) {
	CWSetCReg(d->contr, CBIT(d->contr, CatDirection), 0);
    } else {
	CWSetCReg(d->contr, 0, CBIT(d->contr, CatDirection));
	x = -x;
    }

    while (x--) {
	CWTriggerStep(d->contr);
    }

    d->track = t;
}

#if CHECK_DISK_CHANGED
int
catweasel_disk_changed(catweasel_drive *d)
{
    int ot, t, changed = 0;

    if (INREG(d->contr, CatControl) & SBIT(d->contr, CatDiskChange)) {
	if(!d->diskindrive) {
	    /* first usage of this drive, issue disk change */
	    d->diskindrive = 1;
	    changed = 1;
	} else {
	    /* there's still a disk in there, no change detected. */
	}
    } else {
	ot = d->track;
	if(ot == 79)
	    t = 78;
	else
	    t = ot + 1;
	
	catweasel_seek(d, t);
	catweasel_seek(d, ot);
	
	if (!(INREG(d->contr, CatControl) & SBIT(d->contr, CatDiskChange))) {
	    if(!d->diskindrive) {
		/* drive still empty, nothing has happened */
	    } else {
		/* disk has been removed */
		d->diskindrive = 0;
		changed = 1;
	    }
	} else {
	    /* disk has been inserted */
	    d->diskindrive = 1;
	    changed = 1;
	}
    }

    return changed;
}
#endif

int
catweasel_write_protected(catweasel_drive *d)
{
    return !(INREG(d->contr, CatControl) & SBIT(d->contr, CatWProtect));
}

/* Select clock multiplier */
static int
CWEncodeClock(catweasel_contr *c, int multiplier)
{
    switch (multiplier) {
    case 1:
	return (c->mk == 1) ? 0x80 : 0x00;
    case 2:
	return (c->mk == 1) ? 0x00 : 0x80;
    case 4:
	if (c->mk == 3) return 0xc0;
	/* fall through */
    default:
	fprintf(stderr, "unsupported MK%d clock multiplier %d\n",
		c->mk, multiplier);
	catweasel_free_controller(c);
	exit(1);
    }
}

int
catweasel_read(catweasel_drive *d, int side, int clockmult, int time, int idx)
{
    catweasel_contr *c = d->contr;

#if CHECK_DISK_CHANGED
    if (!CWAwaitCReg(c, 0, SBIT(c, CatDiskChange))) return 0;
#endif

    /* set disk side */
    CWSetCReg(c, CBIT(c, CatSideSelect), (!side) ? CBIT(c, CatSideSelect) : 0);

    /* select clock */
    catweasel_reset_pointer(c);
    OUTREG(c, CatOption, CWEncodeClock(c, clockmult));

    /* store or don't store index pulse in high-order bit */
    /* Note: on MK3, turning on index storage and requesting the
     * index to index read instead does a read starting at the
     * next MFM sync sequence! */
    catweasel_reset_pointer(c);  /* pointer = 0 */
    INREG(c, CatMem);            /* pointer++ (=1) */
    INREG(c, CatMem);            /* pointer++ (=2) */
    OUTREG(c, CatOption, idx ? 0x80 : 0);
    if (c->mk == 3 && !idx) {
	INREG(c, CatMem);        /* pointer++ (=3) */
	OUTREG(c, CatOption, 0);
    }

    catweasel_reset_pointer(c);
    if (time <= 0) {
	/* read index hole to index hole */
	INREG(c, CatStartB);
	/* wait for read to start */
	if (!CWAwaitCReg(c, SBIT(c, CatReading), 0)) return 0;
	/* wait for read to end */
	if (!CWAwaitCReg(c, 0, SBIT(c, CatReading))) return 0;
    } else {
	/* start reading immediately */
	INREG(c, CatStartA);
	/* wait the prescribed time */
	catweasel_usleep(time*1000);
	/* stop reading, don't reset pointer */
	catweasel_abort(c);
    }
    /* add data end mark */
    OUTREG(c, CatMem, 0xff);
    OUTREG(c, CatMem, 0xff);
    OUTREG(c, CatMem, 0);
    OUTREG(c, CatMem, 0xff);
    OUTREG(c, CatMem, 0);
    OUTREG(c, CatMem, 0xff);
    OUTREG(c, CatMem, 0);

    /* reset RA */
    catweasel_reset_pointer(c);

    return 1;
}

int
catweasel_write(catweasel_drive *d, int side, int clockmult, int time)
{
    catweasel_contr *c = d->contr;

#if CHECK_DISK_CHANGED
    if (!CWAwaitCReg(c, 0, SBIT(c, CatDiskChange))) return 0;
#endif

    /* set disk side */
    CWSetCReg(c, CBIT(c, CatSideSelect), (!side) ? CBIT(c, CatSideSelect) : 0);

    /* select clock */
    catweasel_reset_pointer(c);
    OUTREG(c, CatOption, CWEncodeClock(c, clockmult));

    /* enable writing */
    catweasel_reset_pointer(c);
    INREG(c, CatMem);           /* pointer++ (=1) */
    OUTREG(c, CatOption, 0x80);

    INREG(c, CatMem);           /* pointer++ (=2) */
    INREG(c, CatMem);           /* pointer++ (=3) */
    INREG(c, CatMem);           /* pointer++ (=4) */
    INREG(c, CatMem);           /* pointer++ (=5) */
    INREG(c, CatMem);           /* pointer++ (=6) */
    INREG(c, CatMem);           /* pointer++ (=7) */

    if (time <= 0) {
	/* write from index hole to index hole */
	OUTREG(c, CatStartA, 0);
	/* wait for write to start */
	if (!CWAwaitCReg(c, SBIT(c, CatWriting), 0)) return 0;
	/* wait for write to end */
	if (!CWAwaitCReg(c, 0, SBIT(c, CatWriting))) return 0;
    } else {
	/* start writing immediately */
	OUTREG(c, CatStartB, 0);
	/* wait for prescribed time */
	catweasel_usleep(time*1000);
    }
    /* stop writing and reset RA */
    catweasel_abort(c);
    catweasel_reset_pointer(c);

    return 1;
}

int catweasel_await_index(catweasel_drive *d)
{
    catweasel_contr *c = d->contr;
#if CHECK_DISK_CHANGED
    if (!CWAwaitCReg(c, 0, SBIT(c, CatDiskChange))) return 0;
#endif
    if (!CWAwaitCReg(c, 0, SBIT(c, CatIndex))) return 0;
    if (!CWAwaitCReg(c, SBIT(c, CatIndex), 0)) return 0;
    return 1;
}

void catweasel_set_hd(catweasel_contr *c, int hd)
{
    if (hd) {
	CWSetCReg(c, 0, CBIT(c, CatDensityOut));
    } else {
	CWSetCReg(c, CBIT(c, CatDensityOut), 0);
    }
}

unsigned char catweasel_get_byte(catweasel_contr *c)
{
    return INREG(c, CatMem);
}

void catweasel_put_byte(catweasel_contr *c, unsigned char val)
{
    OUTREG(c, CatMem, val);
}

