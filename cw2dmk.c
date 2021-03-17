/*
 * cw2dmk: Dump floppy disk from Catweasel to .dmk format.
 * Copyright (C) 2000 Timothy Mann
 * $Id: cw2dmk.c,v 1.38 2010/01/15 20:32:56 mann Exp $
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

/*#define DEBUG5 1*/ /* Extra checking to detect Catweasel MK1 memory errors */
#define DEBUG5_BYTE 0x7e

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#if __DJGPP__
/* DJGPP doesn't support SA_RESETHAND, so reset manually for it in handler(). */
#define SA_RESETHAND 0
#endif
#if linux
#include <sys/io.h>
#endif
#include "crc.c"
#include "cwfloppy.h"
#include "dmk.h"
#include "kind.h"
#include "cwpci.h"
#include "version.h"

struct catweasel_contr c;

dmk_header_t dmk_header;
unsigned char* dmk_track = NULL;
unsigned char* dmk_merged_track = NULL;
int dmk_merged_track_len;
unsigned char* dmk_tmp_track = NULL;
FILE *dmk_file;

/* Constants and globals for decoding */
#define FM 1
#define MFM 2
#define RX02 3
#define MIXED 0
#define N_ENCS 4
char *enc_name[] = { "autodetect", "FM", "MFM", "RX02" };
int enc_count[N_ENCS];
int enc_sec[DMK_TKHDR_SIZE / 2];
int total_enc_count[N_ENCS];

/* Note: if track guess is too low, we won't notice, so we go very
   high.  I actually have seen a 43-track disk made in a 40-track
   drive.  However, many drives can't step that far. */
#define TRACKS_GUESS 86

/* Suppress FM address mark detection for a few bit times after each
   data CRC is seen.  Helps prevent seeing bogus marks in write
   splices. */
#define WRITE_SPLICE 32

struct TrackStat {
	int errcount;
	int good_sectors;
	int reused_sectors;
	int enc_count[N_ENCS];
	int enc_sec[DMK_TKHDR_SIZE / 2];
};

struct TrackStat merged_stat;

int kind = -1;
int maxsize = 3;  /* 177x/179x look at only low-order 2 bits */
unsigned long long accum, taccum;
int bits;
int ibyte, dbyte;
int cwclock = -1;
int fmthresh = -1;
int mfmthresh1 = -1;
int mfmthresh2 = -1;
int dmktracklen = -1;
float mfmshort = -1.0;
float postcomp = 0.5;
unsigned short crc;
int sizecode;
unsigned char premark;
int mark_after;
int write_splice; /* bit counter, >0 if we may be in a write splice */
int errcount;
int good_sectors;
int reused_sectors;
unsigned short matching_data_crc_val;
int matching_data_crcs;
int total_errcount;
int total_retries;
int total_good_sectors;
int good_tracks;
int err_tracks;
int index_edge;
int fmtimes = 2; /* record FM bytes twice; see man page */
int hole = 1;
int backward_am, flippy = 0;
int first_encoding;  /* first encoding to try on next track */
int curenc;
int uencoding = MIXED;
int reverse = 0;
int accum_sectors = 0;

char* plu(int val)
{
  return (val == 1) ? "" : "s";
}

/* Note: out_level values are used in usage message and on command line */
#define OUT_QUIET 0
#define OUT_SUMMARY 1
#define OUT_TSUMMARY 2
#define OUT_ERRORS 3
#define OUT_IDS 4
#define OUT_HEX 5
#define OUT_RAW 6
#define OUT_SAMPLES 7
int out_level = OUT_TSUMMARY;
int out_file_level = OUT_QUIET;
char *out_file_name;
FILE* out_file;

/* DMK stuff */

unsigned short* dmk_idam_p;
unsigned char* dmk_data_p;
int dmk_valid_id, dmk_awaiting_dam, dmk_awaiting_iam;
int dmk_iam_pos = -1;
int dmk_ignore = 0;
int dmk_ignored;
int dmk_full;
int cylseen = -1;
int prevcylseen;

/* Log a message. */
void 
msg(int level, const char *fmt, ...)
{
  va_list args;

  if (level <= out_level &&
      !(level == OUT_RAW && out_level != OUT_RAW) &&
      !(level == OUT_HEX && out_level == OUT_RAW)) {
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
  }
  if (out_file && level <= out_file_level &&
      !(level == OUT_RAW && out_file_level != OUT_RAW) &&
      !(level == OUT_HEX && out_file_level == OUT_RAW)) {
    va_start(args, fmt);
    vfprintf(out_file, fmt, args);
    va_end(args);
  }
}


/* True if we are ignoring data while waiting for an iam or for the
   first idam */
int
dmk_awaiting_track_start(void)
{
  if (dmk_iam_pos == -1) {
    return !hole && (unsigned char*) dmk_idam_p == dmk_track;
  } else {
    return dmk_awaiting_iam;
  }
}


int
dmk_in_range(void)
{
  if (dmk_full) return 0;
  if (dmk_ignored < dmk_ignore) {
    dmk_ignored++;
    return 0;
  }
  /* Stop at leading edge of last index unless in sector data. */
  if (hole && index_edge >= 3 && dbyte == -1) {
    msg(OUT_HEX, "[index edge %d] ", index_edge);
    dmk_full = 1;
    return 0;
  }
  return 1;
}


void
dmk_data(unsigned char byte, int encoding)
{
  if (!dmk_in_range()) return;
  if (dmk_awaiting_dam && byte >= 0xf8 && byte <= 0xfd) {
    /* Kludge: DMK doesn't tag DAMs, so noise after the ID bytes
       but before the DAM must not have the data bit pattern of a DAM */
    byte = 0xf0;
  }
  if (dmk_data_p - dmk_track <= dmk_header.tracklen - 2) {
    *dmk_data_p++ = byte;
    if (encoding == FM && !(dmk_header.options & DMK_SDEN_OPT)) {
      *dmk_data_p++ = byte;
    }
  }
  if (dmk_data_p - dmk_track > dmk_header.tracklen - 2) {
    /* No room for more bytes after this one */
    msg(OUT_HEX, "[DMK track buffer full] ");
    dmk_full = 1;
  }
}


void
dmk_idam(unsigned char byte, int encoding)
{
  unsigned short idamp;
  if (!dmk_in_range()) return;

  if (!dmk_awaiting_iam && dmk_awaiting_track_start()) {
    /* In this mode, we position the first IDAM a nominal distance
       from the start of the track, to make sure that (1) the whole
       track will fit and (2) if dmk2cw is used to write the image
       back to a real disk, the first IDAM won't be too close to the
       index hole.  */
#define GAP1PLUS 48
    int bytesread = dmk_data_p - (dmk_track + DMK_TKHDR_SIZE);
    if (bytesread < GAP1PLUS) {
      /* Not enough bytes read yet.  Move read bytes forward and add fill. */
      memmove(dmk_track + DMK_TKHDR_SIZE + GAP1PLUS - bytesread,
	      dmk_track + DMK_TKHDR_SIZE,
	      bytesread);
      memset(dmk_track + DMK_TKHDR_SIZE,
	     (encoding == MFM) ? 0x4e : 0xff,
	     GAP1PLUS - bytesread);
    } else {
      /* Too many bytes read.  Move last GAP1PLUS back and throw rest away. */
      memmove(dmk_track + DMK_TKHDR_SIZE,
	      dmk_track + DMK_TKHDR_SIZE + bytesread - GAP1PLUS,
	      GAP1PLUS);
    }
    dmk_data_p = dmk_track + DMK_TKHDR_SIZE + GAP1PLUS;
  }

  dmk_awaiting_dam = 0;
  dmk_valid_id = 0;
  idamp = dmk_data_p - dmk_track;
  if (encoding == MFM) {
    idamp |= DMK_DDEN_FLAG;
  }
  if (dmk_data_p < dmk_track + dmk_header.tracklen) {
    if ((unsigned char*) dmk_idam_p >= dmk_track + DMK_TKHDR_SIZE) {
      msg(OUT_ERRORS, "[too many AMs on track] ");
      errcount++;
    } else {
      if (accum_sectors)
        enc_sec[(dmk_idam_p - (unsigned short *)dmk_track)] = encoding;
      *dmk_idam_p++ = idamp;
      ibyte = 0;
      dmk_data(byte, encoding);
    }
  }
}


