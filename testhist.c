/* Catweasel test program
   From cwfloppy-0.2.1 package, copyright (c) 1998 Michael Krause
   Modified by Timothy Mann
   $Id: testhist.c,v 1.13 2005/04/24 04:16:45 mann Exp $

   Reads a track and prints a histogram of the samples (flux
   transition interarrival times) returned.  Also guesses the drive
   RPM and the bit clock rate, categorizes the samples as short,
   medium, or long (by searching for the expected 2 or 3 peaks in the
   histogram for FM or MFM encoding), and prints the mean and standard
   deviation of the actual length of samples in each category.  Also
   optionally dumps the samples to a file.

   The track is read NNN times (defined below).  When samples are
   being dumped to a file, each read is preceded by a pseudo-sample
   equal to 0x80 + read number (0-origin).  The actual samples can be
   at most 0x7f.

   Usage: testhist port drive track side clock [file]

   Clock is normally 0 to read SD/DD, 1 to read HD.  For Catweasel
   MK1, port should be the I/O port base, or 0 to default to 0x320.
   For Catweasel MK3/4, port should be 0 for the first Catweasel card in
   the machine, 1 for the second, etc.
*/

#define NNN 4

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#if linux
#include <sys/io.h>
#endif
#include "cwfloppy.h"
#include "cwpci.h"

FILE *f;
struct catweasel_contr c;
unsigned char trackbuffer[18*512];

int cwclock = 1;

static void histo_track(int drive, int track, int side, unsigned int *buf) 
{
  int b;
  int i;

  catweasel_seek(&c.drives[drive], track);

  memset(buf, 0, 128*sizeof(int));
  for(i=0;i<NNN;i++) {
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

static void eval_histo(unsigned int *histogram)
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
	   CWHZ * cwclock / psampsw * NNN * 60.0);
    
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


int main(int argc, char **argv) {
    int track, side, port, drive, ret;
    unsigned int buf[128];
    int cw_mk = 1;

    if(argc != 6 && argc != 7) {
	fprintf(stderr,
		"Usage: %s port drive track side clock [file]\n", argv[0]);
	return 1;
    }

    if (strcmp(argv[1], "ide0") == 0) {
        port = -0x1f0;
    } else if (strcmp(argv[1], "ide1") == 0) {
        port = -0x170;
    } else
    	port = strtol(argv[1], NULL, 0);
    drive = atoi(argv[2]);
    track = atoi(argv[3]);
    side = atoi(argv[4]);
    cwclock = atoi(argv[5]);
    if (argc == 7) {
      if (strcmp(argv[6], "-") == 0) {
	f = stdout;
      } else {
	f = fopen(argv[6], "w");
      }
    }

    /* Start Catweasel */
    if (port < 0) {
      port = -port;
      cw_mk = 2;
    }
    if (port < 10) {
      port = pci_find_catweasel(port, &cw_mk);
      if (port == -1) {
	port = MK1_DEFAULT_PORT;
	printf("Failed to detect Catweasel MK3/4 on PCI bus; "
	       "looking for MK1 on ISA bus at 0x%x\n", port);
      }
    }
#if linux
    if (((cw_mk < 3) && ioperm(port, 8, 1) == -1) ||
	(cw_mk >= 3 && iopl(3) == -1)) {
      fprintf(stderr, "testhist: No access to I/O ports\n");
      return 1;
    }
    setuid(getuid());
#endif
    ret = catweasel_init_controller(&c, port, cw_mk, getenv("CW4FIRMWARE"))
      && catweasel_memtest(&c);
    if (!ret) {
      fprintf(stderr, "testhist: Failed to detect Catweasel Mk%d at port 0x%x\n", cw_mk, port);
      return 1;
    }
    catweasel_detect_drive(&c.drives[drive]);
    if (c.drives[drive].type == 0) {
      fprintf(stderr, "testhist: Did not detect drive %d, but trying anyway\n",
	      drive);
    }

    catweasel_select(&c, !drive, drive);
    catweasel_set_motor(&c.drives[drive], 1);
    catweasel_usleep(500000);

    printf("Reading track %d, side %d...\n", track, side);
    histo_track(drive, track, side, buf);
    eval_histo(buf);

    catweasel_set_motor(&c.drives[drive], 0);
    catweasel_select(&c, 0, 0);
    
    catweasel_free_controller(&c);

    if (f) fclose(f);
    
    return 0;
}
