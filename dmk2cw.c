/*
 * dmk2cw: Write a .dmk to a real floppy disk using the Catweasel.
 * Copyright (C) 2001 Timothy Mann
 * $Id: dmk2cw.c,v 1.12 2004/06/05 08:39:05 mann Exp $
 *
 * Depends on Linux Catweasel driver code by Michael Krause
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

/*#define DEBUG1 1*/
#define DEBUG6 1
/*#define DEBUG7 1*/
/*#define DEBUG8 1*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <sys/time.h>
#if linux
#include <sys/io.h>
#endif
#include "cwfloppy.h"
#include "dmk.h"
#include "kind.h"
#include "cwpci.h"
#include "version.h"

struct catweasel_contr c;

/* Command-line parameters */
#define OUT_MIN 0
#define OUT_QUIET 0
#define OUT_NORMAL 1
#define OUT_BYTES 2
#define OUT_SAMPLES 3
#define OUT_MAX 3
int out_fmt = OUT_NORMAL;
int port = 0;
int drive = 0;
int kind = 0;
int steps = 1;
int maxsides = 2;
int sides = 1;
int fmtimes = 2;
int rx02 = 0;
double precomp = 140; /*ns*/
int cwclock = 2;
int hd = 2;
int ignore = 0;
int iam_pos = -1;
int testmode = -1;

void usage()
{
  printf("\nUsage: dmk2cw [options] file.dmk\n");
  printf("\n Options [defaults in brackets]:\n");
  printf(" -d drive      Drive unit, 0 or 1 [%d]\n", drive);
  printf(" -v verbosity  Amount of output [%d]\n", out_fmt);
  printf("               0 = No output\n");
  printf("               1 = Print track and side being written\n");
  printf("               2 = Also dump bytes and encodings\n");
  printf("               3 = Also dump samples\n");
  printf(" -k kind       Kind of disk and drive [%d]\n", kind);
  printf("               0 = guess\n");
  printf("               1 = %s\n", kinds[0].description);
  printf("               2 = %s\n", kinds[1].description);
  printf("               3 = %s\n", kinds[2].description);
  printf("               4 = %s\n", kinds[3].description);
  printf(" -m steps      Step multiplier, 1 or 2 [%d]\n", steps);
  printf(" -s maxsides   Maximum number of sides, 1 or 2 [%d]\n", maxsides);
  printf("\nThese values normally need not be changed:\n");
  printf(" -p port       I/O port base (MK1) or card number (MK3) [%d]\n",
	 port);
  printf(" -c clock      Catweasel clock multiplier [%d]\n", cwclock);
  printf(" -o precomp    Amount of write-precompensation (ns) [%g]\n",
	 precomp);
  printf(" -h hd         HD output; 0=low, 1=high [according to -k]\n");
  printf(" -g ign        Ignore first ign bytes of track [%d]\n", ignore);
  printf(" -i ipos       Force IAM to ipos from track start; "
	 "if -1, don't [%d]\n", iam_pos);
#if DEBUG6
  printf(" -y testmode   Activate various test modes [%d]\n", testmode);
#endif
  printf("\n");
  exit(1);
}

void
cleanup()
{
  catweasel_free_controller(&c);
}

void
handler(int sig)
{
  cleanup();
  signal(sig, SIG_DFL);
  kill(getpid(), sig);
}

void
dmk_read_header(FILE* dmk_file, dmk_header_t* dmk_header)
{
  int ret;
  rewind(dmk_file);
  /* assumes this machine is little-endian: */
  ret = fread(dmk_header, sizeof(dmk_header_t), 1, dmk_file);
  if (ret != 1) {
    fprintf(stderr, "dmk2cw: Error reading from DMK file\n");
    perror("dmk2cw");
    cleanup();
    exit(1);
  }
}


double
cw_measure_rpm(catweasel_drive *d)
{
  struct timeval t1, t2;
  long usec;

  catweasel_await_index(d);
  gettimeofday(&t1, NULL);
  catweasel_await_index(d);
  gettimeofday(&t2, NULL);
  usec = (t2.tv_sec - t1.tv_sec)*1000000 + t2.tv_usec - t1.tv_usec;
  return 60000000.0/(double)usec;
}

