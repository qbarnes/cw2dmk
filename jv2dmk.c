/*
 * jv2dmk: Convert a JV1 or JV3 format emulated floppy to DMK format.
 * Copyright (C) 2002 Timothy Mann
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
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "dmk.h"
#include "jv3.h"
#include "crc.c"

/* Command line options */
#define OUT_QUIET 0
#define OUT_TRACKS 1
#define OUT_SECTORS 2
int verbosity = OUT_QUIET;
int kind = 1;
int indexmark = 0;
int fmtimes = 2;
int dmklen = 0;
int jvwhat = 3;

/* Kinds */
#define NKINDS 4
typedef struct {
  char *name;
  int nomlen;
  int dmklen;
} KindData;

KindData kindData[] = {
  { "5.25-inch or 3.5-inch SD/DD disk", TRKSIZE_DD, DMK_TRACKLEN_5 },
  { "5.25-inch or 3.5-inch SD/DD disk", TRKSIZE_DD, DMK_TRACKLEN_5 },
  { "5.25-inch HD or 8-inch SD/DD disk", TRKSIZE_5HD, DMK_TRACKLEN_8 },
  { "3.5-inch HD disk", TRKSIZE_3HD, DMK_TRACKLEN_3HD }
};

typedef struct {
  unsigned char track;
  unsigned char sector;
  unsigned char flags;
  off_t offset;
  int sizecode;
} SectorId;

unsigned char damtab[] = { 0xfb, 0xfa, 0xf9, 0xf8, 0xfb, 0xf8, 0xfb, 0xf8 };
#define JV3_GETDAM(flags) (damtab[((flags) & (JV3_DENSITY|JV3_DAM)) >> 5])

unsigned char jv1_interleave[] = { 0, 5, 1, 6, 2, 7, 3, 8, 4, 9 };

SectorId idbuf[JV3_SECSMAX];

int
idcompare(const void* id1v, const void* id2v)
{
#define id1 ((SectorId*)id1v)
#define id2 ((SectorId*)id2v)
  if (id1->track < id2->track) return -1;
  if (id1->track > id2->track) return 1;
  if ((id1->flags & JV3_SIDE) < (id2->flags & JV3_SIDE)) return -1;
  if ((id1->flags & JV3_SIDE) > (id2->flags & JV3_SIDE)) return 1;
  if (id1->offset < id2->offset) return -1;
  if (id1->offset > id2->offset) return 1;
  return 0;
}

int
readidblock(FILE* fin, SectorId** curidp, off_t* offsetp)
{
  int i, c;
  SectorId* curid = *curidp;
  off_t offset = *offsetp;

  for (i=0; i<JV3_SECSPERBLK; i++) {
    c = getc(fin);
    if (c == EOF) return EOF;
    curid->track = c;
    curid->sector = getc(fin);
    curid->flags = getc(fin);
    curid->offset = offset;
    if (curid->track == JV3_FREE) {
      curid->sizecode = (curid->flags & JV3_SIZE) ^ 2;
    } else {
      curid->sizecode = (curid->flags & JV3_SIZE) ^ 1;
    }
    offset += 128 << curid->sizecode;
    curid++;
  }
  *curidp = curid;
  *offsetp = offset;
  return 0;
}

void
dmkput(unsigned char** dmkp, unsigned char byte, int count, int curden)
{
  if (!curden) count *= fmtimes;
  while (count--) *(*dmkp)++ = byte;
}

