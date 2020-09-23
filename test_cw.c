/*
 * test_cw - mock Catweasel interface via .dmk disk image playback.
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
#include "dmk.h"

static FILE *dmk_file;
static dmk_header_t dmk_header;
static unsigned char *dmk_track;
static int dmk_idx;

static void damage(int phys_sector, int hit_data)
{
	int off = ((unsigned short *)dmk_track)[phys_sector] & DMK_IDAMP_BITS;

	off++;
	if (hit_data)
		off += 60;

	dmk_track[off] = ~dmk_track[off];
}

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
	dmk_file = fopen("mock.dmk", "rb");
	if (dmk_file == NULL) {
		perror("Could not open mock.dmk");
		return 0;
	}
	if (fread(&dmk_header, sizeof(dmk_header_t), 1, dmk_file) != 1) {
		fprintf(stderr, "error reading from mock.dmk\n");
		perror("");
		fclose(dmk_file);
		dmk_file = 0;
		return 0;
	}
	// Assume that the structure packing and endianess of this machine is correct.

	if ((dmk_header.writeprot != 0x00 && dmk_header.writeprot != 0xff) ||
    	dmk_header.mbz != 0)
	{
	    fprintf(stderr, "mock.dmk is not in DMK format\n");
		fclose(dmk_file);
		dmk_file = 0;
		return 0;
	}

	int sides = (dmk_header.options & DMK_SSIDE_OPT) ? 1 : 2;
	if (sides != 1) {
		fprintf(stderr, "mock.dmk is doubled sided, not supported.\n");
		fclose(dmk_file);
		dmk_file = 0;
		return 0;
	}
	printf("mock.dmk: %s density, tracks=%d, sides=%d, tracklen=0x%x\n",
		dmk_header.options & DMK_SDEN_OPT ? "single" : "double",
		dmk_header.ntracks, sides, dmk_header.tracklen);

	dmk_track = (unsigned char *)malloc(dmk_header.tracklen);
	dmk_idx = 0;

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
static int cnt=0;
    d->track = t;
	fseek(dmk_file, sizeof(dmk_header) + t * dmk_header.tracklen, SEEK_SET);
	if (fread(dmk_track, dmk_header.tracklen, 1, dmk_file) != 1)
		printf("error reading track %d\n", t);

	if (t == 1) {
		if (cnt & 1)
			damage(1, 0);
		else
			damage(2, 0);
	}
	cnt++;

	dmk_idx = DMK_TKHDR_SIZE;
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
	int b = dmk_track[dmk_idx++];
	if (dmk_idx > dmk_header.tracklen)
		dmk_idx = DMK_TKHDR_SIZE;

	return b;
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