#define CW_BIT_INIT -1
#define CW_BIT_FLUSH -2
/* !!Could also dither to get more exact average clock rate */
void
cw_bit(int bit, double mult)
{
  static int len, nextlen;
  static double prevadj, adj;
  int val;

#if DEBUG7
  if (len > 4) {
    printf("?");
  }
#endif

  switch (bit) {
  case CW_BIT_INIT:
    len = 0;
    nextlen = -1;
    prevadj = 0.0;
    adj = 0.0;
    break;
  case 0:
    nextlen++;
    break;
  case 1:
    nextlen++;
    if (len > 0) {
      if (len == 2 && nextlen > 2) {
	adj = -(precomp * CWHZ/1000000000.0);
      } else if (len > 2 && nextlen == 2) {
	adj =  (precomp * CWHZ/1000000000.0);
      } else {
	adj = 0.0;
      }
      val = 129 - (int) (len * mult - prevadj + adj + 0.5);
      if (out_fmt >= OUT_SAMPLES) {
	printf("/%d:%d", len, val);
      }
      if (val >= 0 && val <= 127) {
	catweasel_put_byte(&c, val);
      }
      prevadj = adj;
    }
    len = nextlen;
    nextlen = 0;
    break;
  case CW_BIT_FLUSH:
    val = 129 - (int) (nextlen * mult - prevadj + 0.5);
    if (out_fmt >= OUT_SAMPLES) {
      printf("/%d:%d", nextlen, val);
    }
    if (val >= 0 && val <= 127) {
      catweasel_put_byte(&c, val);
    }
    cw_bit(CW_BIT_INIT, mult);
    break;
  }
}

/* RX02 encoding requires some buffering
   to encode data sequence 011110 specially */
#define RX02_BITPAIR_INIT -1
#define RX02_BITPAIR_FLUSH -2
void
rx02_bitpair(int bit, double mult)
{
  static int accum = 0;
  static int bitcount = 0;
  if (bit == RX02_BITPAIR_INIT) {
    accum = 0;
    bitcount = 0;
  } else if (bit == RX02_BITPAIR_FLUSH) {
    while (bitcount) {
      bitcount--;
      cw_bit((accum & (3<<bitcount)) == 0, mult); /* clock bit */
      cw_bit((accum & (1<<bitcount)) != 0, mult); /* data bit */
    }
  } else {
    accum = (accum << 1) | bit;
    bitcount++;
    if (bitcount == 5) {
      if ((accum & 0x3f) == 0x1e) {
	/* Special RX02 encoding for data bit sequence (0)11110: not
	   the normal MFM (x0)0101010100, but (x0)1000100010.
	   Parentheses in this notation mean that those bits precede
	   this sequence but have already been encoded.  A virtual 0
	   bit precedes the first bit in each sector, so the case of a
	   sector beginning with bits 11110 is handled here.  However,
	   we encode a 0xff leadout after the last CRC byte of each
	   sector, so the case of ending with 01111 does not arise. */
	cw_bit(1, mult);
	cw_bit(0, mult);
	cw_bit(0, mult);
	cw_bit(0, mult);
	cw_bit(1, mult);
	cw_bit(0, mult);
	cw_bit(0, mult);
	cw_bit(0, mult);
	cw_bit(1, mult);
	cw_bit(0, mult);
	bitcount = 0;
      } else {
	cw_bit((accum & (3<<4)) == 0, mult); /* clock bit */
	cw_bit((accum & (1<<4)) != 0, mult); /* data bit */
	bitcount--;
      }
    }
  }
}

int
approx(int a, int b)
{
  /* Return 1 if a is approximately equal to b (within 4% of b) */
  int diff = (a > b) ? (a - b) : (b - a);
  return ((float) diff / (float) b) < 0.04;
}


/* Cleanup parameters */
#define MFM_GAP3Z 8 /*12 nominal, but that is too many.  3 is too few! */
#define FM_GAP3Z  4 /*6 nominal */

/* Byte encodings */
#define SKIP    0  /* padding byte in FM area of a DMK */
#define FM      1  /* FM with FF clock */
#define FM_IAM  2  /* FM with D7 clock (IAM) */
#define FM_AM   3  /* FM with C7 clock (IDAM or DAM) */
#define MFM     4  /* MFM with normal clocking algorithm */
#define MFM_IAM 5  /* MFM C2 with missing clock */
#define MFM_AM  6  /* MFM A1 with missing clock */
#define RX02    7  /* DEC-modified MFM as in RX02 */