void
dotrack(FILE* fin, SectorId** curidp, int track, int side,
	int nomlen, int dmklen, unsigned char* dmkbuf)
{
  SectorId* curid = *curidp;
  int secs = 0;
  int lenleft = nomlen;
  int gap1, gap3, gap4;
  unsigned short* idamp = (unsigned short*) dmkbuf;
  unsigned char* dmkp = dmkbuf + DMK_TKHDR_SIZE;
  int curden, i;
  unsigned short crc;
  unsigned char dam;

  /* Count sectors on track and measure minimum size */
  if (verbosity == OUT_TRACKS) printf("track %2d side %d: ", track, side);
  while (curid->track == track &&
	 ((curid->flags & JV3_SIDE) ? 1 : 0) == side) {
    if (verbosity == OUT_TRACKS) {
      printf("%d ", curid->sector);
    } else if (verbosity == OUT_SECTORS) {
      printf("track %2d, side %d, sector %2d: size %d, %cden, dam %02x",
	     track, side, curid->sector, 128 << curid->sizecode,
	     (curid->flags & JV3_DENSITY) ? 'd' : 's',
	     JV3_GETDAM(curid->flags));
      if (curid->flags & JV3_ERROR) printf(", crc error");
      if (curid->flags & JV3_NONIBM) printf(", non-ibm [not supported]");
      printf("\n");
    }

    if (curid->flags & JV3_DENSITY) {
      lenleft -= (128 << curid->sizecode) + 62;
    } else {
      lenleft -= ((128 << curid->sizecode) + 33) * 2;
    }
    secs++;
    curid++;
  }
  if (verbosity == OUT_TRACKS) printf("\n");
  if (secs > DMK_TKHDR_SIZE/2) {
    fprintf(stderr, "jv2dmk: Too many sectors (%d) on track %d\n",
	    secs, track);
    exit(1);
  }

  curid = *curidp;
  curden = (curid->flags & JV3_DENSITY) != 0;
  if (indexmark) {
    /* Write IBM gap 0, index mark, and gap 1 */
    if (curden) {
      dmkput(&dmkp, 0x4e, 80, curden);
      dmkput(&dmkp, 0x00, 12, curden);
      dmkput(&dmkp, 0xc2, 3, curden);
      dmkput(&dmkp, 0xfc, 1, curden);
      dmkput(&dmkp, 0x4e, 50, curden);
    } else {
      dmkput(&dmkp, 0xff, 40, curden);
      dmkput(&dmkp, 0x00, 6, curden);
      dmkput(&dmkp, 0xfc, 1, curden);
      dmkput(&dmkp, 0xff, 26, curden);
    }
  } else {
    /* Write ISO gap 1 */
    if (curden) {
      dmkput(&dmkp, 0x4e, 32, curden);
    } else {
      dmkput(&dmkp, 0xff, 16, curden);
    }
  }
  gap1 = (dmkp - (dmkbuf + DMK_TKHDR_SIZE));

  /* Set gap4 to 1% of the total length to allow for 1%
   * drive speed variation. */
  gap4 = nomlen / 100;

  /* Divide remaining space into gap3's */
  lenleft -= gap1 + gap4;
  if (lenleft < 0) {
    fprintf(stderr, "jv2dmk: Physical track %d too long by %d bytes\n",
	    track, -lenleft);
    exit(1);
  }
  gap3 = lenleft / secs;

  while (secs--) {
    /* Write sync and IDAM */
    if (curden) {
      dmkput(&dmkp, 0x00, 12, curden);
      dmkput(&dmkp, 0xa1, 3, curden);
      crc = calc_crc1(0x968b, 0xa1); /* CRC of a1 a1 a1 */
      *idamp++ = (dmkp - dmkbuf) | DMK_DDEN_FLAG;
    } else {
      dmkput(&dmkp, 0x00, 6, curden);
      crc = 0xffff;
      *idamp++ = dmkp - dmkbuf;
    }
    dmkput(&dmkp, 0xfe, 1, curden);
    crc = calc_crc1(crc, 0xfe);

    /* Write ID */
    dmkput(&dmkp, track, 1, curden);
    crc = calc_crc1(crc, track);
    dmkput(&dmkp, side, 1, curden);
    crc = calc_crc1(crc, side);
    dmkput(&dmkp, curid->sector, 1, curden);
    crc = calc_crc1(crc, curid->sector);
    dmkput(&dmkp, curid->sizecode, 1, curden);
    crc = calc_crc1(crc, curid->sizecode);
    dmkput(&dmkp, crc >> 8, 1, curden);
    crc = calc_crc1(crc, crc >> 8);
    dmkput(&dmkp, crc >> 8, 1, curden);

    /* Write gap 2, sync, and DAM */
    if (curden) {
      dmkput(&dmkp, 0x4e, 22, curden);
      dmkput(&dmkp, 0x00, 12, curden);
      dmkput(&dmkp, 0xa1, 3, curden);
      crc = calc_crc1(0x968b, 0xa1); /* CRC of a1 a1 a1 */
    } else {
      dmkput(&dmkp, 0xff, 11, curden);
      dmkput(&dmkp, 0x00, 6, curden);
      crc = 0xffff;
    }
    dam = JV3_GETDAM(curid->flags);
    dmkput(&dmkp, dam, 1, curden);
    crc = calc_crc1(crc, dam);

    /* Write data */
    fseek(fin, curid->offset, SEEK_SET);
    i = 128 << curid->sizecode;
    while (i--) {
      unsigned char c = getc(fin);
      dmkput(&dmkp, c, 1, curden);
      crc = calc_crc1(crc, c);
    }

    /* Write data CRC */
    dmkput(&dmkp, crc >> 8, 1, curden);
    crc = calc_crc1(crc, crc >> 8);
    if (curid->flags & JV3_ERROR) crc = ~crc;
    dmkput(&dmkp, crc >> 8, 1, curden);

    /* Write gap 3.  Use density of next sector, if any */
    curid++;
    if (secs > 0) {
      curden = (curid->flags & JV3_DENSITY) != 0;
    }
    if (curden) {
      dmkput(&dmkp, 0x4e, gap3, curden);
    } else {
      dmkput(&dmkp, 0xff, gap3/2, curden);
    }
  }

  /* Write gap 4 */
  lenleft = dmklen - (dmkp - dmkbuf);
  if (lenleft < 0) {
    /* This can happen if the user specified too short a dmklength */
    fprintf(stderr, "jv2dmk: DMK track too long by %d bytes\n", -lenleft);
    exit(1);
  }
  dmkput(&dmkp, curden ? 0x4e : 0xff, lenleft, 1);

  *curidp = curid;
}