void
dmk_iam(unsigned char byte, int encoding)
{
  if (!dmk_in_range()) return;

  if (dmk_iam_pos >= 0) {
    /* If the user told us where to position the IAM...*/
    int bytesread = dmk_data_p - (dmk_track + DMK_TKHDR_SIZE);
    if (dmk_awaiting_iam || (unsigned char*) dmk_idam_p == dmk_track) {
      /* First IAM.  (Or a subsequent IAM with no IDAMs in between --
	 in the latter case, we assume the previous IAM(s) were
	 garbage.)  Position the IAM as instructed.  This can result
	 in data loss if an IAM appears somewhere in the middle of the
	 track, unless the read was for twice the track length as the
	 hole=0 (-h0) flag sets it. */
      int iam_pos = dmk_iam_pos;
      if (encoding == FM && !(dmk_header.options & DMK_SDEN_OPT)) {
	iam_pos *= 2;
      }
      if (bytesread < iam_pos) {
	/* Not enough bytes read yet.  Move read bytes forward and add fill. */
	memmove(dmk_track + DMK_TKHDR_SIZE + iam_pos - bytesread,
		dmk_track + DMK_TKHDR_SIZE,
		bytesread);
	memset(dmk_track + DMK_TKHDR_SIZE,
	       (encoding == MFM) ? 0x4e : 0xff,
	       iam_pos - bytesread);
      } else {
	/* Too many bytes read.  Move last iam_pos back and throw rest away. */
	memmove(dmk_track + DMK_TKHDR_SIZE,
		dmk_track + DMK_TKHDR_SIZE + bytesread - iam_pos,
		iam_pos);
      }
      dmk_data_p = dmk_track + DMK_TKHDR_SIZE + iam_pos;
      dmk_awaiting_iam = 0;
    } else {
      /* IAM that follows another IAM and one or more IDAMs.  If we're
	 >95% of the way around the track, assume it's actually the
	 first one again and stop here.  XXX This heuristic might be
	 useful even when the user isn't having us position by IAM. */
      if (bytesread > (dmk_header.tracklen - DMK_TKHDR_SIZE) * 95 / 100) {
	msg(OUT_IDS, "[stopping before second IAM] ");
	dmk_full = 1;
	return;
      }
    }
  }

  dmk_awaiting_dam = 0;
  dmk_valid_id = 0;
  dmk_data(byte, encoding);
}


void
dmk_write_header(void)
{
  rewind(dmk_file);
  /* assumes this machine is little-endian: */
  fwrite(&dmk_header, sizeof(dmk_header), 1, dmk_file);
}


void
dmk_write(void)
{
  int i;

  if (accum_sectors)
    good_sectors += reused_sectors;

  msg(OUT_TSUMMARY, " %d good sector%s", good_sectors, plu(good_sectors));
  if (accum_sectors && reused_sectors > 0)
    msg(OUT_TSUMMARY, " (%d reused)", reused_sectors);
  msg(OUT_TSUMMARY, ", %d error%s\n", errcount, plu(errcount));
  msg(OUT_IDS, "\n");

  total_good_sectors += good_sectors;
  total_errcount += errcount;
  if (errcount) {
    err_tracks++;
  } else if (good_sectors > 0) {
    good_tracks++;
  }
  for (i = 0; i < N_ENCS; i++) {
    total_enc_count[i] += enc_count[i];
  }
  fwrite(dmk_track, dmk_header.tracklen, 1, dmk_file);
}


void
dmk_init_track(void)
{
  int i;

  memset(dmk_track, 0, dmk_header.tracklen);
  dmk_idam_p = (unsigned short*) dmk_track;
  dmk_data_p = dmk_track + DMK_TKHDR_SIZE;
  dmk_awaiting_dam = 0;
  dmk_valid_id = 0;
  dmk_full = 0;
  good_sectors = 0;
  if (accum_sectors)
    reused_sectors = 0;
  for (i = 0; i < N_ENCS; i++) {
    enc_count[i] = 0;
  }
  errcount = 0;
  backward_am = 0;
  dmk_ignored = 0;
  if (dmk_ignore < 0) {
    i = dmk_ignore;
    while (i++) *dmk_data_p++ = 0xff;
  }
  cylseen = -1;
  if (dmk_iam_pos >= 0) {
    dmk_awaiting_iam = 1;
  }
  write_splice = 0;
}


void
check_missing_dam(void)
{
  if (dmk_awaiting_dam)
    msg(OUT_ERRORS, "[missing DAM] ");
  else if (dbyte > 0)
    msg(OUT_ERRORS, "[incomplete sector data] ");
  else
    return;

  dmk_awaiting_dam = 0;
  dmk_valid_id = 0;
  dbyte = ibyte = -1;
  errcount++;
  if (accum_sectors)
    dmk_idam_p[-1] |= DMK_EXTRA_FLAG;
}


int
dmk_check_wraparound(void)
{
  /* Once we've read 95% of the track, if we see a sector ID that's
     identical to the first one we saw on the track, conclude that we
     wrapped around and are seeing the first one again, and
     retroactively ignore it. */
  unsigned short first_idamp, last_idamp;
  int cmplen;
  if (dmk_data_p - dmk_track - DMK_TKHDR_SIZE <
      (dmk_header.tracklen - DMK_TKHDR_SIZE) * 95 / 100) {
    return 0;
  }
  first_idamp = *(unsigned short*) dmk_track;
  last_idamp = *(dmk_idam_p - 1);
  if (first_idamp == last_idamp) return 0;
  if ((first_idamp & DMK_DDEN_FLAG) != (last_idamp & DMK_DDEN_FLAG)) return 0;
  if ((first_idamp & DMK_DDEN_FLAG) || (dmk_header.options & DMK_SDEN_OPT)) {
    cmplen = 5;
  } else {
    cmplen = 10;
  }
  if (memcmp(&dmk_track[first_idamp & DMK_IDAMP_BITS],
	     &dmk_track[last_idamp & DMK_IDAMP_BITS], cmplen) == 0) {
    msg(OUT_ERRORS, "[wraparound] ");
    *--dmk_idam_p = 0;
    dmk_awaiting_dam = 0;
    ibyte = -1;
    dmk_full = 1;
    return 1;
  }
  return 0;
}

// Get pointer to sector N in rotational order.
unsigned char*
dmk_get_phys_sector(unsigned char *track, int n)
{
  int off;

  if (n < 0 || n >= DMK_TKHDR_SIZE / 2)
    return NULL;

  off = ((short *)track)[n] & DMK_IDAMP_BITS;
  // IDAM offsets skip over IDAM offset table so can never be 0.
  if (off == 0)
    return NULL;

  // Filter out bogus offset.  The mininum valid sector size here is really
  // only avoiding very bad situations.
  if (off < 0 || off > dmktracklen - 10)
    return NULL;

  return track + off;
}

// Get length of sector N in rotational order
int 
dmk_get_phys_sector_len(unsigned char *track, int n, int tracklen)
{
  unsigned char* s0 = dmk_get_phys_sector(track, n);
  unsigned char* s1 = dmk_get_phys_sector(track, n + 1);

  if (!s0)
    return -1;

  if (s1) {
    if (s0 >= s1) {
      #if 0
      int i;
      printf("\nphysical sector misordering from %d to %d .. "
	     "off %d off %d max %d!\n",
      	     n, n + 1, s0 - track, s1 - track, dmktracklen);
      for (i = 0; i < 10; i++)
        printf("%x ", ((short *)track)[i]);
      printf("\n");
      if (track == dmk_tmp_track) printf("TMP track\n");
      if (track == dmk_track) printf("common track\n");
      if (track == dmk_merged_track) printf("merged track\n");
      #endif

      return -1;
    }

    return s1 - s0;
  }

  return tracklen - (s0 - (track + DMK_TKHDR_SIZE));
}

int
dmk_get_sector_num(unsigned char *secdata)
{
  // Single density repeats every byte in DMK format.  If we see a repeat
  // of the sector ID then we know the sector number is at twice the offset.
  return secdata[secdata[1] == 0xfe ? 6 : 3];
}

int
copy_preamble(unsigned char **dst, unsigned char *track)
{
  unsigned char *pre_end = dmk_get_phys_sector(track, 0);
  int pre_len;

  if (!pre_end || pre_end <= track + DMK_TKHDR_SIZE)
    return 0;

  pre_len = pre_end - (track + DMK_TKHDR_SIZE);
  memcpy(*dst, track + DMK_TKHDR_SIZE, pre_len);
  *dst += pre_len;

  return 1;
}

