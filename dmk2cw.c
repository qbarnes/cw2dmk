/*
 * dmk2cw: Write a .dmk to a real floppy disk using the Catweasel.
 * Copyright (C) 2001 Timothy Mann
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

#define DEBUG6 1
/*#define DEBUG7 1*/
/*#define DEBUG8 1*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#if __DJGPP__
/* DJGPP doesn't support SA_RESETHAND, so reset manually for it in handler(). */
#define SA_RESETHAND 0
#endif
#include <sys/time.h>
#if linux
#include <sys/io.h>
#include <errno.h>
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
double precomplo = 140.0, precomphi = 140.0, precomp; /*ns*/
int cwclock = 2;
int hd = 4;
int ignore = 0;
int iam_pos = -1;
int testmode = -1;
int fill = 0;
int reverse = 0;
int datalen = -1;
double rate_adj = 1.0;
int dither = 0;
unsigned step_ms = 6;
unsigned settle_ms = 0;

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
  printf(" -T stp[,stl]  Step time [%u] and head settling time [%u] ms\n",
         step_ms, settle_ms);
  printf(" -s maxsides   Maximum number of sides, 1 or 2 [%d]\n", maxsides);
  printf("\nThese values normally need not be changed:\n");
  printf(" -p port       I/O port base (MK1) or card number (MK3/4) [%d]\n",
	 port);
  printf(" -c clock      Catweasel clock multiplier [%d]\n", cwclock);
  printf(" -o plo[,phi]  Write-precompensation range (ns) [%g,%g]\n",
	 precomplo, precomphi);
  printf(" -h hd         HD; 0=lo, 1=hi, 2=lo/hi, 3=hi/lo, 4=by kind [%d]\n",
	 hd);
  printf(" -l len        Use only first len bytes of track [%d]\n", datalen);
  printf(" -g ign        Ignore first ign bytes of track [%d]\n", ignore);
  printf(" -i ipos       Force IAM to ipos from track start; "
	 "if -1, don't [%d]\n", iam_pos);
  printf(" -r reverse    0 = normal, 1 = reverse sides [%d]\n", reverse);
  printf(" -f fill       Fill type [0x%x]\n", fill);
  printf("               0x0 = 0xff if in FM, 0x4e if in MFM\n");
  printf("               0x1 = erase only\n");
  printf("               0x2 = write very long transitions\n");
  printf("               0x3 = stop with no fill; leave old data intact\n");
  printf("               0x1nn = nn in FM\n");
  printf("               0x2nn = nn in MFM\n");
  printf(" -a rate_adj   Data rate adjustment factor [%.1f]\n", rate_adj);
  printf(" -e dither     Dither data rate 0=false, 1=true [%d]\n", dither);
#if DEBUG6
  printf(" -y testmode   Activate various test modes [%d]\n", testmode);
#endif
  printf("\n");
  printf("dmk2cw version %s\n", VERSION);
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
#if __DJGPP__
  struct sigaction sa_dfl = { .sa_handler = SIG_DFL };
  sigaction(sig, &sa_dfl, NULL);
#endif
  raise(sig);
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
    exit(1);
  }
}


double
cw_measure_rpm(catweasel_drive *d)
{
  struct timeval t1, t2;
  long long usec;

  catweasel_await_index(d);
  gettimeofday(&t1, NULL);
  catweasel_await_index(d);
  gettimeofday(&t2, NULL);
  usec = (t2.tv_sec - t1.tv_sec)*1000000 + t2.tv_usec - t1.tv_usec;
  return 60000000.0/(double)usec;
}

#define CW_BIT_INIT -1
#define CW_BIT_FLUSH -2

