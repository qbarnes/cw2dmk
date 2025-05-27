/* Catweasel histogram analyzer.

   Copyright (c) 2000-2025 Timothy P. Mann.

   Based on a test program from the cwfloppy-0.2.1 package, which is
   copyright (c) 1998 Michael Krause.

   Obtains Catweasel samples either directly from reading a single
   track, or from replaying a cw2dmk verbosity 7 log file.  Prints a
   histogram of the samples (flux transition interarrival times) from
   each track.  Analyzes the data to guess the drive RPM and the bit
   clock rate, searches for the expected 2 (FM) or 3 (MFM) peaks in
   the histogram, categorizes the samples as short, medium, or long
   accordingly, and prints the mean and standard deviation of the
   actual length of samples in each category.

   Usage for direct reads from Catweasel:

       cwhisto port drive track side clock [outfile]

       One track is read 4 times (value of NNN below) and the samples
       from all passes are accumulated together for analysis.

       * port: Catweasel port; see -p in cw2dmk man page
       * drive: Drive unit number; see -d in cw2dmk man page
       * track: physical track number, 0 origin
       * side: physical side number, 0 or 1
       * clock: Catweasel clock; see -c in cw2dmk man page
       * outfile: Optional file to dump raw samples to.
         When samples are being dumped to a file, each pass over the
         track is preceded by a pseudo-sample equal to 0x80 + read
         number (0-origin).  The actual samples can be at most 0x7f.

   Usage for replaying a log:

       cwhisto infile clock split

       All tracks in the log are read and analyzed.

       * infile: the cw2dmk -v7 log to be replayed
       * clock: Catweasel clock; see -c in cw2dmk man page (usually 2).
       * split: If the log contains multiple consecutive passes over
         the same track and split=0, the passes are accumulated
         together for analysis; if split=1, each pass is analyzed
         separately.

       Note: If the original read was done with -h0, the analyzed
       drive speed will be nonsensical, but the rest of the analysis
       should be okay.  See comment in the code for why.

   Some possible future improvements:
      * Clean up and refactor a bit more.
      * Improve command line parsing, provide defaults.
      * Direct mode: add an option to loop through the whole disk.
      * Direct mode: add an option to specify the number of passes.
      * Replay mode: add an option to analyze only a specific track.
      * Replay mode: add ability to replay outfiles from direct mode (?).
      * Replay mode: add ability to deduce the clock from the logged
        cw2dmk output.  This currently isn't logged directly, so, ugh.
*/

#define NNN 4

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#if linux
#include <sys/io.h>
#include <errno.h>
#endif
#include "cwfloppy.h"
#include "cwpci.h"
#include "parselog.h"

FILE *f;
struct catweasel_contr c;
char *progname;

int cwclock = 1;

/*
 * Initialize the Catweasel and spin up the drive.  Exits on errors.
 */
static void cw_initialize(int port, int drive)
{
  int cw_mk = 1;
  int ret;

  /* Start Catweasel */
  if (port < 10) {
    port = pci_find_catweasel(port, &cw_mk);
    if (port == -1) {
      port = MK1_DEFAULT_PORT;
      printf("Failed to detect Catweasel MK3/4 on PCI bus; "
             "looking for MK1 on ISA bus at 0x%x\n", port);
    }
  }
#if linux
  if ((cw_mk == 1 && ioperm(port, 8, 1) == -1) ||
      (cw_mk >= 3 && iopl(3) == -1)) {
    fprintf(stderr, "testhist: No access to I/O ports\n");
    exit(1);
  }
  if (setuid(getuid()) != 0) {
    fprintf(stderr, "testhist: setuid failed: %s\n", strerror(errno));
    exit(1);
  }
#endif
  ret = catweasel_init_controller(&c, port, cw_mk, getenv("CW4FIRMWARE"),
                                  6, 0)
    && catweasel_memtest(&c);
  if (!ret) {
    fprintf(stderr, "testhist: Failed to detect Catweasel at port 0x%x\n", port);
    exit(1);
  }
  catweasel_detect_drive(&c.drives[drive]);
  if (c.drives[drive].type == 0) {
    fprintf(stderr, "testhist: Did not detect drive %d, but trying anyway\n",
            drive);
  }

  catweasel_select(&c, !drive, drive);
  catweasel_set_motor(&c.drives[drive], 1);
  catweasel_usleep(500000);
}