// Go over the track we have read and replace any bad sectors with sectors
// from any previous read attempts.
// Takes a very simple-minded approach and cannot cope with a situation
// where sectors appear to be missing because of damage to the IDAM or DAM
// headers.
void
dmk_merge_sectors(void)
{
  int tracklen = dmk_data_p - (dmk_track + DMK_TKHDR_SIZE);
  unsigned char *tmp_data_p = dmk_tmp_track + DMK_TKHDR_SIZE;
  short *idam_p = (short *)dmk_track;
  short *tmp_idam_p = (short *)dmk_tmp_track;
  short *merged_idam_p = (short *)dmk_merged_track;
  unsigned char *dmk_sec;
  int cur;
  int overflow = 0;
  int best_errcount;
  int best_repair;
  struct TrackStat tmp_stat;
  enum Pick { Merged, Current, Tmp } best;

  // As a special case, use the track as-is if it read without error.
  if (errcount == 0) {
    memcpy(dmk_merged_track, dmk_track, DMK_TKHDR_SIZE + tracklen);
    dmk_merged_track_len = tracklen;
    merged_stat.errcount = errcount;
    merged_stat.good_sectors = good_sectors;
    merged_stat.reused_sectors = 0;
    memcpy(merged_stat.enc_count, enc_count, sizeof enc_count);
    memcpy(merged_stat.enc_sec, enc_sec, sizeof enc_sec);
    return;
  }

  memset(dmk_tmp_track, 0, DMK_TKHDR_SIZE);
  tmp_stat.errcount = errcount;
  tmp_stat.good_sectors = good_sectors;
  tmp_stat.reused_sectors = 0;
  memcpy(tmp_stat.enc_count, enc_count, sizeof enc_count);
  memcpy(tmp_stat.enc_sec, enc_sec, sizeof enc_sec);

  for (cur = 0; (dmk_sec = dmk_get_phys_sector(dmk_track, cur)); cur++) {
    int replaced = 0;
    // Bad sector?  See if we can find a replacement
    if (idam_p[cur] & DMK_EXTRA_FLAG) {
      int secnum = dmk_get_sector_num(dmk_sec), prev;
      unsigned char *prev_sec;
      for (prev = 0;
	   (prev_sec = dmk_get_phys_sector(dmk_merged_track, prev));
	   prev++) {
        int seclen = dmk_get_phys_sector_len(dmk_merged_track, prev,
					     dmk_merged_track_len);
        if (dmk_get_sector_num(prev_sec) != secnum) 
	  continue;

	// Ignore previous sector if it had an error
	if (merged_idam_p[prev] & DMK_EXTRA_FLAG)
	  continue;

	// The very first sector needs the pre-amble copied over, too.
	// We only understand this if the first sector is replacing the first
	// sector.  If not, then we skip because best not create bogus data.
	if (cur == 0) {
	  if (prev != 0)
	    continue;

	  if (!copy_preamble(&tmp_data_p, dmk_merged_track))
	    continue;
	}

	// Don't overflow the merged track.
	if (seclen <= 0 || tmp_data_p + seclen > dmk_tmp_track + dmktracklen)
	  continue;

	msg(OUT_ERRORS, "[reuse %02x] ", secnum);

	*tmp_idam_p++ = (merged_idam_p[prev] & ~DMK_IDAMP_BITS) |
	  ((tmp_data_p - dmk_tmp_track) & DMK_IDAMP_BITS);

	memcpy(tmp_data_p, prev_sec, seclen);
	tmp_data_p += seclen;
	replaced = 1;
	tmp_stat.reused_sectors++;
	tmp_stat.enc_count[enc_sec[cur]]++;
	// There should be an error for every bad sector, but just
	// to be careful.
	if (tmp_stat.errcount > 0)
	  tmp_stat.errcount--;
	break;
      }
    }

    if (!replaced) {
      // Copy the sector we have whether it be a good or bad read.
      int seclen = dmk_get_phys_sector_len(dmk_track, cur, tracklen);

      // Need to copy preamble if we are the first sector
      if (cur == 0 && !copy_preamble(&tmp_data_p, dmk_track))
        overflow = 1;
      else if (seclen < 0 || tmp_data_p + seclen > dmk_tmp_track + dmktracklen)
        overflow = 1;
      else {
        *tmp_idam_p++ = (idam_p[cur] & ~DMK_IDAMP_BITS) |
      	  ((tmp_data_p - dmk_tmp_track) & DMK_IDAMP_BITS);
        memcpy(tmp_data_p, dmk_sec, seclen);
        tmp_data_p += seclen;
      }
    }
  }

  // dmk_tmp_track has tmp_stat.errcount errors
  // (or is unusable if overflow is set).
  // dmk_merged_track has merged_stat.errcount errors.
  // dmk_track has errcount errors.

  // We want to keep the best as determined by the lowest error count and
  // that will become our merged track.

  best = Current;
  best_errcount = errcount;
  best_repair = 0;
  // overflow means that the candidate merged track tmp is not viable.
  if (!overflow && tmp_stat.errcount < best_errcount) {
    best = Tmp;
    best_errcount = tmp_stat.errcount;
    best_repair = tmp_stat.reused_sectors;
  }
  // If we have a previous merged track, it may still be the best.
  // Especially if it has fewer repairs.
  if (dmk_merged_track_len > 0) {
    if (merged_stat.errcount < best_errcount ||
        (merged_stat.errcount == best_errcount &&
	 merged_stat.reused_sectors < best_repair))
    {
      best = Merged;
      best_errcount = merged_stat.errcount;
      best_repair = merged_stat.reused_sectors;
    }
  }

  //msg(OUT_ERRORS, "(%d,%d,%d) ", errcount, tmp_stat.errcount,
  //    merged_stat.errcount);

  switch (best) {
  default:
  case Current:
    msg(OUT_ERRORS, "[using current] ");
    memcpy(dmk_merged_track, dmk_track, DMK_TKHDR_SIZE + tracklen);
    dmk_merged_track_len = tracklen;
    merged_stat.good_sectors = good_sectors;
    merged_stat.reused_sectors = 0;
    memcpy(merged_stat.enc_count, enc_count, sizeof enc_count);
    memcpy(merged_stat.enc_sec, enc_sec, sizeof enc_sec);
    break;
  case Tmp:
    msg(OUT_ERRORS, "[using merged] ");
    dmk_merged_track_len = tmp_data_p - (dmk_tmp_track + DMK_TKHDR_SIZE);
    memcpy(dmk_merged_track, dmk_tmp_track,
	   DMK_TKHDR_SIZE + dmk_merged_track_len);
    merged_stat = tmp_stat;
    break;
  case Merged:
    msg(OUT_ERRORS, "[using previous] ");
    break;
  }

  merged_stat.errcount = best_errcount;
}

int
secsize(int sizecode, int encoding)
{
  switch (encoding) {
  case MFM:
    /* 179x can only do sizes 128, 256, 512, 1024, and ignores
       higher-order bits.  If you need to read a 765-formatted disk
       with larger sectors, change maxsize with the -z
       command line option. */
    return 128 << (sizecode % (maxsize + 1));

  case FM:
  default:
    /* WD1771 has two different encodings for sector size, depending on
       a bit in the read/write command that is not recorded on disk.
       We guess IBM encoding if the size is <= maxsize, non-IBM
       if larger.  This doesn't really matter for demodulating the
       data bytes, only for checking the CRC.  */
    if (sizecode <= maxsize) {
      /* IBM */
      return 128 << sizecode;
    } else {
      /* non-IBM */
      return 16 * (sizecode ? sizecode : 256);
    }

  case RX02:
    return 256 << (sizecode % (maxsize + 1));
  }
}


void
init_decoder(void)
{
  accum = 0;
  taccum = 0;
  bits = 0;
  ibyte = dbyte = -1;
  premark = 0;
  mark_after = -1;
  curenc = first_encoding;
}


int
mfm_valid_clock(unsigned long long accum)
{
  /* Check for valid clock bits */
  unsigned int xclock = ~((accum >> 1) | (accum << 1)) & 0xaaaa;
  unsigned int clock = accum & 0xaaaa;
  if (xclock != clock) {
    //msg(OUT_ERRORS, "[clock exp %04x got %04x]", xclock, clock);
    return 0;
  }
  return 1;
}


/* Window used to undo RX02 MFM transform */
#if 1
#define WINDOW 4   /* change aligned 1000 -> 0101 */
#else
#define WINDOW 12  /* change aligned x01000100010 -> x00101010100 */
#endif


void
change_enc(int newenc)
{
  if (curenc != newenc) {
    msg(OUT_ERRORS, "[%s->%s] ", enc_name[curenc], enc_name[newenc]);
    curenc = newenc;
  }
}


/*
 * Main routine of the FM/MFM/RX02 decoder.  The input is a stream of
 * alternating clock/data bits, passed in one by one.  See decoder.txt
 * for documentation on how the decoder works.
 */