int
cw_bit(int bit, double mult)
{
  static int len, nextlen;
  static double prevadj, adj, preverr;
  double fticks;
  int iticks;
  int res;

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
    preverr = 0.0;
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
      fticks = len * mult - prevadj + adj - preverr;
      iticks = (int)(fticks + 0.5);
      if (iticks > 129) {
	fprintf(stderr, "dmk2cw: Interval %d too large; "
		"try a smaller value for -c if possible\n", iticks);
	exit(1);
      }
      if (iticks < 3) {
	fprintf(stderr, "dmk2cw: Interval %d too small; bug?\n", iticks);
	iticks = 3;
      }
      prevadj = adj;
      if (dither) {
	preverr = (double)iticks - fticks;
      }
      if (out_fmt >= OUT_SAMPLES) {
	printf("/%d:%d", len, iticks);
      }
      res = catweasel_put_byte(&c, 129 - iticks);
      if (res < 0) return res;
    }
    len = nextlen;
    nextlen = 0;
    break;
  case CW_BIT_FLUSH:
    cw_bit(1, mult);
    cw_bit(1, mult);
    cw_bit(CW_BIT_INIT, mult);
    break;
  }
  return 1;
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

/*
 * MFM with missing clock, or normal MFM if missing_clock = -1.
 * On entry, *prev_bit is the previous bit encoded;
 * on exit, the last bit encoded.
 */
int
mfm_byte(int byte, int missing_clock, double mult, int *prev_bit)
{
  int i, bit, res = 0;

  for (i=0; i<8; i++) {
    bit = (byte & 0x80) != 0;
    res = cw_bit(*prev_bit == 0 && bit == 0 && i != missing_clock, mult);
    if (res < 0) break;
    res = cw_bit(bit, mult);
    if (res < 0) break;
    byte <<= 1;
    *prev_bit = bit;
  }
  return res;
}

/*
 * FM with specified clock pattern.
 * On exit, *prev_bit is 0, in case the next byte is MFM.
 */
int
fm_byte(int byte, int clock_byte, double mult, int *prev_bit)
{
  int i, bit, res = 0;

  for (i=0; i<8; i++) {
    bit = (clock_byte & 0x80) != 0;
    clock_byte <<= 1;
    res = cw_bit(bit, mult);
    if (res < 0) break;
    res = cw_bit(0, mult);
    if (res < 0) break;
    bit = (byte & 0x80) != 0;
    byte <<= 1;
    res = cw_bit(bit, mult);
    if (res < 0) break;
    res = cw_bit(0, mult);
    if (res < 0) break;
  }
  *prev_bit = 0;
  return res;
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

/* Byte encodings and letters used when logging them */
#define SKIP    0  /* -  padding byte in FM area of a DMK */
#define FM      1  /* F  FM with FF clock */
#define FM_IAM  2  /* I  FM with D7 clock (IAM) */
#define FM_AM   3  /* A  FM with C7 clock (IDAM or DAM) */
#define MFM     4  /* M  MFM with normal clocking algorithm */
#define MFM_IAM 5  /* J  MFM C2 with missing clock */
#define MFM_AM  6  /* B  MFM A1 with missing clock */
#define RX02    7  /* X  DEC-modified MFM as in RX02 */
#define encoding_letter "-FIAMJBX"

/* Or'ed into encoding at end of sector data */
#define SECTOR_END 0x80
#define enc(encoding) ((encoding) & ~SECTOR_END)
#define isend(encoding) (((encoding) & SECTOR_END) != 0)
#define ismfm(encoding) (enc(encoding) >= MFM && enc(encoding) <= MFM_AM)
#define ismark(encoding) (enc(encoding) == FM_IAM || \
                          enc(encoding) == FM_AM || \
                          enc(encoding) == MFM_IAM || \
                          enc(encoding) == MFM_AM)

#include "secsize.c"

/*
 * Like strtol, but exit with a fatal error message if there are any
 * invalid characters or the string is empty.
 */