/*
 * Deinitialize the Catweasel and stop the drive.
 */
static void cw_finalize(int drive)
{
  catweasel_set_motor(&c.drives[drive], 0);
  catweasel_select(&c, 0, 0);
  catweasel_free_controller(&c);
}

/*
 * Use the already-initialized Catweasel to read a given track "passes"
 * times into the 128-int histogram buffer at buf.
 */
static void cw_histo_track(int drive, int track, int side, int passes,
                           unsigned int *buf)
{
  int b;
  int i;

  catweasel_seek(&c.drives[drive], track);

  memset(buf, 0, 128*sizeof(int));
  for (i=0; i<passes; i++) {
    /*
     * Use index-to-index read without marking index edges.  Although
     * this does count the width of the index pulse twice, it's fast and
     * quite accurate enough for a histogram.
     */
    if (!catweasel_read(&c.drives[drive], side, cwclock, 0, 0)) {
      fprintf(stderr, "Read error\n");
      catweasel_free_controller(&c);
      exit(2);
    }
    if (f) putc(0x80 + i, f); // mark start of ith read
    while((b = catweasel_get_byte(&c)) != -1 && b < 0x80) {
      b &= 0x7f;
      buf[b]++;
      if (f) putc(b, f);
    }
  }
}

/*
 * Print the raw histogram buckets, analyze it, and print the results.
 */
static void eval_histo(unsigned int *histogram, int passes)
{
    int i, ii, j, pwidth, psamps, psampsw;
    double peak[3], sd[3], ps[3];

    /* Print histogram */
    for(i=0;i<128;i+=8) {
	printf("%3d: %06d %06d %06d %06d %06d %06d %06d %06d\n", i,
	       histogram[i+0], histogram[i+1], histogram[i+2], histogram[i+3],
	       histogram[i+4], histogram[i+5], histogram[i+6], histogram[i+7]);
    }

    /* Find two or three peaks */
    i = 0;
    for (j=0; j<3; j++) {
      pwidth = 0;
      psamps = 0;
      psampsw = 0;
      while (histogram[i] < 64 && i < 128) i++;
      while (histogram[i] >= 64 && i < 128) {
	pwidth++;
	psamps += histogram[i];
	psampsw += histogram[i] * i;
	i++;
      }
      if (pwidth == 0 || pwidth > 24) {
	/* Not a real peak */
	break;
      } else {
	peak[j] = ((double) psampsw) / psamps;
	ps[j] = psamps;
	sd[j] = 0.0;
	for (ii = i - pwidth; ii < i; ii++) {
	  sd[j] += histogram[ii] * pow((double)ii - peak[j], 2);
	}
	sd[j] = sqrt(sd[j]/((double)psamps-1));
	printf("peak %d: mean %.05f, sd %.05f\n", j, peak[j], sd[j]);
      }
    }

    /* Guess drive RPM based on total number of cw clock cycles */
    /* Note that the sample values and histogram bucket numbers
       are (I believe) 1 less than the actual number of cycles, so
       we need to add 1 in some places here */
    psamps = 0;
    psampsw = 0;
    for (i=0; i<128; i++) {
      psamps += histogram[i];
      psampsw += histogram[i] * (i + 1);
    }
    printf("drive speed approx    %f RPM\n",
	   CWHZ * cwclock / psampsw * passes * 60.0);

    /* Guess bit clock by imputing the expected number of
       clocks to each peak */
#if 0
    /* This code weights each peak the same */
    if (j == 2) {
      /* FM encoding */
      printf("FM data clock approx  %f kHz\n",
	     CWHZ / 1000.0 * cwclock /
	     ((peak[0]/0.5 + peak[1]/1.0)/2.0 + 1.0));
    } else if (j == 3) {
      /* MFM encoding */
      printf("MFM data clock approx %f kHz\n",
	     CWHZ / 1000.0 * cwclock /
	     ((peak[0]/1.0 + peak[1]/1.5 + peak[2]/2.0)/3.0 + 1.0));
    }
#else
    /* This code weights each peak by the number of samples in it;
       I think that should be a bit more accurate. */
    if (j == 2) {
      /* FM encoding */
      printf("FM data clock approx  %f kHz\n",
	     CWHZ / 1000.0 * cwclock /
	     ((peak[0]/0.5*ps[0] + peak[1]/1.0*ps[1])
	      / (ps[0] + ps[1]) + 1.0));
    } else if (j == 3) {
      /* MFM encoding */
      printf("MFM data clock approx %f kHz\n",
	     CWHZ / 1000.0 * cwclock /
	     ((peak[0]/1.0*ps[0] + peak[1]/1.5*ps[1] + peak[2]/2.0*ps[2])
	      / (ps[0] + ps[1] + ps[2]) + 1.0));
    }
#endif
}