void
process_bit(int bit)
{
  static int curcyl = 0;
  static unsigned short crc_val, crc_val_saved;
  unsigned char val = 0;
  int i;

  accum = (accum << 1) + bit;
  taccum = (taccum << 1) + bit;
  bits++;
  if (mark_after >= 0) mark_after--;
  if (write_splice > 0) write_splice--;

  /*
   * Pre-detect address marks: we shift bits into the low-order end of
   * our 64-bit shift register (accum), look for marks in the lower
   * half, but decode data from the upper half.  When we recognize a
   * mark (or certain other patterns), we repeat or drop some bits to
   * achieve proper clock/data separatation and proper byte-alignment.
   * Pre-detecting the marks lets us do this adjustment earlier and
   * decode data more cleanly.
   *
   * We always sample bits at the MFM rate (twice the FM rate), but
   * we look for both FM and MFM marks at the same time.  There is
   * ambiguity here if we're dealing with normal (not DEC-modified)
   * MFM, because FM marks can be legitimate MFM data.  So we don't
   * look for FM marks while we think we're inside an MFM ID or data
   * block, only in gaps.  With -e2, we don't look for FM marks at
   * all.
   */

  /*
   * For FM and RX02 marks, we look at 9 data bits (including a
   * leading 0), which ends up being 36 bits of accum (2x for clocks,
   * another 2x for the double sampling rate).  We must not look
   * inside a region that can contain standard MFM data.
   */
  if (uencoding != MFM && bits >= 36 && !write_splice &&
      (curenc != MFM || (ibyte == -1 && dbyte == -1 && mark_after == -1))) {
    switch (accum & 0xfffffffffULL) {
    case 0x8aa2a2a88ULL:  /* 0xfc / 0xd7: Index address mark */
    case 0x8aa222aa8ULL:  /* 0xfe / 0xc7: ID address mark */
    case 0x8aa222888ULL:  /* 0xf8 / 0xc7: Standard deleted DAM */
    case 0x8aa22288aULL:  /* 0xf9 / 0xc7: RX02 deleted DAM / WD1771 user DAM */
    case 0x8aa2228a8ULL:  /* 0xfa / 0xc7: WD1771 user DAM */
    case 0x8aa2228aaULL:  /* 0xfb / 0xc7: Standard DAM */
    case 0x8aa222a8aULL:  /* 0xfd / 0xc7: RX02 DAM */
      change_enc(FM);
      if (bits < 64 && bits >= 48) {
	msg(OUT_HEX, "(+%d)", 64-bits);
	bits = 64; /* byte-align by repeating some bits */
      } else if (bits < 48 && bits > 32) {
	msg(OUT_HEX, "(-%d)", bits-32);
	bits = 32; /* byte-align by dropping some bits */
      }
      mark_after = 32;
      premark = 0; // doesn't apply to FM marks
      break;

    case 0xa222a8888ULL:  /* Backward 0xf8-0xfd DAM */
      change_enc(FM);
      backward_am++;
      msg(OUT_ERRORS, "[backward AM] ");
      break;
    }
  }

  /*
   * For MFM premarks, we look at 16 data bits (two copies of the
   * premark), which ends up being 32 bits of accum (2x for clocks).
   */
  if (uencoding != FM && uencoding != RX02 &&
      bits >= 32 && !write_splice) {
    switch (accum & 0xffffffff) {
    case 0x52245224:
      /* Pre-index mark, 0xc2c2 with missing clock between bits 3 & 4
	 (using 0-origin big-endian counting!).  Would be 0x52a452a4
	 without missing clock. */
      change_enc(MFM);
      premark = 0xc2;
      if (bits < 64 && bits > 48) {
	msg(OUT_HEX, "(+%d)", 64-bits);
	bits = 64; /* byte-align by repeating some bits */
      }
      mark_after = bits;
      break;

    case 0x44894489:
      /* Pre-address mark, 0xa1a1 with missing clock between bits 4 & 5
	 (using 0-origin big-endian counting!).  Would be 0x44a944a9
	 without missing clock.  Reading a pre-address mark backward
	 also matches this pattern, but the following byte is then 0x80. */
      change_enc(MFM);
      premark = 0xa1;
      if (bits < 64 && bits > 48) {
	msg(OUT_HEX, "(+%d)", 64-bits);
	bits = 64; /* byte-align by repeating some bits */
      }
      mark_after = bits;
      break;

    case 0x55555555:
      if (curenc == MFM && mark_after < 0 &&
	  ibyte == -1 && dbyte == -1 && !(bits & 1)) {
	/* ff ff in gap.  This should probably be 00 00, so drop 1/2 bit */
	msg(OUT_HEX, "(-1)");
	bits--;
      }
      break;

    case 0x92549254:
      if (mark_after < 0 && ibyte == -1 && dbyte == -1) {
	/* 4e 4e in gap.  This should probably be byte-aligned */
	change_enc(MFM);
	if (bits < 64 && bits > 48) {
	  /* Byte-align by dropping bits */
	  msg(OUT_HEX, "(-%d)", bits-48);
	  bits = 48;
	}
      }
      break;
    }
  }

  /* Undo RX02 DEC-modified MFM transform (in taccum) */
#if WINDOW == 4
  if (bits >= WINDOW && (bits & 1) == 0 && (accum & 0xfULL) == 0x8ULL) {
    taccum = (taccum & ~0xfULL) | 0x5ULL;
  }
#else /* WINDOW == 12 */
  if (bits >= WINDOW && (bits & 1) == 0 && (accum & 0x7ffULL) == 0x222ULL) {
    taccum = (taccum & ~0x7ffULL) | 0x154ULL;
  }
#endif

  if (bits < 64) return;

  if (curenc == FM || curenc == MIXED) {
    /* Heuristic to detect being off by some number of bits */
    if (mark_after != 0 && ((accum >> 32) & 0xddddddddULL) != 0x88888888ULL) {
      for (i = 1; i <= 3; i++) {
	if (((accum >> (32 - i)) & 0xddddddddULL) == 0x88888888ULL) {
	  /* Ignore oldest i bits */
	  bits -= i;
	  msg(OUT_HEX, "(-%d)", i);
	  if (bits < 64) return;
	  break;
	}
      }
      if (i > 3) {
#if 0 /* Bad idea: fires way too often in FM gaps. */
	/* Check if it looks more like MFM */
	if (uencoding != FM && uencoding != RX02 &&
	    ibyte == -1 && dbyte == -1 && !write_splice &&
	    (accum & 0xaaaaaaaa00000000ULL) &&
	    (accum & 0x5555555500000000ULL)) {
	  for (i = 1; i <= 2; i++) {
	    if (mfm_valid_clock(accum >> (48 - i))) {
	      change_enc(MFM);
	      bits -= i;
	      msg(OUT_HEX, "(-%d)", i);
	      return;
	    }
	  }
	}	    
#endif
	/* Note bad clock pattern */
	msg(OUT_HEX, "?");
      }
    }
    for (i=0; i<8; i++) {
      val |= (accum & (1ULL << (4*i + 1 + 32))) >> (3*i + 1 + 32);
    }
    bits = 32;

  } else if (curenc == MFM) {
    for (i=0; i<8; i++) {
      val |= (accum & (1ULL << (2*i + 48))) >> (i + 48);
    }
    bits = 48;

  } else /* curenc == RX02 */ {
    for (i=0; i<8; i++) {
      val |= (taccum & (1ULL << (2*i + 48))) >> (i + 48);
    }
    bits = 48;
  }

  if (mark_after == 0) {
    mark_after = -1;
    switch (val) {
    case 0xfc:
      /* Index address mark */
      if (curenc == MFM && premark != 0xc2) break;
      check_missing_dam();
      msg(OUT_IDS, "\n#fc ");
      dmk_iam(0xfc, curenc);
      ibyte = -1;
      dbyte = -1;
      return;

    case 0xfe:
      /* ID address mark */
      if (curenc == MFM && premark != 0xa1) break;
      if (dmk_awaiting_iam) break;
      check_missing_dam();
      msg(OUT_IDS, "\n#fe ");
      dmk_idam(0xfe, curenc);
      crc = calc_crc1((curenc == MFM) ? 0xcdb4 : 0xffff, val);
      dbyte = -1;
      return;

    case 0xf8: /* Standard deleted data address mark */
    case 0xf9: /* WD1771 user or RX02 deleted data address mark */
    case 0xfa: /* WD1771 user data address mark */
    case 0xfb: /* Standard data address mark */
    case 0xfd: /* RX02 data address mark */
      if (dmk_awaiting_track_start() || !dmk_in_range()) break;
      if (curenc == MFM && premark != 0xa1) break;
      if (!dmk_awaiting_dam) {
	msg(OUT_ERRORS, "[unexpected DAM] ");
	errcount++;
	break;
      }
      dmk_awaiting_dam = 0;
      msg(OUT_HEX, "\n");
      msg(OUT_IDS, "#%2x ", val);
      dmk_data(val, curenc);
      if ((uencoding == MIXED || uencoding == RX02) &&
	  (val == 0xfd ||
	   (val == 0xf9 && (total_enc_count[RX02] + enc_count[RX02] > 0 ||
			    uencoding == RX02)))) {
	change_enc(RX02);
      }
      /* For MFM, premark a1a1a1 is included in the CRC */
      crc = calc_crc1((curenc == MFM) ? 0xcdb4 : 0xffff, val);
      ibyte = -1;
      dbyte = secsize(sizecode, curenc) + 2;
      return;

    case 0x80: /* MFM DAM or IDAM premark read backward */
      if (curenc != MFM || premark != 0xc2) break;
      backward_am++;
      msg(OUT_ERRORS, "[backward AM] ");
      break;

    default:
      /* Premark with no mark */
      //msg(OUT_ERRORS, "[dangling premark] ");
      //errcount++;
      break;
    }
  }

  switch (ibyte) {
  default:
    break;
  case 0:
    msg(OUT_IDS, "cyl=");
    curcyl = val;
    break;
  case 1:
    msg(OUT_IDS, "side=");
    break;
  case 2:
    msg(OUT_IDS, "sec=");
    break;
  case 3:
    msg(OUT_IDS, "size=");
    sizecode = val;
    break;
  case 4:
    msg(OUT_HEX, "crc=");
    break;
  case 6:
    if (crc == 0) {
      msg(OUT_IDS, "[good ID CRC] ");
      dmk_valid_id = 1;
    } else {
      msg(OUT_ERRORS, "[bad ID CRC] ");
      errcount++;
      if (accum_sectors)
	dmk_idam_p[-1] |= DMK_EXTRA_FLAG;
      ibyte = -1;
    }
    msg(OUT_HEX, "\n");
    dmk_awaiting_dam = 1;
    dmk_check_wraparound();
    break;
  case 18:
    /* Done with post-ID gap */
    ibyte = -1;
    break;
  }

  if (ibyte == 2) {
    msg(OUT_ERRORS, "%02x ", val);
  } else if (ibyte >= 0 && ibyte <= 3) {
    msg(OUT_IDS, "%02x ", val);
  } else {
    msg(OUT_SAMPLES, "<");
    msg(OUT_HEX, "%02x", val);
    msg(OUT_SAMPLES, ">");
    msg(OUT_HEX, " ", val);
    msg(OUT_RAW, "%c", val);
  }

  dmk_data(val, curenc);

  if (ibyte >= 0) ibyte++;
  if (dbyte > 0) dbyte--;
  crc = calc_crc1(crc, val);

  if (dbyte == 0) {
    crc_val = (crc_val << 8) | val;
    if (crc == 0) {
      msg(OUT_IDS, "[good data CRC] ");
      if (dmk_valid_id) {
	if (good_sectors == 0) first_encoding = curenc;
	good_sectors++;
	enc_count[curenc]++;
	cylseen = curcyl;
      }
    } else {
      msg(OUT_ERRORS, "[bad data CRC] ");
      errcount++;
      if (accum_sectors) {
        // Don't count both header and data CRC errors for a sector.
	// Because otherwise dropping a single error for a replacement sector
	// will not show it fully corrected.  Need to track errors/sector.
        if (dmk_idam_p[-1] & DMK_EXTRA_FLAG)
	  errcount--;
	dmk_idam_p[-1] |= DMK_EXTRA_FLAG;
      }
    }
    msg(OUT_HEX, "\n");
    if (good_sectors == 1) {
      crc_val_saved = crc_val;
      matching_data_crcs = 0;
    } else if (good_sectors > 1) {
      if (crc_val == crc_val_saved) {
	matching_data_crc_val = crc_val;
        matching_data_crcs++;
      }
    }
    dbyte = -1;
    dmk_valid_id = 0;
    write_splice = WRITE_SPLICE;
    if (curenc == RX02) {
      change_enc(FM);
    }
  } else if (dbyte == 1) {
    crc_val = val;
  }

  /* Predetect bad MFM clock pattern.  Can't detect at decode time
     because we need to look at 17 bits. */
  if (curenc == MFM && bits == 48 && !mfm_valid_clock(accum >> 32)) {
    if (mfm_valid_clock(accum >> 31)) {
      msg(OUT_HEX, "(-1)");
      bits--;
    } else {
      msg(OUT_HEX, "?");
    }
  }
}


