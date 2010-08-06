/*
 * dmk2jv3: Convert a DMK format emulated floppy to JV3 format if possible
 * Copyright (C) 2002 Timothy Mann
 * $Id: dmk2jv3.c,v 1.10 2010/01/15 19:28:46 mann Exp $
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
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "dmk.h"
#include "jv3.h"
#include "crc.c"

/* Command-line parameters */
#define OUT_MIN 0
#define OUT_QUIET 0
#define OUT_WARNINGS 1
#define OUT_SECTORS 2
#define OUT_MAX 2
int out_fmt = OUT_WARNINGS;

void usage()
{
  printf("\nUsage: dmk2jv3 [options] file.dmk [file.dsk]\n");
  printf(" Options [defaults in brackets]:\n");
  printf(" -v verbosity  Amount of output [%d]\n", out_fmt);
  printf("               0 = Print only errors\n");
  printf("               1 = Also print warnings\n");
  printf("               2 = Also print sector numbers found\n");
  exit(1);
}

void
dmk_read_header(FILE* dmk_file, dmk_header_t* dmk_header)
{
  int ret;
  rewind(dmk_file);
  /* assumes this machine is little-endian: */
  ret = fread(dmk_header, sizeof(dmk_header_t), 1, dmk_file);
  if (ret != 1) {
    fprintf(stderr, "dmk2jv3: Error reading from DMK file\n");
    perror("dmk2jv3");
    exit(1);
  }
}

void
dmk_inc(int *datap, int tracklen, int density, int fmtimes)
{
  *datap = *datap + (density ? 1 : fmtimes);
  if (*datap > tracklen) *datap = DMK_TKHDR_SIZE;
}

/* State for JV3 being written */
typedef struct {
  unsigned char track;
  unsigned char sector;
  unsigned char flags;
} SectorId;

typedef struct {
  int next;
  SectorId id[JV3_SECSMAX+1];
  int errcount;
  int warncount;
  long id2addr;
  int warnSize, warnIDCRC, warnMissingDAM;
} JV3State;

static JV3State jv3;

void
jv3_reset()
{
  jv3.next = 0;
  memset(jv3.id, 0xff, sizeof(jv3.id));
  jv3.errcount = 0;
  jv3.warncount = 0;
  jv3.id2addr = 0;
  jv3.warnSize = 0;
}

void
jv3_id(unsigned char track, unsigned char side,
       unsigned char sector, unsigned char sizecode,
       unsigned char dam, int density, int crcerror)
{
  SectorId id;

  if (jv3.next == JV3_SECSMAX) {
    printf("[Too many total sectors (%d)]\n", jv3.next + 1);
    jv3.errcount++;
    jv3.next++;
    return;
  }
  if (jv3.next == JV3_SECSMAX/2 && out_fmt >= OUT_WARNINGS) {
    printf("[Warning: too many total sectors for some emulators (> %d)]\n",
	   jv3.next);
    jv3.warncount++;
  }

  if (track >= JV3_TRACKSMAX) {
    printf("[Track number %u too large, using %u]\n", track, JV3_TRACKSMAX-1);
    jv3.errcount++;
    track = JV3_TRACKSMAX-1;
  }
  id.track = track;

  if (side > 1) {
    /* Can't really happen; main passes the physical side number here. */
    printf("[Side number 0x%02x too large, using %u]\n", side, side & 1);
    jv3.errcount++;
    side = side & 1;
  }
  id.flags = side ? JV3_SIDE : 0;

  id.sector = sector;

  if (sizecode > 3) {
    printf("[Sector size code 0x%02x too large, using %d]\n",
	   sizecode, sizecode & 3);
    jv3.errcount++;
    sizecode = sizecode & 3;
  }
  if (sizecode != 1 && out_fmt >= OUT_WARNINGS && !jv3.warnSize) {
    printf("[Warning: sector size %d not supported by some emulators]\n",
	   128 << sizecode);
    jv3.warncount++;
    jv3.warnSize = 1;
  }
  id.flags |= (sizecode ^ 1);
    
  if (density == 0) {
    /* Single density */
    switch (dam) {
    case 0xf8:
      id.flags |= JV3_DAMSDF8;
      break;
    case 0xf9:
      id.flags |= JV3_DAMSDF9;
      break;
    case 0xfa:
      id.flags |= JV3_DAMSDFA;
      break;
    case 0xfb:
      id.flags |= JV3_DAMSDFB;
      break;
    default:
      printf("[Single density DAM 0x%02x not supported, using 0xf8]\n", dam);
      jv3.errcount++;
      id.flags |= JV3_DAMSDF8;
      break;
    }
  } else {
    /* Double density */
    id.flags |= JV3_DENSITY;
    switch (dam) {
    case 0xf8:
      id.flags |= JV3_DAMDDF8;
      break;
    case 0xfb:
      id.flags |= JV3_DAMDDFB;
      break;
    default:
      printf("[Double density DAM 0x%02x not supported, using 0xf8]\n", dam);
      jv3.errcount++;
      id.flags |= JV3_DAMDDF8;
      break;
    }
  }

  if (crcerror) id.flags |= JV3_ERROR;

  jv3.id[jv3.next++] = id;
}