/*
 * Exit with a usage message.
 */
void usage(void)
{
  fprintf(stderr,
          "Usage: %s port drive track side clock [outfile]\n"
          "       %s infile clock split\n", progname, progname);
  exit(2);
}

int main(int argc, char **argv)
{
  int track, side, port, drive;
  unsigned int buf[128];

  progname = argv[0];

  if (argc == 6 || argc == 7) {
    port = strtol(argv[1], NULL, 16);
    drive = atoi(argv[2]);
    track = atoi(argv[3]);
    side = atoi(argv[4]);
    cwclock = atoi(argv[5]);
    if (argc == 7) {
      if (strcmp(argv[6], "-") == 0) {
        f = stdout;
      } else {
        f = fopen(argv[6], "w");
        if (f == NULL) {
          perror(argv[6]);
          exit(1);
        }
      }
    }

    cw_initialize(port, drive);
    printf("Reading track %d, side %d...\n", track, side);
    cw_histo_track(drive, track, side, NNN, buf);
    if (f) fclose(f);
    cw_finalize(drive);

    eval_histo(buf, NNN);

  } else if (argc == 4) {
    int track, side, pass, split;
    FILE *infile;

    if (strcmp(argv[1], "-") == 0) {
      infile = stdin;
    } else {
      infile = fopen(argv[1], "r");
      if (infile == NULL) {
        perror(argv[1]);
        exit(1);
      }
    }
    cwclock = atoi(argv[2]);
    split = atoi(argv[3]);

    /*
     * Read the entire log file and histogram everything in it.  If
     * "split" is false and there are multiple consecutive passes on
     * any track, accumulate all the passes.
     */
    track = side = pass = -2;
    for (;;) {
      int rtrack, rside, rpass;
      rtrack = parse_track(infile, &rside, &rpass);
      if (rtrack != track || rside != side || split) {
        /* Encountered a new track: (rtrack,rside) */
        if (track >= 0) {
          /* Analyze and print data from the previous track: (track,side) */
          printf("Analyzing track %d, side %d, pass%s %d...\n",
                 track, side, split ? "" : "es", pass);
          eval_histo(buf, split ? 1 : pass);
        }
        if (rtrack < 0) {
          break;
        }
        /* Prep to parse the first pass of the new track */
        memset(buf, 0, 128*sizeof(int));
      }
      track = rtrack;
      side = rside;
      pass = rpass;
      /* Parse the current pass of the current track and accumulate histo */
      for (;;) {
        int sample;

        sample = parse_sample(infile);
        if (sample < 0 || sample >= 0x80) {
          break;
        }
        /* Simply ignore the index pulse position.  Thus we may double
         * count parts of the track.  We won't double count too much
         * if cw2dmk did an index to index read (-h1, the default).
         * With -h0, we might double count a lot, so the drive speed
         * estimate will be way off, but the peak detection and clock
         * rates should be okay.  Simply paying attention to the index
         * position (if it's even in the data) generally won't help
         * the -h0 case, as what we really need to do is clip out a
         * range of the data that represents exactly one full
         * revolution.
         */
        sample &= 0x7f;
        buf[sample]++;
      }
    }
  } else {
    usage();
  }

  return 0;
}