/*
 * Convert Catweasel samples to strings of alternating clock/data bits
 * and pass them to process_bit for further decoding.
 * Ad hoc method using two fixed thresholds modified by a postcomp
 * factor.
 */
void
process_sample(int sample)
{
  static float adj = 0.0;
  int len;

  msg(OUT_SAMPLES, "%d", sample);
  if (uencoding == FM) {
    if (sample + adj <= fmthresh) {
      /* Short */
      len = 2;
    } else {
      /* Long */
      len = 4;
    }
  } else {
    if (sample + adj <= mfmthresh1) {
      /* Short */
      len = 2;
    } else if (sample + adj <= mfmthresh2) {
      /* Medium */
      len = 3;
    } else {
      /* Long */
      len = 4;
    }
    
  }
  adj = (sample - (len/2.0 * mfmshort * cwclock)) * postcomp;

  msg(OUT_SAMPLES, "%c ", "--sml"[len]);

  process_bit(1);
  while (--len) process_bit(0);
}


/* Push out any valid bits left in accum at end of track */
void
flush_bits(void)
{
  int i;
  for (i=0; i<63; i++) {
    process_bit(!(i&1));
  }
}


/* Main program */

void
cleanup(void)
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
set_kind(void)
{
  kind_desc* kd = &kinds[kind-1];
  cwclock = kd->cwclock;
  fmthresh = kd->fmthresh;
  mfmthresh1 = kd->mfmthresh1;
  mfmthresh2 = kd->mfmthresh2;
  dmktracklen = kd->cwtracklen;
  mfmshort = kd->mfmshort;
}


/* Do a histogram of a track, also counting the total
   number of catweasel clocks to go around the track.
*/
int
do_histogram(int drive, int track, int side, int histogram[128],
	     int* total_cycles, int* total_samples, float* first_peak)
{
  int b;
  int i, tc, ts;
  float peak;
  int pwidth, psamps, psampsw;

  tc = 0;
  ts = 0;
  for (i=0; i<128; i++) {
    histogram[i] = 0;
  }
  catweasel_seek(&c.drives[drive], track);
  /*
   * Use index-to-index read without marking index edges.  Although
   * this does count the width of the index pulse twice, it's fast and
   * quite accurate enough for a histogram.
   */
  if (!catweasel_read(&c.drives[drive], side ^ reverse, 1, 0, 0)) {
    return 0;
  }
  while ((b = catweasel_get_byte(&c)) != -1 && b < 0x80) {
    histogram[b & 0x7f]++;
    tc += b + 1;  /* not sure if the +1 is right */
    ts++;
  }

  /* Print histogram for debugging */
  for (i=0; i<128; i+=8) {
    msg(OUT_SAMPLES, "%3d: %06d %06d %06d %06d %06d %06d %06d %06d\n",
	i, histogram[i+0], histogram[i+1], histogram[i+2], histogram[i+3],
	histogram[i+4], histogram[i+5], histogram[i+6], histogram[i+7]);
  }

  /* Find first peak */
  i = 0;
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
  if (pwidth > 24) {
    /* Track is blank */
    peak = -1.0;
  } else {
    /* again not sure of +1.0 */
    peak = ((float) psampsw) / psamps + 1.0; 
  }

  *total_cycles = tc;
  *total_samples = ts;
  *first_peak = peak;
  return 1;
}


/* Guess the kind of drive and media in use */
void
detect_kind(int drive)
{
  int histogram[128];
  int total_cycles, total_samples;
  float peak, rpm, dclock;

  if (!do_histogram(drive, 0, 0, histogram,
		    &total_cycles, &total_samples, &peak)) {
    if (hole) {
      fprintf(stderr, "cw2dmk: No index hole detected\n");
    } else {
      fprintf(stderr,
	      "cw2dmk: No index hole; can't detect drive and media type\n");
      fprintf(stderr,
	      "  Try using the -k flag to specify the correct type\n");
    }
    exit(1);
  }

  if (peak < 0.0) {
    /* Track is blank */
    fprintf(stderr, "cw2dmk: Track 0 side 0 is unformatted\n");
    exit(1);
  } else {
    dclock = 7080.5 / peak;
  }

  /* Total cycles gives us the RPM */
  rpm = 7080500.0 / ((float)total_cycles) * 60.0;

  msg(OUT_SAMPLES, "Data clock approx %f kHz\n", dclock);
  msg(OUT_SAMPLES, "Drive speed approx %f RPM\n", rpm);

  if (rpm > 270.0 && rpm < 330.0) {
    /* 300 RPM */
    if (dclock > 225.0 && dclock < 287.5) {
      /* Data rate 250 kHz */
      kind = 2;
    } else if (dclock > 450.0 && dclock < 575.0) {
      /* Data rate 500 kHz */
      kind = 4;
    }
  } else if (rpm > 330.0 && rpm < 396.0) {
    /* 360 RPM */
    if (dclock > 270.0 && dclock < 345.0) {
      /* Data rate 300 kHz */
      kind = 1;
    } else if (dclock > 450.0 && dclock < 575.0) {
      /* Data rate 500 kHz */
      kind = 3;
    }
  }    

  if (kind == -1) {
    fprintf(stderr, "cw2dmk: Failed to detect drive and media type\n");
    fprintf(stderr, "  Data clock approx %f kHz\n", dclock);
    fprintf(stderr, "  Drive speed approx %f RPM\n", rpm);
    exit(1);
  }
  set_kind();

  msg(OUT_QUIET + 1, "Detected %s\n", kinds[kind-1].description);
  fflush(stdout);
}