int
main(int argc, char** argv)
{
  FILE* dmk_file;
  dmk_header_t dmk_header;
  unsigned char* dmk_track;
  unsigned char* dmk_encoding;
  kind_desc* kd;
  int ch, ret, i;
  int track, side;
  int idampp, first_idamp, idamp, next_idamp, datap;
  int encoding, next_encoding;
  int dam_min, dam_max, got_iam, skip;
  int byte, clock_byte, bit, prev_bit;
  double mult;
  int rx02_data = 0;
  int cw_mk = 1;

  opterr = 0;
  for (;;) {
    ch = getopt(argc, argv, "p:d:v:k:m:s:o:c:h:g:i:y:");
    if (ch == -1) break;
    switch (ch) {
    case 'p':
      port = strtol(optarg, NULL, 16);
      if (port < 0 || (port >= MK3_MAX_CARDS && port < MK1_MIN_PORT) ||
	  (port > MK1_MAX_PORT)) {
	fprintf(stderr, "dmk2cw: -p must be between %d and %d for MK3 cards,\n"
		"  or between 0x%x and 0x%x for MK1 cards.\n",
		0, MK3_MAX_CARDS-1, MK1_MIN_PORT, MK1_MAX_PORT);
	exit(1);
      }
      break;
    case 'd':
      drive = strtol(optarg, NULL, 0);
      if (drive < 0 || drive > 1) usage();
      break;
    case 'v':
      out_fmt = strtol(optarg, NULL, 0);
      if (out_fmt < OUT_MIN || out_fmt > OUT_MAX) usage();
      break;
    case 'k':
      kind = strtol(optarg, NULL, 0);
      if (kind < 0 || kind > NKINDS) usage();
      break;
    case 'm':
      steps = strtol(optarg, NULL, 0);
      if (steps < 1 || steps > 2) usage();
      break;
    case 's':
      maxsides = strtol(optarg, NULL, 0);
      if (maxsides < 1 || maxsides > 2) usage();
      break;
    case 'o':
      precomp = strtod(optarg, NULL);
      if (precomp < 0.0) usage();
      break;
    case 'c':
      cwclock = strtol(optarg, NULL, 0);
      if (cwclock != 1 && cwclock != 2 && cwclock != 4) usage();
      break;
    case 'h':
      hd = strtol(optarg, NULL, 0);
      if (hd < 0 || hd > 2) usage();
      break;
    case 'g':
      ignore = strtol(optarg, NULL, 0);
      break;
    case 'i':
      iam_pos = strtol(optarg, NULL, 0);
      break;
#if DEBUG6
    case 'y':
      testmode = strtol(optarg, NULL, 0);
      break;
#endif
    default:
      usage();
      break;
    }
  }

  if (optind >= argc) {
    usage();
  }
  if (ignore != 0 && iam_pos != -1) {
    fprintf(stderr, "dmk2cw: -g and -i cannot be used together\n");
    usage();
  }

  /* Keep drive from spinning endlessly on (expected) signals */
  signal(SIGHUP, handler);
  signal(SIGINT, handler);
  signal(SIGQUIT, handler);
  signal(SIGPIPE, handler);
  signal(SIGTERM, handler);

  if (out_fmt > OUT_NORMAL) {
    printf("Version: %s  Command line:\n", VERSION);
    for (i = 0; i < argc; i++) {
      printf("%s ", argv[i]);
    }
    printf("\n");
  }

  /* Start Catweasel */
  if (port < 10) {
    cw_mk = 3;
    port = pci_find_catweasel(port);
    if (port == -1) {
      cw_mk = 1;
      port = MK1_DEFAULT_PORT;
      printf("Failed to detect Catweasel MK3 on PCI bus; "
	     "looking for MK1 on ISA bus at 0x%x\n", port);
      fflush(stdout);
    }
  }
#if linux
  if ((cw_mk == 1 && ioperm(port, 8, 1) == -1) ||
      (cw_mk == 3 && iopl(3) == -1)) {
    fprintf(stderr, "dmk2cw: No access to I/O ports\n");
    return 1;
  }
  setuid(getuid());
#endif
  catweasel_init_controller(&c, port, cw_mk);
  ret = catweasel_memtest(&c);
  if (ret) {
    if (out_fmt >= OUT_QUIET) {
      printf("Detected Catweasel MK%d at port 0x%x\n", cw_mk, port);
      fflush(stdout);
    }
  } else {
    fprintf(stderr, "dmk2cw: Failed to detect Catweasel at port 0x%x\n",
	    port);
    return 1;
  }
  if (cw_mk == 1 && cwclock == 4) {
    fprintf(stderr, "dmk2cw: Catweasel MK1 does not support 4x clock\n");
    cleanup();
    return 1;
  }
  catweasel_detect_drive(&c.drives[drive]);

  /* Error if drive not detected */
  if (c.drives[drive].type == 0) {
    catweasel_detect_drive(&c.drives[1 - drive]);
    if (c.drives[1 - drive].type == 0) {
      fprintf(stderr, "dmk2cw: Failed to detect any drives\n");
    } else {
      fprintf(stderr, "dmk2cw: Drive %d was not detected, but drive %d was.\n"
	      "You can give the -d%d option to use drive %d.\n",
	      drive, 1-drive, 1-drive, 1-drive);
    }
    cleanup();
    return 1;
  }

  /* Open input file */
  dmk_file = fopen(argv[optind], "rb");
  if (dmk_file == NULL) {
    perror(argv[optind]);
    cleanup();
    exit(1);
  }

  /* Set DMK parameters */
  dmk_read_header(dmk_file, &dmk_header);
  if ((dmk_header.writeprot != 0x00 && dmk_header.writeprot != 0xff) ||
      dmk_header.mbz != 0) {
    fprintf(stderr, "dmk2cw: File is not in DMK format\n");
    cleanup();
    exit(1);
  }
  sides = (dmk_header.options & DMK_SSIDE_OPT) ? 1 : 2;
  fmtimes = (dmk_header.options & DMK_SDEN_OPT) ? 1 : 2;
  rx02 = (dmk_header.options & DMK_RX02_OPT) ? 1 : 0;
  dmk_track = (unsigned char*) malloc(dmk_header.tracklen);
  dmk_encoding = (unsigned char*) malloc(dmk_header.tracklen);

  /* Select drive, start motor, wait for spinup */
  catweasel_select(&c, !drive, drive);
  catweasel_set_motor(&c.drives[drive], 1);
  catweasel_usleep(500000);

  if (catweasel_write_protected(&c.drives[drive])) {
    fprintf(stderr, "dmk2cw: Disk is write-protected\n");
    cleanup();
    return 1;
  }

  /* Detect kind if needed */
  if (kind == 0) {
    double rpm = cw_measure_rpm(&c.drives[drive]);
#if DEBUG8
    printf("drive rpm approx %f\n", rpm);
    printf("dmk track length %d\n", dmk_header.tracklen);
#endif
    if (rpm > 342.0 && rpm < 378.0) {
      /* about 360 RPM */
      if (approx(dmk_header.tracklen, DMK_TRACKLEN_5) ||
	  approx(dmk_header.tracklen, DMK_TRACKLEN_5SD)) {
	kind = 1;
      } else if (approx(dmk_header.tracklen, DMK_TRACKLEN_8) ||
		 approx(dmk_header.tracklen, DMK_TRACKLEN_8SD)) {
	kind = 3;
      }
    } else if (rpm > 285.0 && rpm < 315.0) {
      /* about 300 RPM */
      if (approx(dmk_header.tracklen, DMK_TRACKLEN_5) ||
	  approx(dmk_header.tracklen, DMK_TRACKLEN_5SD)) {
	kind = 2;
      } else if (approx(dmk_header.tracklen, DMK_TRACKLEN_3HD)) {
	kind = 4;
      }
    }
    if (kind == 0) {
      fprintf(stderr, "dmk2cw: Failed to guess drive kind; use -k\n");
      exit(1);
    } else {
      kd = &kinds[kind-1];
      if (out_fmt > OUT_QUIET) printf("Guessing %s\n", kd->description);
    }
  } else {
    kd = &kinds[kind-1];
  }
  mult = (kd->mfmshort / 2.0) * cwclock;
  if (hd == 2) {
    hd = kd->hd;
  }

  /* Loop through tracks */
  for (track=0; track<dmk_header.ntracks; track++) {
    catweasel_seek(&c.drives[drive], track * steps);

    /* Loop through sides */
    for (side=0; side<sides; side++) {
      /* Read DMK track data */
      ret = fread(dmk_track, dmk_header.tracklen, 1, dmk_file);
      if (ret != 1) {
	fprintf(stderr, "dmk2cw: Error reading from DMK file\n");
	perror("dmk2cw");
	cleanup();
	exit(1);
      }
      if (testmode >= 0 && testmode <= 0xff) {
	memset(dmk_track + sizeof(dmk_header), testmode,
	       dmk_header.tracklen - sizeof(dmk_header));
      }

      /* Determine encoding for each byte and clean up */
      idampp = 0;
      idamp = 0;
      dam_min = 0;
      dam_max = 0;
      got_iam = 0;
      skip = 0;

      /* First IDAM pointer has some special uses; need to get it here */
      next_idamp = dmk_track[idampp++];
      next_idamp += dmk_track[idampp++] << 8;
      if (next_idamp == 0 || next_idamp == 0xffff) {
	next_encoding = FM;
	next_idamp = 0x7fff;
	first_idamp = 0;
      } else {
	next_encoding = (next_idamp & DMK_DDEN_FLAG) ? MFM : FM;
	next_idamp &= DMK_IDAMP_BITS;
	first_idamp = next_idamp;
      }
      encoding = next_encoding;

      /* Check if writing to side 1 (the second side) of a 1-sided drive */
      if (side > maxsides) {
	if (first_idamp == 0) {
	  /* No problem; there is nothing to write here */
	  break;
	}
	fprintf(stderr,	"dmk2cw: Drive is 1-sided but DMK file is 2-sided\n");
	cleanup();
	exit(1);
      }

      if (out_fmt >= OUT_NORMAL) {
	printf("Track %d, side %d", track, side);
	if (out_fmt >= OUT_BYTES) {
	  printf("\n");
	} else {
	  printf("\r");
	}
	fflush(stdout);
      }

      /* Loop through data bytes */
      for (datap = DMK_TKHDR_SIZE; datap < dmk_header.tracklen; datap++) {
	if (datap >= next_idamp) {
	  /* Read next IDAM pointer */
	  idamp = next_idamp;
	  encoding = next_encoding;
	  next_idamp = dmk_track[idampp++];
	  next_idamp += dmk_track[idampp++] << 8;
	  if (next_idamp == 0 || next_idamp == 0xffff) {
	    next_encoding = encoding;
	    next_idamp = 0x7fff;
	  } else {
	    next_encoding = (next_idamp & DMK_DDEN_FLAG) ? MFM : FM;
	    next_idamp &= DMK_IDAMP_BITS;
	  }

	  /* Project where DAM will be */
	  if (encoding == FM) {
	    dam_min = idamp + 7 * fmtimes;
	    dam_max = dam_min + 30 * fmtimes;  /* ref 1791 datasheet */
	  } else {
	    dam_min = idamp + 7;
	    dam_max = dam_min + 43;  /* ref 1791 datasheet */
	  }
	}

	/* Choose encoding */
	if (datap == idamp && dmk_track[datap] == 0xfe) {
	  /* ID address mark */
	  skip = 1;
	  if (encoding == FM) {
	    dmk_encoding[datap] = FM_AM;
	    /* Cleanup: precede mark with some FM 00's */
	    for (i = datap-1;
		 i >= DMK_TKHDR_SIZE && i >= datap - FM_GAP3Z*fmtimes; i--) {
	      dmk_track[i] = 0;
	      if (fmtimes == 2 && (i&1)) {
		dmk_encoding[i] = SKIP;
	      } else {
		dmk_encoding[i] = FM;
	      }
	    }
	  } else {
	    dmk_encoding[datap] = encoding;
	    /* Cleanup: precede mark with 3 MFM A1's with missing clocks,
	       and some MFM 00's before that */
	    for (i = datap-1; i >= DMK_TKHDR_SIZE && i >= datap-3; i--) {
	      dmk_track[i] = 0xA1;
	      dmk_encoding[i] = MFM_AM;
	    }
	    for (; i >= DMK_TKHDR_SIZE && i >= datap-3 - MFM_GAP3Z; i--) {
	      dmk_track[i] = 0x00;
	      dmk_encoding[i] = MFM;
	    }
	  }

	} else if (datap >= dam_min && datap <= dam_max &&
		   dmk_track[datap] >= 0xf8 && dmk_track[datap] <= 0xfd) {
	  /* Data address mark */
	  dam_max = 0;  /* prevent detecting again inside data */
	  skip = 1;
	  if (encoding == FM) {
	    dmk_encoding[datap] = FM_AM;
	    /* Cleanup: precede mark with some FM 00's */
	    for (i = datap-1;
		 i >= DMK_TKHDR_SIZE && i >= datap - FM_GAP3Z*fmtimes; i--) {
	      dmk_track[i] = 0;
	      if (fmtimes == 2 && (i&1)) {
		dmk_encoding[i] = SKIP;
	      } else {
		dmk_encoding[i] = FM;
	      }
	    }
	  } else {
	    dmk_encoding[datap] = encoding;
	    /* Cleanup: precede mark with 3 MFM A1's with missing clocks,
	       and some MFM 00's before that */
	    for (i = datap-1; i >= DMK_TKHDR_SIZE && i >= datap-3; i--) {
	      dmk_track[i] = 0xA1;
	      dmk_encoding[i] = MFM_AM;
	    }
	    for (; i >= DMK_TKHDR_SIZE && i >= datap-3 - MFM_GAP3Z; i--) {
	      dmk_track[i] = 0x00;
	      dmk_encoding[i] = MFM;
	    }
	  }
	  
	  /* Prepare to switch to RX02-modified MFM if needed */
	  if (rx02 && (dmk_track[datap] == 0xf9 ||
		       dmk_track[datap] == 0xfd)) {
	    rx02_data = 2 + (256 << dmk_track[idamp+4]);
	    /* Follow CRC with one RX02-MFM FF */
	    dmk_track[fmtimes + datap + rx02_data++] = 0xff;
	  }

	} else if (datap > DMK_TKHDR_SIZE && datap <= first_idamp
		   && !got_iam && dmk_track[datap] == 0xfc &&
		   ((encoding == MFM && dmk_track[datap-1] == 0xc2) ||
		    (encoding == FM && (dmk_track[datap-2] == 0x00 ||
					dmk_track[datap-2] == 0xff)))) {
	  /* Index address mark */
	  got_iam = datap;
	  skip = 1;
	  if (encoding == FM) {
	    dmk_encoding[datap] = FM_IAM;
	    /* Cleanup: precede mark with some FM 00's */
	    for (i = datap-1;
		 i >= DMK_TKHDR_SIZE && i >= datap - FM_GAP3Z*fmtimes; i--) {
	      dmk_track[i] = 0;
	      if (fmtimes == 2 && (i&1)) {
		dmk_encoding[i] = SKIP;
	      } else {
		dmk_encoding[i] = FM;
	      }
	    }
	  } else {
	    dmk_encoding[datap] = encoding;
	    /* Cleanup: precede mark with 3 MFM C2's with missing clocks,
	       and some MFM 00's before that */
	    for (i = datap-1; i >= DMK_TKHDR_SIZE && i >= datap-3; i--) {
	      dmk_track[i] = 0xC2;
	      dmk_encoding[i] = MFM_IAM;
	    }
	    for (; i >= DMK_TKHDR_SIZE && i >= datap-3 - MFM_GAP3Z; i--) {
	      dmk_track[i] = 0x00;
	      dmk_encoding[i] = MFM;
	    }
	  }

	} else if (rx02_data > 0) {
	  if (fmtimes == 2 && skip) {
	    /* Skip the duplicated DAM */
	    dmk_encoding[datap] = SKIP;
	    skip = 0;
	  } else {
	    /* Encode an rx02-modified MFM byte */
	    dmk_encoding[datap] = RX02;
	    rx02_data--;
	  }

	} else if (encoding == FM && fmtimes == 2 && skip) {
	  /* Skip bytes that are an odd distance from an address mark */
	  dmk_encoding[datap] = SKIP;
	  skip = !skip;

	} else {
	  /* Normal case */
	  dmk_encoding[datap] = encoding;
	  skip = !skip;
	}
      }

      /* Encode into clock/data stream */
      catweasel_reset_pointer(&c);

      if (testmode >= 0x100 && testmode <= 0x1ff) {
	/* Fill with constant value instead of actual data; for testing */
	for (i=0; i<128*1024; i++) {
	  catweasel_put_byte(&c, testmode);
	}
	  
      } else {
	for (i=0; i<7; i++) {
	  /* XXX Is this needed/correct? */
	  catweasel_put_byte(&c, 0);
	}

	cw_bit(CW_BIT_INIT, mult);
	bit = 0;
	encoding = dmk_encoding[DMK_TKHDR_SIZE];
	if (iam_pos >= 0) {
	  if (got_iam == 0) {
	    fprintf(stderr,
		    "dmk2cw: No index address mark on track %d, side %d\n",
		    track, side);
	  } else {
	    ignore = got_iam - DMK_TKHDR_SIZE - iam_pos;
	  }
	}
	for (datap = DMK_TKHDR_SIZE + ignore;
	     datap < dmk_header.tracklen; datap++) {
	  if (datap >= DMK_TKHDR_SIZE) {
	    byte = dmk_track[datap];
	    encoding = dmk_encoding[datap];
	  } else {
	    byte = (encoding == MFM) ? 0x4e : 0xff;
	  }
	  if (out_fmt >= OUT_BYTES) {
	    printf("%c%02x", "-FIAMJBX"[encoding], byte);
	  }
	  switch (encoding) {
	  case SKIP:    /* padding byte in FM area of a DMK */
	    break;
	  case FM:      /* FM with FF clock */
	    for (i=0; i<8; i++) {
	      cw_bit(1, mult);
	      cw_bit(0, mult);
	      bit = (byte & 0x80) ? 1 : 0;
	      byte <<= 1;
	      cw_bit(bit, mult);
	      cw_bit(0, mult);
	    }
	    break;
	  case FM_IAM:  /* FM with D7 clock (IAM) */
	    clock_byte = 0xd7;
	    for (i=0; i<8; i++) {
	      bit = (clock_byte & 0x80) != 0;
	      clock_byte <<= 1;
	      cw_bit(bit, mult);
	      cw_bit(0, mult);
	      bit = (byte & 0x80) != 0;
	      byte <<= 1;
	      cw_bit(bit, mult);
	      cw_bit(0, mult);
	    }
	    break;
	  case FM_AM:   /* FM with C7 clock (IDAM or DAM) */
	    clock_byte = 0xc7;
	    for (i=0; i<8; i++) {
	      bit = (clock_byte & 0x80) != 0;
	      clock_byte <<= 1;
	      cw_bit(bit, mult);
	      cw_bit(0, mult);
	      bit = (byte & 0x80) != 0;
	      byte <<= 1;
	      cw_bit(bit, mult);
	      cw_bit(0, mult);
	    }
	    break;
	  case MFM:     /* MFM with normal clocking algorithm */
	    for (i=0; i<8; i++) {
	      prev_bit = bit;
	      bit = (byte & 0x80) != 0;
	      cw_bit(prev_bit == 0 && bit == 0, mult);
	      cw_bit(bit, mult);
	      byte <<= 1;
	    }
	    break;
	  case MFM_IAM: /* MFM C2 with missing clock */
	    for (i=0; i<8; i++) {
	      prev_bit = bit;
	      bit = (byte & 0x80) != 0;
	      cw_bit(prev_bit == 0 && bit == 0 && i != 4, mult);
	      cw_bit(bit, mult);
	      byte <<= 1;
	    }
	    break;
	  case MFM_AM:  /* MFM A1 with missing clock */
	    for (i=0; i<8; i++) {
	      prev_bit = bit;
	      bit = (byte & 0x80) != 0;
	      cw_bit(prev_bit == 0 && bit == 0 && i != 5, mult);
	      cw_bit(bit, mult);
	      byte <<= 1;
	    }
	    break;
	  case RX02:    /* DEC-modified MFM as in RX02 */
	    if (dmk_encoding[datap-1] != RX02) {
	      rx02_bitpair(RX02_BITPAIR_INIT, mult);
	    }
	    for (i=0; i<8; i++) {
	      bit = (byte & 0x80) != 0;
	      rx02_bitpair(bit, mult);
	      byte <<= 1;
	    }
	    if (dmk_encoding[datap+1] != RX02) {
	      rx02_bitpair(RX02_BITPAIR_FLUSH, mult);
	    }
	    break;
	  }
	}

	rx02_bitpair(RX02_BITPAIR_FLUSH, mult);
	cw_bit(1, mult);
	cw_bit(CW_BIT_FLUSH, mult);
      }

      if (out_fmt >= OUT_BYTES) {
	printf("\n");
	fflush(stdout);
      }

      catweasel_set_hd(&c, hd);

      if (!catweasel_write(&c.drives[drive], side, cwclock, -1)) {
	fprintf(stderr, "dmk2cw: Write error\n");
	cleanup();
	return 1;
      }
    }
  }

  cleanup();
  return 0;
}