int
main(int argc, char** argv)
{
  char* dmk_name = NULL;
  char* jv3_name = NULL;
  FILE* dmk_file;
  FILE* jv3_file;
  dmk_header_t dmk_header;
  unsigned char* dmk_track;
  int ch, ret;
  int track, side, sides, fmtimes, rx02;
  int idampp, idamp, datap;
  int density;
  int dam_min, dam_range;

  opterr = 0;
  for (;;) {
    ch = getopt(argc, argv, "v:");
    if (ch == -1) break;
    switch (ch) {
    case 'v':
      out_fmt = strtol(optarg, NULL, 0);
      if (out_fmt < OUT_MIN || out_fmt > OUT_MAX) usage();
      break;
    default:
      usage();
      break;
    }
  }

  switch (argc - optind) {
  case 2:
    /* Two filenames given */
    dmk_name = argv[optind];
    jv3_name = argv[optind+1];
    break;

  case 1: {
    char *p;
    int len;

    dmk_name = argv[optind];
    p = strrchr(dmk_name, '.');
    if (p == NULL) {
      len = strlen(dmk_name);
    } else {
      len = p - dmk_name;
    }
    jv3_name = (char *) malloc(len + 5);
    sprintf(jv3_name, "%.*s.dsk", len, dmk_name);
    break; }

  default:
    usage();
  }

  dmk_file = fopen(dmk_name, "rb");
  if (dmk_file == NULL) {
    perror(dmk_name);
    exit(1);
  }

  /* Set DMK parameters */
  dmk_read_header(dmk_file, &dmk_header);
  if ((dmk_header.writeprot != 0x00 && dmk_header.writeprot != 0xff) ||
      dmk_header.mbz != 0) {
    fprintf(stderr, "dmk2jv3: File is not in DMK format\n");
    exit(1);
  }
  sides = (dmk_header.options & DMK_SSIDE_OPT) ? 1 : 2;
  fmtimes = (dmk_header.options & DMK_SDEN_OPT) ? 1 : 2;
  rx02 = (dmk_header.options & DMK_RX02_OPT) ? 1 : 0;
  dmk_track = (unsigned char*) malloc(dmk_header.tracklen);
  if (out_fmt >= OUT_SECTORS) {
    printf("[tracks=%d, sides=%d, fmtimes=%d, rx02=%d, tracklen=0x%x]\n",
	   dmk_header.ntracks, sides, fmtimes, rx02, dmk_header.tracklen);
  }

  if (rx02) {
    fprintf(stderr, "dmk2jv3: JV3 does not support RX02 encoding\n");
    exit(1);
  }

  jv3_file = fopen(jv3_name, "wb");
  if (jv3_file == NULL) {
    perror(jv3_name);
    exit(1);
  }
  fseek(jv3_file, JV3_SECSTART, 0);
  jv3_reset();

  /* Loop through tracks */
  for (track=0; track<dmk_header.ntracks; track++) {

    /* Loop through sides */
    for (side=0; side<sides; side++) {
      /* Read DMK track data */
      ret = fread(dmk_track, dmk_header.tracklen, 1, dmk_file);
      if (ret != 1) {
	if (errno == 0) {
	  printf("[End of file on input]\n");
	  break;
	}
	fprintf(stderr, "dmk2jv3: Error reading from DMK file\n");
	perror("dmk2jv3");
	exit(1);
      }

      if (out_fmt >= OUT_SECTORS) {
	printf("Track %d, side %d, sector: ", track, side);
      }

      /* Loop through ids */
      idampp = 0;
      for (;;) {
	idamp = dmk_track[idampp++];
	idamp += dmk_track[idampp++] << 8;
	if (idamp == 0 || idamp == 0xffff) break;
	density = (idamp & DMK_DDEN_FLAG) != 0;
	idamp &= DMK_IDAMP_BITS;

	/* Project where DAM will be */
	if (!density) {
	  dam_min = idamp + 7 * fmtimes;
	  dam_range = 30 * fmtimes;  /* ref 1791 datasheet */
	} else {
	  dam_min = idamp + 7;
	  dam_range = 43;  /* ref 1791 datasheet */
	}

	/* Decode an id block if idamp is valid */
	datap = idamp;
	if (dmk_track[datap] == 0xfe) {
	  unsigned char ltrack, lside, sector, sizecode, dam = 0;
	  int crcerror = 0, size;
	  unsigned short crc;
#         define DMK_INC(d) dmk_inc(&d, dmk_header.tracklen, density, fmtimes)

	  /* Start ID CRC check */
	  if (density == 0) {
	    crc = 0xffff;
	  } else {
	    crc = calc_crc1(0x968b, 0xa1); /* CRC of a1 a1 a1 */
	  }
	  crc = calc_crc1(crc, 0xfe);
	  DMK_INC(datap);
	  ltrack = dmk_track[datap];
	  crc = calc_crc1(crc, ltrack);
	  DMK_INC(datap);
	  lside = dmk_track[datap];
	  crc = calc_crc1(crc, lside);
	  DMK_INC(datap);
	  sector = dmk_track[datap];
	  crc = calc_crc1(crc, sector);
	  DMK_INC(datap);
	  sizecode = dmk_track[datap];
	  crc = calc_crc1(crc, sizecode);
	  size = 128 << (sizecode & 3);

	  if (out_fmt >= OUT_SECTORS) {
	    printf(" %d", sector);
	  }

	  if (ltrack != track) {
	    printf("[False track number 0x%02x not supported]\n", ltrack);
	    jv3.errcount++;
	  }

	  if (lside != side && out_fmt >= OUT_WARNINGS) {
	    printf("[Warning: False side number 0x%02x not supported, "
		   "using %d]\n", lside, side);
	    jv3.warncount++;
	  }

	  /* Check ID CRC */
	  DMK_INC(datap);
	  crc = calc_crc1(crc, dmk_track[datap]);
	  DMK_INC(datap);
	  crc = calc_crc1(crc, dmk_track[datap]);
	  if (crc != 0) {
	    printf("[Recording ID CRC error as data CRC error]\n");
	    jv3.errcount++;
	    crcerror = 1;
	  }

	  /* Look for dam */
	  datap = dam_min;
	  while (--dam_range >= 0) {
	    dam = dmk_track[datap];
	    DMK_INC(datap);
	    if (dam >= 0xf8 && dam <= 0xfb) break;
	  }
	  if (dam_range < 0) {
	    printf("[Recording missing DAM as data CRC error]\n");
	    jv3.errcount++;
	    dam = 0xfb;
	  }

	  /* Start data CRC check */
	  if (density == 0) {
	    crc = 0xffff;
	  } else {
	    crc = calc_crc1(0x968b, 0xa1); /* CRC of a1 a1 a1 */
	  }
	  crc = calc_crc1(crc, dam);

	  /* Write out data */
	  while (size--) {
	    putc(dmk_track[datap], jv3_file);
	    crc = calc_crc1(crc, dmk_track[datap]);
	    DMK_INC(datap);
	  }

	  /* Check data CRC */
	  crc = calc_crc1(crc, dmk_track[datap]);
	  DMK_INC(datap);
	  crc = calc_crc1(crc, dmk_track[datap]);
	  if (crc != 0) {
	    crcerror = 1;
	  }

	  if (out_fmt >= OUT_SECTORS && crcerror) {
	    printf("?");
	  }

	  /* Remember id info */
	  jv3_id(track, side, sector, sizecode, dam, density, crcerror);

	  /* Leave space for second id block if needed */
	  if (jv3.next == JV3_SECSPERBLK) {
	    jv3.id2addr = ftell(jv3_file);
	    fseek(jv3_file, jv3.id2addr + JV3_SECSTART, 0);
	  }
	}
      }

      if (out_fmt >= OUT_SECTORS) {
	printf("\n");
	fflush(stdout);
      }
    }
  }

  /* Write out id blocks and writeprot flag */
  fseek(jv3_file, 0, 0);
  ret = fwrite(jv3.id, JV3_SECSTART - 1, 1, jv3_file);
  if (ret != 1) {
    fprintf(stderr, "dmk2jv3: Error writing to JV3 file\n");
    perror("dmk2jv3");
    exit(1);
  }
  fputc(0xff, jv3_file);
  if (jv3.id2addr) {
    fseek(jv3_file, jv3.id2addr, 0);
    fwrite(((unsigned char*) jv3.id) + JV3_SECSTART - 1,
	   JV3_SECSTART, 1, jv3_file);
  }
  if (out_fmt > OUT_QUIET || jv3.errcount > 0 || jv3.warncount > 0) {
    printf("%d total errors, %d total warnings\n",
	   jv3.errcount, jv3.warncount);
  }
  return 0;
}
