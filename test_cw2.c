/*
 * test_cw2 - mock Catweasel interface via dumped tracks
 *
 * Copyright (C) 2014 George Phillips
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

#include <stdio.h>
#include <stdlib.h>

#include "cwfloppy.h"
#include "cwpci.h"

static int seq = 0;
static int prev_track = -1;
static int prev_side = -1;
static unsigned trackbuf[200000];
static int track_len;
static int tidx;

unsigned int
catweasel_usleep(unsigned int _useconds)
{
	return 0;
}

void
catweasel_reset_pointer(catweasel_contr *c)
{
}

void
catweasel_abort(catweasel_contr *c)
{
}

/* Trivial memory test.  Returns number of seemingly good bytes. */
int
catweasel_memtest(catweasel_contr *c)
{
	return 1;
}

/* Fill Catweasel's memory with specified value, and check it is there. */
void
catweasel_fillmem(catweasel_contr *c, unsigned char byte)
{
}

/* Return true if successful */
int
catweasel_init_controller(catweasel_contr *c, int iobase, int mk, char *fwname)
{
    return 1;
}

void
catweasel_detect_drive(catweasel_drive *d)
{
    /* assume track 0 sensor works until proven otherwise below */
    d->type = 1;

    d->track = 0;
}

void
catweasel_free_controller(catweasel_contr *c)
{
}

void
catweasel_select(catweasel_contr *c, int dr0, int dr1)
{
}

void
catweasel_set_motor(catweasel_drive *d, int on)
{
}

void
catweasel_seek(catweasel_drive *d, int t)
{
    d->track = t;
}

#if CHECK_DISK_CHANGED
int
catweasel_disk_changed(catweasel_drive *d)
{
    return 0;
}
#endif

int
catweasel_write_protected(catweasel_drive *d)
{
	return 0;
}

int
catweasel_read(catweasel_drive *d, int side, int clockmult, int time, int idx)
{
	char filename[32];
	FILE *fp;
	int i;

	// Newer dumps reset the sequence number
	if (prev_track != d->track || prev_side != side) {
		prev_track = d->track;
		prev_side = side;
		seq = 0;
	}

	//sprintf(filename, "dump3/disk2/c_s%dt%02d.%03d", side, d->track, seq++);
	//sprintf(filename, "dump4/fail38/c_s%dt%02d.%03d", side, d->track, seq++);
	sprintf(filename, "dump5/gp2/c_s%dt%02d.%03d", side, d->track, seq++);

	fp = fopen(filename, "rb");
	if (!fp) {
		fprintf(stderr, "Could not open '%s'.  Test divergence.\n", filename);
		exit(1);
	}
	for (i = 0; ;i++) {
		int ch = fgetc(fp);
		if (ch == EOF)
			break;

		trackbuf[i] = ch;
	}
	track_len = i;
	tidx = 0;

    return 1;
}

int
catweasel_write(catweasel_drive *d, int side, int clockmult, int time)
{
    return 1;
}

int catweasel_await_index(catweasel_drive *d)
{
    return 1;
}

void catweasel_set_hd(catweasel_contr *c, int hd)
{
}

int catweasel_get_byte(catweasel_contr *c)
{
	if (tidx < track_len)
		return trackbuf[tidx++];

	return -1;
}

int catweasel_put_byte(catweasel_contr *c, unsigned char val)
{
    return 0;
}

int
pci_find_catweasel(int index, int *cw_mk)
{
	*cw_mk = 4; // MK4 Catweasel
	return 1;
}