/* Check if back side is formatted */
int
detect_sides(int drive)
{
  int res = 1;
  int histogram[128];
  int total_cycles, total_samples;
  float peak;

  if (!do_histogram(drive, 0, 1, histogram,
		    &total_cycles, &total_samples, &peak)) {
    fprintf(stderr,
	    "cw2dmk: No index hole; can't detect if side 1 is formatted\n");
    exit(1);
  }

  if (peak > 0.0) {
    float dclock = 7080.5 / peak;
    if (dclock > 225.0 && dclock < 575.0) {
      res = 2;
    }
  }
  msg(OUT_QUIET + 1, "Detected side 1 %sformatted\n",
      res == 1 ? "not " : "");
  fflush(stdout);
  return res;
}


/* Command-line parameters */
int port = 0;
int tracks = -1;
int sides = -1;
int steps = -1;
int drive = -1;
int retries = 4;
int alternate = 0;

void usage(void)
{
  printf("\nUsage: cw2dmk [options] file.dmk\n");
  printf("\n Options [defaults in brackets]:\n");
  printf(" -d drive      Drive unit, 0 or 1, or -1 to autodetect [%d]\n",
	 drive);
  printf(" -v verbosity  Amount of output [%d]\n", out_level);
  printf("               0 = No output\n");
  printf("               1 = Summary of disk\n");
  printf("               2 = + summary of each track\n");
  printf("               3 = + individual errors\n");
  printf("               4 = + track IDs and DAMs\n");
  printf("               5 = + hex data and event flags\n");
  printf("               6 = like 4, but with raw data too\n");
  printf("               7 = like 5, but with Catweasel samples too\n");
  printf("               21 = level 2 to logfile, 1 to screen, etc.\n");
  printf(" -u logfile    Log output to the give file [none]\n");
  printf("\n Options to manually set values that are normally autodetected\n");
  printf(" -p port       I/O port base (MK1) or card number (MK3/4) [%d]\n",
	 port);
  printf(" -k kind       1 = %s\n", kinds[0].description);
  printf("               2 = %s\n", kinds[1].description);
  printf("               3 = %s\n", kinds[2].description);
  printf("               4 = %s\n", kinds[3].description);
  printf(" -m steps      Step multiplier, 1 or 2\n");
  printf(" -t tracks     Number of tracks per side\n");
  printf(" -s sides      Number of sides\n");
  printf(" -b chk_sides  Sides to check for pre-formatted sectors (0-3)\n");
  printf("               0 = disable check\n");
  printf("               1 = check only side 0\n");
  printf("               2 = check only side 1\n");
  printf("               3 = check both sides (default)\n");
  printf(" -C crc        Hex value of data CRC to ignore for "
	 "pre-formatted checks\n");
  printf(" -w fmtimes    Write FM bytes 1 or 2 times [%d]\n", fmtimes);
  printf(" -e encoding   1 = FM (SD), 2 = MFM (DD or HD), 3 = RX02\n");
  printf("\n Special options for hard to read diskettes\n");
  printf(" -x retries    Number of retries on errors [%d]\n", retries);
  printf(" -a alternate  Alternate even/odd tracks on retries with -m2 [%d]\n",
	 alternate);
  printf("               0 = always even\n");
  printf("               1 = always odd\n");
  printf("               2 = even, then odd\n");
  printf("               3 = odd, then even\n");
  printf(" -j            Join sectors between retries (%s)\n",
	 accum_sectors ? "on" : "off");
  printf(" -o postcomp   Amount of read-postcompensation (0.0-1.0) [%.2f]\n",
	 postcomp);
  printf(" -h hole       Track start: 1 = index hole, 0 = anywhere [%d]\n",
	 hole);
  printf(" -g ign        Ignore first ign bytes of track [%d]\n", dmk_ignore);
  printf(" -i ipos       Force IAM to ipos from track start; "
	 "if -1, don't [%d]\n", dmk_iam_pos);
  printf(" -z maxsize    Allow sector sizes up to 128<<maxsize [%d]\n",
	 maxsize);
  printf(" -r reverse    0 = normal, 1 = reverse sides [%d]\n", reverse);
  printf("\n Fine-tuning options; effective only after the -k option\n");
  printf(" -c clock      Catweasel clock multipler [%d]\n", cwclock);
  printf(" -1 threshold  MFM threshold for short vs. medium [%d]\n",
	 mfmthresh1);
  printf(" -2 threshold  MFM threshold for medium vs. long [%d]\n",
	 mfmthresh2);
  printf(" -f threshold  FM-only (-e1) threshold for short vs. long [%d]\n",
	 fmthresh);
  printf(" -l bytes      DMK track length in bytes [%d]\n", dmktracklen);
  printf("\n");
  printf("cw2dmk version %s\n", VERSION);
  printf("\n");
  exit(1);
}


int
main(int argc, char** argv)
{
  int ch, track, side, headpos, readtime, i;
  int guess_sides = 0, guess_steps = 0, guess_tracks = 0;
  int check_preformatted = 3;
  int check_preformatted_skip_crc = 0x5d30;
  int cw_mk = 1;

  opterr = 0;
  for (;;) {
    ch = getopt(argc, argv,
		"p:d:v:u:k:m:t:s:e:w:x:a:o:h:g:i:z:r:c:1:2:f:l:b:C:j");
    if (ch == -1) break;
    switch (ch) {
    case 'p':
      port = strtol(optarg, NULL, 16);
      if (port < 0 || (port >= MK3_MAX_CARDS && port < MK1_MIN_PORT) ||
	  (port > MK1_MAX_PORT)) {
	fprintf(stderr,
		"cw2dmk: -p must be between 0x%x and 0x%x for MK3/4 cards,\n"
		"  or between 0x%x and 0x%x for MK1 cards.\n",
		0, MK3_MAX_CARDS-1, MK1_MIN_PORT, MK1_MAX_PORT);
	exit(1);
      }
      break;
    case 'd':
      drive = strtol(optarg, NULL, 0);
      if (drive < -1 || drive > 1) usage();
      break;
    case 'v':
      out_level = strtol(optarg, NULL, 0);
      if (out_level < OUT_QUIET || out_level > OUT_SAMPLES * 11) {
	usage();
      }
      out_file_level = out_level / 10;
      out_level = out_level % 10;
      break;
    case 'u':
      out_file_name = optarg;
      break;
    case 'k':
      kind = strtol(optarg, NULL, 0);
      if (kind < 1 || kind > NKINDS) usage();
      set_kind();
      break;
    case 'm':
      steps = strtol(optarg, NULL, 0);
      if (steps < 1 || steps > 2) usage();
      break;
    case 't':
      tracks = strtol(optarg, NULL, 0);
      if (tracks < 0 || tracks > 85) usage();
      break;
    case 's':
      sides = strtol(optarg, NULL, 0);
      if (sides < 1 || sides > 2) usage();
      break;
    case 'e':
      uencoding = strtol(optarg, NULL, 0);
      if (uencoding < FM || uencoding > RX02) usage();
      break;
    case 'w':
      fmtimes = strtol(optarg, NULL, 0);
      if (fmtimes != 1 && fmtimes != 2) usage();
      break;
    case 'x':
      retries = strtol(optarg, NULL, 0);
      if (retries < 0) usage();
      break;
    case 'a':
      alternate = strtol(optarg, NULL, 0);
      if (alternate < 0 || alternate > 3) usage();
      break;
    case 'o':
      postcomp = strtod(optarg, NULL);
      if (postcomp < 0.0 || postcomp > 1.0) usage();
      break;
    case 'h':
      hole = strtol(optarg, NULL, 0);
      if (hole < 0 || hole > 1) usage();
      break;
    case 'g':
      dmk_ignore = strtol(optarg, NULL, 0);
      break;
    case 'i':
      dmk_iam_pos = strtol(optarg, NULL, 0);
      break;
    case 'z':
      maxsize = strtol(optarg, NULL, 0);
      if (maxsize < 0 || maxsize > 255) usage();
      break;
    case 'r':
      reverse = strtol(optarg, NULL, 0);
      if (reverse < 0 || reverse > 1) usage();
      break;
    case 'c':
      cwclock = strtol(optarg, NULL, 0);
      if (kind == -1 || (cwclock != 1 && cwclock != 2 && cwclock != 4)) {
	usage();
      }
      break;
    case '1':
      mfmthresh1 = strtol(optarg, NULL, 0);
      if (kind == -1) usage();
      break;
    case '2':
      mfmthresh2 = strtol(optarg, NULL, 0);
      if (kind == -1) usage();
      break;
    case 'f':
      fmthresh = strtol(optarg, NULL, 0);
      if (kind == -1) usage();
      break;
    case 'l':
      dmktracklen = strtol(optarg, NULL, 0);
      if (dmktracklen < 0 || dmktracklen > 0x4000) usage();
      if (kind == -1) usage();
      break;
    case 'j':
      accum_sectors = 1;
      break;
    case 'b':
      check_preformatted = strtol(optarg, NULL, 0);
      break;
    case 'C':
      check_preformatted_skip_crc = strtol(optarg, NULL, 16);
      break;
    default:
      usage();
      break;
    }
  }

  if (optind >= argc) {
    usage();
  }

  if (out_file_name && out_file_level == OUT_QUIET) {
    /* Default: log to file at same level as screen */
    out_file_level = out_level;
  }
  if (!out_file_name && out_file_level > OUT_QUIET) {
    char *p;
    int len;

    p = strrchr(argv[optind], '.');
    if (p == NULL) {
      len = strlen(argv[optind]);
    } else {
      len = p - argv[optind];
    }
    out_file_name = (char *) malloc(len + 5);
    sprintf(out_file_name, "%.*s.log", len, argv[optind]);
  }

  /* Keep drive from spinning endlessly on (expected) signals */
  struct sigaction sa_def = { .sa_handler = handler, .sa_flags = SA_RESETHAND };
  struct sigaction sa_int = { .sa_handler = handler };
  int sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGTERM };

  for (int s = 0; s < sizeof(sigs)/sizeof(*sigs); ++s) {
    if (sigaction(sigs[s], sigs[s] == SIGINT ? &sa_int : &sa_def, 0) == -1) {
      fprintf(stderr, "sigaction failed for signal %d.\n", sigs[s]);
      exit(1);
    }
  }

