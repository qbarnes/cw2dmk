/* $Id: cwfloppy.h,v 1.12 2005/04/24 04:16:45 mann Exp $ */

#ifndef _CWFLOPPY_H
#define _CWFLOPPY_H

/*
 * Low-level routines
 */

typedef struct catweasel_drive {
    struct catweasel_contr *contr; /* The controller this drive belongs to */
    int number;                    /* Drive number: 0 or 1 */
    int type;                      /* 0 = not present, 1 = present */
    int track;                     /* current r/w head position (0..79) */
    int diskindrive;               /* 0 = no disk, 1 = disk in drive */
} catweasel_drive;

typedef struct catweasel_contr {
    int iobase;                    /* 0 = not present */
    void (*msdelay)(int ms);       /* microseconds delay routine */
    int mk;                        /* 1 = Catweasel MK1, 3 = MK3, 4 = MK4 */
    unsigned char *reg;
    unsigned char *stat;
    unsigned char *ctrl;
    catweasel_drive drives[2];     /* max. two drives on each controller */
    int private[4];                /* private data */
    unsigned step_us, settle_us;
} catweasel_contr;

/* Initialize a Catweasel controller.  Return true on success. */
int catweasel_init_controller(catweasel_contr *c, int iobase, int mk,
			      char *fwname,
                              unsigned step_ms, unsigned settle_ms);

/* Detect whether drive is present using track0 sensor */
void catweasel_detect_drive(catweasel_drive *d);

/* Reset the controller */
void catweasel_free_controller(catweasel_contr *c);

/* Set current drive select mask */
void catweasel_select(catweasel_contr *c, int dr0, int dr1);

/* Start/stop the drive's motor */
void catweasel_set_motor(catweasel_drive *d, int on);

/* Move the r/w head -- msdelay might be used */
void catweasel_seek(catweasel_drive *d, int track);

/* Check for a disk change and update d->diskindrive
   -- msdelay might be used. Returns 1 == disk has been changed */
int catweasel_disk_changed(catweasel_drive *d);

/* Check if disk in selected drive is write protected. */
int catweasel_write_protected(catweasel_drive *d);

/* Read data -- msdelay will be used */
/* If time = 0, read from index hole to index hole */
/* If idx = 1, high order bit will be set while index hole is going by */
/* Inserts a 0x80 0x00 sequence at the end of the buffer if there is room. */
int catweasel_read(catweasel_drive *d, int side, int clock, int time, int idx);

/* Write data -- msdelay will be used */
/* If time = 0, write from index hole to index hole */
int catweasel_write(catweasel_drive *d, int side, int clock, int time);

/* Trivial Catweasel memory test.  Returns number of seemingly good bytes;
 * should be 128K */
int catweasel_memtest(catweasel_contr *c);

/* Fill Catweasel's memory with specified value, and check it is there. */
void catweasel_fillmem(catweasel_contr *c, unsigned char byte);

/* Wait until index hole is next seen */
int catweasel_await_index(catweasel_drive *d);

/* Set the HD line of the Shugart bus */
void catweasel_set_hd(catweasel_contr *c, int hd);

/* Get one byte from Catweasel memory.  Returns -1 if reading past end
   of memory.  Also watch for the 0x80 0x00 sequence that
   catweasel_read puts at the end of partial reads.  Although this is
   technically ambiguous, it should be very rare in real data. */
int catweasel_get_byte(catweasel_contr *c);

/* Reset Catweasel memory pointer */
void catweasel_reset_pointer(catweasel_contr *c);

/* Put one byte to Catweasel memory.  Return 0 if OK, -1 if
   memory was full.  */
int catweasel_put_byte(catweasel_contr *c, unsigned char val);

/* Working version of usleep */
unsigned int catweasel_usleep(unsigned int _useconds);

/* Catweasel min sample rate.
 * MK1 supports 1x and 2x; MK3 and MK4 support 1x, 2x, 4x */
#define CWHZ 7080500.0

/* Factory port setting for MK1 */
#define MK1_DEFAULT_PORT 0x320
#define MK1_MIN_PORT 0x220
#define MK1_MAX_PORT 0x388
#define MK3_MAX_CARDS 16

#endif /* _CWFLOPPY_H */