void
usage()
{
  printf("Usage: jv2dmk [options] file.dsk [file.dmk]\n");
  printf(" Options [defaults in brackets]:\n");
  printf(" -v verbosity  Amount of output [%d]\n", verbosity);
  printf("               0 = No output\n");
  printf("               1 = List of sectors on each track\n");
  printf("               2 = Details about each sector\n");
  printf(" -j which      Specify JV1 or JV3 [%d]\n", jvwhat);
  printf(" -k kind       Specify the type of media [%d]\n", kind);
  printf("               1 or 2 = %s\n", kindData[0].name);
  printf("               3 = %s\n", kindData[2].name);
  printf("               4 = %s\n", kindData[3].name);
  printf(" -i iam        1 = write IBM index address mark; 0 = don't [%d]\n",
	 indexmark);
  printf(" -w fmtimes    Write FM bytes 1 or 2 times [%d]\n", fmtimes);
  printf(" -l bytes      DMK track length in bytes [depends on kind]\n");
  exit(2);
}

int
main(int argc, char *argv[])
{
  char* jv_name = NULL;
  char* dmk_name = NULL;
  FILE* fin;
  FILE* fout;
  SectorId* curid;
  off_t offset;
  int i, totalsectors;
  int maxtrack, maxside, track, side;
  unsigned char *dmkbuf;
  dmk_header_t *dmkheader;
  int nomlen, buflen;
  int ch;

  opterr = 0;
  for (;;) {
    ch = getopt(argc, argv, "v:j:k:i:w:l:");
    if (ch == -1) break;
    switch (ch) {
    case 'v':
      verbosity = strtol(optarg, NULL, 0);
      if (verbosity < OUT_QUIET || verbosity > OUT_SECTORS) usage();
      break;
    case 'j':
      jvwhat = strtol(optarg, NULL, 0);
      if (jvwhat != 1 && jvwhat != 3) usage();
      break;
    case 'k':
      kind = strtol(optarg, NULL, 0);
      if (kind < 1 || kind > NKINDS) usage();
      break;
    case 'i':
      indexmark = strtol(optarg, NULL, 0);
      if (indexmark < 0 || indexmark > 1) usage();
      break;
    case 'w':
      fmtimes = strtol(optarg, NULL, 0);
      if (fmtimes < 1 || fmtimes > 2) usage();
      break;
    case 'l':
      dmklen = strtol(optarg, NULL, 0);
      if (dmklen < 0 || dmklen > 0x4000) usage();
      break;
    default:
      usage();
      break;
    }
  }

  switch (argc - optind) {
  case 2:
    /* Two filenames given */
    jv_name = argv[optind];
    dmk_name = argv[optind+1];
    break;

  case 1: {
    char *p;
    int len;

    jv_name = argv[optind];
    p = strrchr(jv_name, '.');
    if (p == NULL) {
      len = strlen(jv_name);
    } else {
      len = p - jv_name;
    }
    dmk_name = (char *) malloc(len + 5);
    sprintf(dmk_name, "%.*s.dmk", len, jv_name);
    break; }

  default:
    usage();
  }

  nomlen = kindData[kind-1].nomlen;
  if (dmklen == 0) dmklen = kindData[kind-1].dmklen;
  buflen = nomlen + DMK_TKHDR_SIZE;
  if (dmklen > buflen) buflen = dmklen;
  dmkbuf = (unsigned char*) calloc(1, buflen);
  dmkheader = (dmk_header_t *) calloc(1, sizeof(dmk_header_t));

  fin = fopen(jv_name, "rb");
  if (fin == NULL) {
    perror(jv_name);
    exit(1);
  }	
  fout = fopen(dmk_name, "w+b");
  if (fout == NULL) {
    perror(dmk_name);
    exit(1);
  }	

  if (jvwhat == 1) {
    /* Fake up some ids */
    fseek(fin, 0, SEEK_END);
    totalsectors = ftell(fin) / 256;
    maxside = 0;
    maxtrack = (totalsectors + 9) / 10 - 1;
    offset = 0;
    curid = idbuf;
    for (track=0; track<=maxtrack; track++) {
      for (i=0; i<10; i++) {
	curid->track = track;
	curid->sector = jv1_interleave[(i + 9*track) % 10];
	curid->flags = (track == 17) ? JV3_DAMSDFA : 0;
	curid->offset = offset + 256 * curid->sector;
	curid->sizecode = 1;
	curid++;
      }
      offset += 2560;
    }
    curid->track = curid->sector = curid->flags = JV3_FREE;
  } else {
    /* Read up to two blocks of ids */
    curid = idbuf;
    offset = JV3_SECSTART;
    for (i=0; i<2; i++) {
      if (readidblock(fin, &curid, &offset)) break;
      fseek(fin, offset, SEEK_SET);
      offset += JV3_SECSTART;
    }
    totalsectors = curid - idbuf;

    /* Sort the ids by track, side, offset. */
    qsort(idbuf, totalsectors, sizeof(SectorId), idcompare);

    /* Find number of tracks and sides */
    curid = idbuf;
    maxtrack = -1;
    maxside = 0;
    while (curid->track != JV3_FREE) {
      if (curid->track > maxtrack) maxtrack = curid->track;
      if (curid->flags & JV3_SIDE) maxside = 1;
      curid++;
    }
  }

  /* Write DMK header */
  dmkheader->ntracks = maxtrack + 1;
  dmkheader->tracklen = dmklen; 
  dmkheader->options = ((maxside == 0) ? DMK_SSIDE_OPT : 0)
                     + ((fmtimes == 1) ? DMK_SDEN_OPT : 0);
  /* assumes this machine is little-endian: */
  fwrite(dmkheader, sizeof(dmk_header_t), 1, fout);

  /* Write tracks */
  curid = idbuf;
  for (track=0; track<=maxtrack; track++) {
    for (side=0; side<=maxside; side++) {
      dotrack(fin, &curid, track, side, nomlen, dmklen, dmkbuf);
      fwrite(dmkbuf, dmklen, 1, fout);
    }
  }

  return 0;
}