#if linux
  if (geteuid() != 0) {
    fprintf(stderr, "cw2dmk: Must be setuid to root or be run as root\n");
    exit(1);
  }
#endif
  /* Detect PCI catweasel */
  if (port < 10) {
    port = pci_find_catweasel(port, &cw_mk);
  }

#if linux
  /* Get port access and drop other root privileges */
  /* We avoid opening files and calling msg() before this point */
  if ((cw_mk == 1 &&
       ioperm(port == -1 ? MK1_DEFAULT_PORT : port, 8, 1) == -1) ||
      (cw_mk >= 3 && iopl(3) == -1)) {
    fprintf(stderr, "cw2dmk: No access to I/O ports\n");
    exit(1);
  }
  setuid(getuid());
#endif

  /* Open log file if needed */
  if (out_file_name) {
    out_file = fopen(out_file_name, "w");
    if (out_file == NULL) {
      perror(out_file_name);
      exit(1);
    }
  }

  /* Log the version number and command line */
  msg(OUT_TSUMMARY, "cw2dmk %s\n", VERSION);
  msg(OUT_ERRORS, "Command line: ");
  for (i = 0; i < argc; i++) {
    msg(OUT_ERRORS, "%s ", argv[i]);
  }
  msg(OUT_ERRORS, "\n");

  /* Finish detecting and initializating Catweasel */
  if (port == -1) {
    port = MK1_DEFAULT_PORT;
    msg(OUT_SUMMARY, "Failed to detect Catweasel MK3/4 on PCI bus; "
	"looking for MK1 on ISA bus at 0x%x\n", port);
    fflush(stdout);
  }
  ch = catweasel_init_controller(&c, port, cw_mk, getenv("CW4FIRMWARE"))
    && catweasel_memtest(&c);
  if (ch) {
    msg(OUT_SUMMARY, "Detected Catweasel MK%d at port 0x%x\n", cw_mk, port);
    fflush(stdout);
  } else {
    fprintf(stderr, "cw2dmk: Failed to detect Catweasel at port 0x%x\n", port);
    exit(1);
  }
  if (cw_mk == 1 && cwclock == 4) {
    fprintf(stderr, "cw2dmk: Catweasel MK1 does not support 4x clock\n");
    exit(1);
  }

  if (atexit(cleanup)) {
    fprintf(stderr, "cw2dmk: Can't establish atexit() call.\n");
    exit(1);
  }

  /* Detect drive */
  if (drive == -1) {
    for (drive = 0; drive < 2; drive++) {
      msg(OUT_SUMMARY, "Looking for drive %d...", drive);
      fflush(stdout);
      catweasel_detect_drive(&c.drives[drive]);
      if (c.drives[drive].type == 1) {
	msg(OUT_SUMMARY, "detected\n");
	break;
      } else {
	msg(OUT_SUMMARY, "not detected\n");
      }
    }
    if (drive == 2) {
      fprintf(stderr, "cw2dmk: Failed to detect any drives\n");
      exit(1);
    }
  } else {
    msg(OUT_SUMMARY, "Looking for drive %d...", drive);
    fflush(stdout);
    catweasel_detect_drive(&c.drives[drive]);
    if (c.drives[drive].type == 1) {
      msg(OUT_SUMMARY, "detected\n");
    } else {
      msg(OUT_SUMMARY, "not detected\n");
      fprintf(stderr, "cw2dmk: Drive %d not detected; proceeding anyway\n",
	      drive);
    }
  }

  /* Open output file */
  dmk_file = fopen(argv[optind], "wb");
  if (dmk_file == NULL) {
    perror(argv[optind]);
    exit(1);
  }

  /* Select drive, start motor, wait for spinup */
  catweasel_select(&c, !drive, drive);
  catweasel_set_motor(&c.drives[drive], 1);
  catweasel_usleep(500000);

  /* Guess various parameters if not supplied */
  if (kind == -1) detect_kind(drive);
  if (sides == -1) {
    sides = detect_sides(drive);
    guess_sides = 1; /* still allow this guess to be changed */
  }
  if (steps == -1) {
    if (kind == 1) {
      steps = 2;
    } else {
      steps = 1;
    }
    guess_steps = 1;
  }
  if (tracks == -1) {
    tracks = TRACKS_GUESS / steps;
    guess_tracks = 1;
  }

  /* Set parameters for reading with or without an index hole. */
  if (hole) {
    if (c.mk == 1) {
      /* With CW MK1, use hardware hole-to-hole read */
      readtime = 0;
    } else {
      /*
       * With CW MK3, hardware hole-to-hole read can't be made to
       * store the hole locations in the data stream.  Use timed read
       * instead.  Read an extra 10% in case of sectors wrapping past
       * the hole.
       */
      readtime = 1.1 * kinds[kind-1].readtime;    
    }
  } else {
    /* Read for 2 revolutions */
    readtime = 2 * kinds[kind-1].readtime;
  }

 restart:
  if (guess_sides || guess_steps || guess_tracks) {
    msg(OUT_SUMMARY,
	"Trying %d side%s, %d tracks/side, %s stepping, %s encoding\n",
	sides, plu(sides), tracks, (steps == 1) ? "single" : "double",
	enc_name[uencoding]);
  }
  fflush(stdout);
  total_errcount = 0;
  total_retries = 0;
  total_good_sectors = 0;
  for (i = 0; i < N_ENCS; i++) {
    total_enc_count[i] = 0;
  }
  good_tracks = 0;
  err_tracks = 0;
  first_encoding = (uencoding == RX02 ? FM : uencoding);

  /* Set DMK parameters */
  memset(&dmk_header, 0, sizeof(dmk_header));
  dmk_header.ntracks = tracks;
  dmk_header.tracklen = dmktracklen; 
  dmk_header.options = ((sides == 1) ? DMK_SSIDE_OPT : 0) +
                       ((fmtimes == 1) ? DMK_SDEN_OPT : 0) +
                       ((uencoding == RX02) ? DMK_RX02_OPT : 0);
  dmk_write_header();
  if (dmk_track) free(dmk_track);
  dmk_track = (unsigned char*) malloc(dmktracklen);
  if (accum_sectors) {
    if (dmk_merged_track) free(dmk_merged_track);
    dmk_merged_track = (unsigned char*) malloc(dmktracklen);
    dmk_merged_track_len = 0;
    memset(dmk_merged_track, 0, dmktracklen);
    if (dmk_tmp_track) free(dmk_tmp_track);
    dmk_tmp_track = (unsigned char*) malloc(dmktracklen);
  }

  /* Loop over tracks */
  for (track=0; track<tracks; track++) {
    prevcylseen = cylseen;
    headpos = track * steps + ((steps == 2) ? (alternate & 1) : 0);

    /* Loop over sides */
    for (side=0; side<sides; side++) {
      int retry = 0;
      int failing;

      if (accum_sectors) {
	dmk_merged_track_len = 0;
	memset(dmk_merged_track, 0, dmktracklen);
	// Do not have to initialize merged_stat as dmk_merged_track_len == 0
	// will stop us from using that information.
      }

      /* Loop over retries */
      do {
	msg(OUT_TSUMMARY, "Track %d, side %d, pass %d:",
	    track, side, retry + 1);
	fflush(stdout);

	int b = 0, oldb;
#if DEBUG3
	int histogram[128], i;
	for (i=0; i<128; i++) histogram[i] = 0;
#endif
	/* Seek to correct track */
	if ((steps == 2) && (retry > 0) && (alternate & 2)) {
	  headpos ^= 1;
	}
	catweasel_seek(&c.drives[drive], headpos);
	dmk_init_track();
	init_decoder();
#if DEBUG5
	if (c.mk == 1) {
	  catweasel_fillmem(&c, DEBUG5_BYTE);
	}
#endif

	int cw_ret;
	if (hole) {
	  /*
	   * Do read from index hole to index hole.
	   */
	  cw_ret = catweasel_read(&c.drives[drive], side ^ reverse, cwclock,
			      0, 0);
	} else {
	  /*
	   * Do read.  Store index holes in the data stream; this
	   * helps detect wraparound and avoid duplicating data.
	   */
	  cw_ret = catweasel_read(&c.drives[drive], side ^ reverse, cwclock,
			      readtime, 1);
	}

	if (!cw_ret) {
	  fprintf(stderr, "cw2dmk: Read error\n");
	  exit(1);
	}

	/* Loop over samples */
	oldb = 0;
	index_edge = 0;
	while (!dmk_full) {
	  b = catweasel_get_byte(&c);
	  if (b == -1 || (b == 0x00 && oldb == 0x80)) {
	    msg(OUT_HEX, "[end of data] ");
	    break;
	  }
#if DEBUG5
	  if (c.mk == 1 && b == DEBUG5_BYTE) {
	    static int ecount = 0;
	    ecount++;
	    if (ecount == 16) {
	      fprintf(stderr,
		      "cw2dmk: Catweasel memory error?! See cw2dmk.txt\n");
	    }
	  }
#endif
	  /*
	   * Index hole edge check.
	   */ 
	  if ((oldb ^ b) & 0x80) {
	    index_edge++;
	    msg(OUT_HEX, (b & 0x80) ? "{" : "}");
	  }
	  oldb = b;
	  b &= 0x7f;
#if DEBUG3
	  histogram[b]++;
#endif

	  /* Process this sample */
	  process_sample(b);
	}

	/*
	 * All samples read; finish up this (re)try.
	 */
#if DEBUG3
	/* Print histogram for debugging */
	for (i=0; i<128; i+=8) {
	  printf("%3d: %06d %06d %06d %06d %06d %06d %06d %06d\n", i,
		 histogram[i+0], histogram[i+1], histogram[i+2],
		 histogram[i+3], histogram[i+4], histogram[i+5],
		 histogram[i+6], histogram[i+7]);
	}
#endif
	flush_bits();
	check_missing_dam();
	if (ibyte != -1) {
	  /* Ignore incomplete sector IDs; assume they are wraparound */
	  msg(OUT_IDS, "[wraparound] ");
	  *--dmk_idam_p = 0;
	}
	if (dbyte != -1) {
	  errcount++;
	  msg(OUT_ERRORS, "[incomplete sector data] ");
	}
	msg(OUT_IDS, "\n");
	if (track == 0 && side == 1 && good_sectors == 0 && 
	    backward_am >= 9 && backward_am > errcount) {
	  msg(OUT_ERRORS, "[possibly a flippy disk] ");
	  flippy = 1;
	}
	if (track == 0 && errcount == 0 &&
	    (matching_data_crcs+1) == good_sectors) {
	  static int preformatted_side_detected = 0;
	  if ((check_preformatted & (side+1)) &&
	    (check_preformatted_skip_crc != matching_data_crc_val)) {
	    msg(OUT_QUIET + 1, "[Pre-formatted side detected");
	    preformatted_side_detected++;
	    if ((preformatted_side_detected == 1 && sides == 1) ||
		 preformatted_side_detected == 2) {
	      msg(OUT_QUIET + 1, "; stopping read]\n");
	      exit(1);
	    }
	    if (side == 0) {
	      msg(OUT_QUIET + 1, "]\n");
	    } else {
	      msg(OUT_QUIET + 1, "; restarting single-sided]\n");
	      preformatted_side_detected = 0;
	      guess_sides = 0;
	      sides = 1;
	      goto restart;
	    }
	  }
	}
	if (guess_sides && side == 1) {
	  guess_sides = 0;
	  if (good_sectors == 0) {
	    msg(OUT_QUIET + 1, "[apparently single-sided; restarting]\n");
	    sides = 1;
	    goto restart;
	  }
	}
	if (guess_steps) {
	  if (track == 3) guess_steps = 0;
	  if (steps == 1) {
	    if ((track & 1) &&
		(good_sectors == 0 ||
		 cylseen == track - 1 || cylseen == track + 1)) {
	      msg(OUT_QUIET + 1,
		  "[double-stepping apparently needed; restarting]\n");
	      steps = 2;
	      if (guess_tracks) tracks = TRACKS_GUESS / steps;
	      goto restart;
	    }
	  } else {
	    if (good_sectors && track > 0 && cylseen == track * 2) {
	      msg(OUT_QUIET + 1,
		"[single-stepping apparently needed; restarting]\n");
	      steps = 1;
	      if (guess_tracks) tracks = TRACKS_GUESS / steps;
	      goto restart;
	    }	      
	  }
	}
	if (guess_tracks && (track == 35 || track >= 40) &&
	    (good_sectors == 0 ||
	     (side == 0 && cylseen == prevcylseen) ||
	     (side == 0 && track >= 80 && cylseen == track/2))) {
	  msg(OUT_QUIET + 1, "[apparently only %d tracks; done]\n", track);
	  dmk_header.ntracks = track;
	  dmk_write_header();
	  goto done;
	}

	if (accum_sectors) {
	  // Dump track before merging (unifdef for debugging)
	  #if 0
	  char filename[32];
	  sprintf(filename, "c_s%dt%02d_%d.trk", side, track, retry);
	  FILE *fp = fopen(filename, "wb");
	  if (!fp)
	    fprintf(stderr, "Could not write to '%s'\n", filename);
	  else {
	    fwrite(dmk_track, dmk_data_p - dmk_track, 1, fp);
	    fclose(fp);
	  }
	  #endif

	  dmk_merge_sectors();
	} 

	failing = (accum_sectors ? merged_stat.errcount : errcount) > 0;

	if (failing)
	  failing = retry++ <= retries;

	// Generally just reporting on the latest read.
	if (failing) {
	  msg(OUT_TSUMMARY, "[%d good, %d error%s]\n",
	      good_sectors, errcount, plu(errcount));
	}
      } while (failing);
      total_retries += retry;
      fflush(stdout);
      if (accum_sectors) {
	short *idam_p = (short *)dmk_track;
	int i;
      	memset(dmk_track, (curenc == MFM) ? 0x4e : 0xff, dmk_header.tracklen);
	memcpy(dmk_track, dmk_merged_track,
	       DMK_TKHDR_SIZE + dmk_merged_track_len);
	for (i = 0; i < DMK_TKHDR_SIZE / 2; i++)
	  *idam_p++ &= ~DMK_EXTRA_FLAG;

	errcount = merged_stat.errcount;
	good_sectors = merged_stat.good_sectors;
	reused_sectors = merged_stat.reused_sectors;
	memcpy(enc_count, merged_stat.enc_count, sizeof enc_count);
	memcpy(enc_sec, merged_stat.enc_sec, sizeof enc_sec);
      }
      dmk_write();
    }
  }
 done:

  cleanup();
  if (total_enc_count[RX02] > 0 && uencoding != RX02) {
    // XXX What if disk had some 0xf9 DAM sectors misinterpreted as
    // WD1771 FM instead of RX02-MFM before we detected RX02?  Ugh.
    // Should at least detect this and give an error.  Maybe
    // autorestart if it happens.  I believe it's quite unlikely, as I
    // suspect the 0xf9 DAM is never or almost never actually used on
    // RX02 disks.
    dmk_header.options |= DMK_RX02_OPT;
    dmk_write_header();
  }
  msg(OUT_SUMMARY, "\nTotals:\n");
  msg(OUT_SUMMARY,
      "%d good track%s, %d good sector%s (%d FM + %d MFM + %d RX02)\n",
      good_tracks, plu(good_tracks),
      total_good_sectors, plu(total_good_sectors),
      total_enc_count[FM], total_enc_count[MFM], total_enc_count[RX02]);
  msg(OUT_SUMMARY, "%d bad track%s, %d unrecovered error%s, %d retr%s\n",
      err_tracks, plu(err_tracks), total_errcount, plu(total_errcount),
      total_retries, (total_retries == 1) ? "y" : "ies");
  if (flippy) {
    msg(OUT_SUMMARY, "Possibly a flippy disk; check reverse side too\n");
  }
  return 0;
}