long int
strtol_strict(const char *nptr, int base, const char *name)
{
  long int res;
  char *endptr;

  res = strtol(nptr, &endptr, base);
  if (*nptr == '\0' || *endptr != '\0') {
    fprintf(stderr, "dmk2cw: %s requires a numeric argument\n", name);
    exit(1);
  }
  return res;
}

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
  int encoding, next_encoding, prev_encoding;
  int dam_min, dam_max, got_iam, skip;
  int byte, bit;
  double mult;
  int rx02_data;
  int sector_data;
  int cw_mk = 1;
  int tracklen;
  int extra_bytes;
  char optname[3] = "-?";

  opterr = 0;
  for (;;) {
    ch = getopt(argc, argv, "p:d:v:k:m:s:o:c:h:l:g:i:r:f:a:e:y:T:");
    if (ch == -1) break;
    optname[1] = ch;
    switch (ch) {
    case 'p':
      port = strtol_strict(optarg, 16, optname);
      if (port < 0 || (port >= MK3_MAX_CARDS && port < MK1_MIN_PORT) ||
	  (port > MK1_MAX_PORT)) {
	fprintf(stderr,
		"dmk2cw: -p must be between %d and %d for MK3/4 cards,\n"
		"  or between 0x%x and 0x%x for MK1 cards.\n",
		0, MK3_MAX_CARDS-1, MK1_MIN_PORT, MK1_MAX_PORT);
	exit(1);
      }
      break;
    case 'd':
      drive = strtol_strict(optarg, 0, optname);
      if (drive < 0 || drive > 1) usage();
      break;
    case 'v':
      out_fmt = strtol_strict(optarg, 0, optname);
      if (out_fmt < OUT_MIN || out_fmt > OUT_MAX) usage();
      break;
    case 'k':
      kind = strtol_strict(optarg, 0, optname);
      if (kind < 0 || kind > NKINDS) usage();
      break;
    case 'm':
      steps = strtol_strict(optarg, 0, optname);
      if (steps < 1 || steps > 2) usage();
      break;
    case 's':
      maxsides = strtol_strict(optarg, 0, optname);
      if (maxsides < 1 || maxsides > 2) usage();
      break;
    case 'o':
      ret = sscanf(optarg, "%lf, %lf", &precomplo, &precomphi);
      if (ret == 0) usage();
      if (ret == 1) precomphi = precomplo;
      if (precomplo < 0.0 || precomphi < 0.0) usage();
      break;
    case 'c':
      cwclock = strtol_strict(optarg, 0, optname);
      if (cwclock != 1 && cwclock != 2 && cwclock != 4) usage();
      break;
    case 'h':
      hd = strtol_strict(optarg, 0, optname);
      if (hd < 0 || hd > 4) usage();
      break;
    case 'l':
      datalen = strtol_strict(optarg, 0, optname);
      break;
    case 'g':
      ignore = strtol_strict(optarg, 0, optname);
      break;
    case 'i':
      iam_pos = strtol_strict(optarg, 0, optname);
      break;
    case 'r':
      reverse = strtol_strict(optarg, 0, optname);
      if (reverse < 0 || reverse > 1) usage();
      break;
    case 'f':
      fill = strtol_strict(optarg, 0, optname);
      if (fill < 0 || (fill > 3 && fill < 0x100) || fill > 0x2ff) {
	usage();
      }
      break;
    case 'a':
      ret = sscanf(optarg, "%lf", &rate_adj);
      if (ret != 1) usage();
      if (rate_adj < 0.0) usage();
      break;
    case 'e':
      dither = strtol_strict(optarg, 0, optname);
      if (dither < 0 || dither > 1) usage();
      break;
#if DEBUG6
    case 'y':
      testmode = strtol_strict(optarg, 0, optname);
      break;
#endif
    case 'T':
      i = sscanf(optarg, "%u,%u", &step_ms, &settle_ms);
      if (i < 1) usage();
      break;
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
  struct sigaction sa = { .sa_handler = handler, .sa_flags = SA_RESETHAND };
  int sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGTERM };

  for (int s = 0; s < sizeof(sigs)/sizeof(*sigs); ++s) {
    if (sigaction(sigs[s], &sa, 0) == -1) {
      fprintf(stderr, "sigaction failed for signal %d.\n", sigs[s]);
      exit(1);
    }
  }


  if (out_fmt > OUT_QUIET) {
    printf("dmk2cw %s\n", VERSION);
  }
  if (out_fmt > OUT_NORMAL) {
    printf("Command line: ");
    for (i = 0; i < argc; i++) {
      printf("%s ", argv[i]);
    }
    printf("\n");
  }

  /* Start Catweasel */
#if linux
  if (geteuid() != 0) {
    fprintf(stderr, "cw2dmk: Must be setuid to root or be run as root\n");
    exit(1);
  }
#endif
  if (port < 10) {
    port = pci_find_catweasel(port, &cw_mk);
    if (port == -1) {
      port = MK1_DEFAULT_PORT;
      printf("Failed to detect Catweasel MK3/4 on PCI bus; "
	     "looking for MK1 on ISA bus at 0x%x\n", port);
      fflush(stdout);
    }
  }
#if linux
  if ((cw_mk == 1 && ioperm(port, 8, 1) == -1) ||
      (cw_mk >= 3 && iopl(3) == -1)) {
    fprintf(stderr, "dmk2cw: No access to I/O ports\n");
    exit(1);
  }
  if (setuid(getuid()) != 0) {
    fprintf(stderr, "dmk2cw: setuid failed: %s\n", strerror(errno));
    exit(1);
  }
#endif
  ret = catweasel_init_controller(&c, port, cw_mk, getenv("CW4FIRMWARE"),
                                  step_ms, settle_ms)
    && catweasel_memtest(&c);
  if (ret) {
    if (out_fmt >= OUT_QUIET) {
      printf("Detected Catweasel MK%d at port 0x%x\n", cw_mk, port);
      fflush(stdout);
    }
  } else {
    fprintf(stderr, "dmk2cw: Failed to detect Catweasel at port 0x%x\n",
	    port);
    exit(1);
  }
  if (cw_mk == 1 && cwclock == 4) {
    fprintf(stderr, "dmk2cw: Catweasel MK1 does not support 4x clock\n");
    exit(1);
  }
  if (cw_mk < 4 && fill == 1) {
    fprintf(stderr, "dmk2cw: Catweasel MK%d does not support fill type %d\n",
	    cw_mk, fill);
  }
  catweasel_detect_drive(&c.drives[drive]);

  if (atexit(cleanup)) {
    fprintf(stderr, "cw2dmk: Can't establish atexit() call.\n");
    exit(1);
  }

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
    exit(1);
  }

  /* Open input file */
  dmk_file = fopen(argv[optind], "rb");
  if (dmk_file == NULL) {
    perror(argv[optind]);
    exit(1);
  }

  /* Set DMK parameters */
  dmk_read_header(dmk_file, &dmk_header);
  if ((dmk_header.writeprot != 0x00 && dmk_header.writeprot != 0xff) ||
      dmk_header.mbz != 0) {
    fprintf(stderr, "dmk2cw: File is not in DMK format\n");
    exit(1);
  }
  sides = (dmk_header.options & DMK_SSIDE_OPT) ? 1 : 2;
  fmtimes = (dmk_header.options & DMK_SDEN_OPT) ? 1 : 2;
  rx02 = (dmk_header.options & DMK_RX02_OPT) ? 1 : 0;
  if (datalen < 0 || datalen > dmk_header.tracklen - DMK_TKHDR_SIZE) {
    tracklen = dmk_header.tracklen;
  } else {
    tracklen = DMK_TKHDR_SIZE + datalen;
  }
  dmk_track = (unsigned char*) malloc(tracklen);
  dmk_encoding = (unsigned char*) malloc(tracklen);
  /*
   * Extra bytes after the data CRC, if any.  secsize() accounts for
   * extra bytes before the data CRC.
   */
  if (dmk_header.quirks & QUIRK_EXTRA_CRC) {
    extra_bytes = 6;
  } else if (dmk_header.quirks & QUIRK_EXTRA) {
    extra_bytes = 6; // unspecified, use 6 in case really QUIRK_EXTRA_CRC
  } else {
    extra_bytes = 0;
  }

  /* Select drive, start motor, wait for spinup */
  catweasel_select(&c, !drive, drive);
  catweasel_set_motor(&c.drives[drive], 1);
  catweasel_usleep(500000);

  if (catweasel_write_protected(&c.drives[drive])) {
    fprintf(stderr, "dmk2cw: Disk is write-protected\n");
    exit(1);
  }

  /* Detect kind if needed */
  if (kind == 0) {
    double rpm = cw_measure_rpm(&c.drives[drive]);
#if DEBUG8
    printf("drive rpm approx %f\n", rpm);
    printf("dmk track length %d (using %d)\n", dmk_header.tracklen, tracklen);
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
  mult = (kd->mfmshort / 2.0) * cwclock / rate_adj;
  if (hd == 4) {
    hd = kd->hd;
  }

  /* Loop through tracks */
  for (track=0; track<dmk_header.ntracks; track++) {
    catweasel_seek(&c.drives[drive], track * steps);

    precomp = ((dmk_header.ntracks - 1 - track) * precomplo +
	       track * precomphi) / (dmk_header.ntracks - 1);

    /* Loop through sides */
    for (side=0; side<sides; side++) {
      /* Read DMK track data */
      fseek(dmk_file, sizeof(dmk_header_t) +
	    (track * sides + side) * dmk_header.tracklen, 0);
      ret = fread(dmk_track, tracklen, 1, dmk_file);
      if (ret != 1) {
	fprintf(stderr, "dmk2cw: Error reading from DMK file\n");
	perror("dmk2cw");
	exit(1);
      }
      if (testmode >= 0 && testmode <= 0xff) {
	/* Fill with constant value instead of actual data; for testing */
	memset(dmk_track + DMK_TKHDR_SIZE, testmode, tracklen - DMK_TKHDR_SIZE);
      }

      /* Determine encoding for each byte and clean up */
      idampp = 0;
      idamp = 0;
      dam_min = 0;
      dam_max = 0;
      got_iam = 0;
      skip = 0;
      rx02_data = 0;
      sector_data = 0;

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
      for (datap = DMK_TKHDR_SIZE; datap < tracklen; datap++) {
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
		   ((dmk_track[datap] >= 0xf8 && dmk_track[datap] <= 0xfb) ||
		    dmk_track[datap] == 0xfd)) {
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
	  } else {
            /* Compute expected sector size, including CRC and any extra
             * bytes after the CRC.  This matters only for warning if
             * the entire sector didn't fit, so be liberal and use
             * maxsize = 7 instead of burdening the user with yet
             * another command line option.*/
            sector_data = secsize(dmk_track[idamp+4], encoding, 7,
                                  dmk_header.quirks) + 2 + extra_bytes;
          }

	} else if (datap >= DMK_TKHDR_SIZE && datap <= first_idamp
		   && !got_iam && dmk_track[datap] == 0xfc &&
		   ((encoding == MFM && dmk_track[datap-1] == 0xc2) ||
		    (encoding == FM && (dmk_track[datap-fmtimes] == 0x00 ||
					dmk_track[datap-fmtimes] == 0xff)))) {
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
            if (rx02_data == 0) {
              dmk_encoding[datap] |= SECTOR_END;
            }
	  }

	} else if (encoding == FM && fmtimes == 2 && skip) {
	  /* Skip bytes that are an odd distance from an address mark */
	  dmk_encoding[datap] = SKIP;
	  skip = !skip;

	} else {
	  /* Normal case */
	  dmk_encoding[datap] = encoding;
	  skip = !skip;
          if (sector_data > 0) {
            sector_data--;
            if (sector_data == 0) {
              dmk_encoding[datap] |= SECTOR_END;
            }
          }
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
        prev_encoding = SKIP;
	if (iam_pos >= 0) {
	  if (got_iam == 0) {
	    fprintf(stderr,
		    "dmk2cw: No index address mark on track %d, side %d\n",
		    track, side);
	  } else {
	    ignore = got_iam - DMK_TKHDR_SIZE - iam_pos;
	  }
	}
	for (datap = DMK_TKHDR_SIZE + ignore; datap < tracklen; datap++) {
	  if (datap >= DMK_TKHDR_SIZE) {
	    byte = dmk_track[datap];
	    encoding = dmk_encoding[datap];
	  } else {
	    byte = ismfm(encoding) ? 0x4e : 0xff;
	  }
          if (encoding != SKIP) {
            if (out_fmt >= OUT_SAMPLES) printf("\n");
            if (out_fmt >= OUT_BYTES) {
              if (enc(encoding) != enc(prev_encoding)) {
                if (ismark(encoding) && prev_encoding != SKIP) printf("\n");
                printf("<%c>", encoding_letter[enc(encoding)]);
                prev_encoding = encoding;
              }
              printf("%02x%s ", byte, isend(encoding) ? "|" : "");
            }
	  }
	  switch (enc(encoding)) {
	  case SKIP:    /* padding byte in FM area of a DMK */
	    break;

	  case FM:      /* FM with FF clock */
	    fm_byte(byte, 0xff, mult, &bit);
	    break;

	  case FM_IAM:  /* FM with D7 clock (IAM) */
	    fm_byte(byte, 0xd7, mult, &bit);
	    break;

	  case FM_AM:   /* FM with C7 clock (IDAM or DAM) */
	    fm_byte(byte, 0xc7, mult, &bit);
	    break;

	  case MFM:     /* MFM with normal clocking algorithm */
	    mfm_byte(byte, -1, mult, &bit);
	    break;

	  case MFM_IAM: /* MFM with missing clock 4 */
	    mfm_byte(byte, 4, mult, &bit);
	    break;

	  case MFM_AM:  /* MFM with missing clock 5 */
	    mfm_byte(byte, 5, mult, &bit);
	    break;

	  case RX02:    /* DEC-modified MFM as in RX02 */
	    if (enc(dmk_encoding[datap-1]) != RX02) {
	      rx02_bitpair(RX02_BITPAIR_INIT, mult);
	    }
	    for (i=0; i<8; i++) {
	      bit = (byte & 0x80) != 0;
	      rx02_bitpair(bit, mult);
	      byte <<= 1;
	    }
	    if (enc(dmk_encoding[datap+1]) != RX02) {
	      rx02_bitpair(RX02_BITPAIR_FLUSH, mult);
	    }
	    break;
	  }

          if (isend(encoding)) {
            if (catweasel_sector_end(&c) < 0) {
              fprintf(stderr, "dmk2cw: Catweasel memory full\n");
              exit(1);
            }
          }
        }

	rx02_bitpair(RX02_BITPAIR_FLUSH, mult);

	/* In case the DMK buffer is shorter than the physical track,
	   fill the rest of the Catweasel's memory with a fill
	   pattern. */
	switch (fill) {
	case 0:
	  /* Fill with a standard gap byte in most recent encoding */
          if (ismfm(encoding)) {
	    for (;;) {
	      if (mfm_byte(0x4e, -1, mult, &bit) < 0) break;
	    }
          } else {
	    for (;;) {
	      if (fm_byte(0xff, 0xff, mult, &bit) < 0) break;
	    }
          }
          break;

	case 1:
	  /* Erase remainder of track and write nothing. */
	  /* Note: when reading back a track like this, my drives
	     appear to see garbage there, not a lack of transitions.
	     Maybe the drive just isn't happy not seeing a transition
	     for a long time and it ends up manufacturing them from
	     noise? */
	  cw_bit(CW_BIT_FLUSH, mult);
	  for (;;) {
	    if (catweasel_put_byte(&c, 0x81) < 0) break;
	  }
	  break;

	case 2:
	  /* Fill with a pattern of very long transitions. */
	  cw_bit(CW_BIT_FLUSH, mult);
	  for (;;) {
	    if (catweasel_put_byte(&c, 0) < 0) break;
	  }
	  break;

	case 3:
	  /* Stop writing, leaving whatever was there before. */
	  cw_bit(CW_BIT_FLUSH, mult);
	  catweasel_put_byte(&c, 0xff);
	  break;

	default:
	  switch (fill >> 8) {
	  case 1:
	  default:
	    /* Fill with a specified byte in FM */
	    for (;;) {
	      if (fm_byte(fill & 0xff, 0xff, mult, &bit) < 0) break;
	    }
	    break;
	  case 2:
	    /* Fill with a specified byte in MFM */
	    for (;;) {
	      if (mfm_byte(fill & 0xff, -1, mult, &bit) < 0) break;
	    }
	    break;
	  }
	}
      }

      if (out_fmt >= OUT_BYTES) {
	printf("\n");
	fflush(stdout);
      }

      catweasel_set_hd(&c, (hd & 1) ^ ((hd > 1) && (track > 43)));

      ret = catweasel_write(&c.drives[drive], side ^ reverse, cwclock, -1);
      if (ret == 0) {
	fprintf(stderr, "dmk2cw: Write error\n");
	exit(1);
      } else if (ret == -1) {
        printf("dmk2cw: Some data did not fit on track %d, side %d\n",
               track, side);
      }
    }
  }

  cleanup();
  return 0;
}
