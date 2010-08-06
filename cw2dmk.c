/*
 * cw2dmk: Dump floppy disk from Catweasel to .dmk format.
 * Copyright (C) 2000 Timothy Mann
 * $Id: cw2dmk.c,v 1.33 2005/04/05 08:10:56 mann Exp $
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

/*#define DEBUG2 1*/ /* Print histogram and detected speeds for debugging */

#define DEBUG5 1 /* Do extra checking to detect Catweasel MK1 memory errors */
#define DEBUG5_BYTE 0x7e

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>
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
FILE *dmk_file;

/* Constants and globals for decoding */
#define FM 1
#define MFM 2
#define RX02 3
#define MIXED 0
char *enc_name[] = { "autodetect", "FM", "MFM", "RX02" };

/* Note: if track guess is too low, we won't notice, so we go very
   high.  I actually have seen a 43-track disk made in a 40-track
   drive.  However, many drives can't step that far. */
#define TRACKS_GUESS 86

/* Suppress FM address mark detection for a few bit times after each
   data CRC is seen.  Helps prevent seening bogus marks in write
   splices. */
#define WRITE_SPLICE 32

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
float postcomp = 0.0;
unsigned short crc;
int sizecode;
unsigned char premark;
int mark_after;
int write_splice; /* bit counter, >0 if we may be in a write splice */
int errcount;
int good_sectors;
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

char* plu(val)
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
dmk_awaiting_track_start()
{
  if (dmk_iam_pos == -1) {
    return !hole && (unsigned char*) dmk_idam_p == dmk_track;
  } else {
    return dmk_awaiting_iam;
  }
}

int
dmk_in_range()
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
dmk_dam(unsigned char byte, int encoding)
{
  if (!dmk_in_range()) return;
  if (!dmk_awaiting_dam && !dmk_awaiting_track_start()) {
    msg(OUT_ERRORS, "[unexpected DAM] ");
    errcount++;
  }
  dmk_awaiting_dam = 0;
  dmk_data(byte, encoding);
}

void
dmk_write_header()
{
  rewind(dmk_file);
  /* assumes this machine is little-endian: */
  fwrite(&dmk_header, sizeof(dmk_header), 1, dmk_file);
}

void
dmk_write()
{
  msg(OUT_TSUMMARY, "%d good sector%s, %d error%s\n",
      good_sectors, plu(good_sectors), errcount, plu(errcount));
  msg(OUT_IDS, "\n");

  total_good_sectors += good_sectors;
  total_errcount += errcount;
  if (errcount) {
    err_tracks++;
  } else {
    good_tracks++;
  }
  fwrite(dmk_track, dmk_header.tracklen, 1, dmk_file);
}

void
dmk_init_track()
{
  memset(dmk_track, 0, dmk_header.tracklen);
  dmk_idam_p = (unsigned short*) dmk_track;
  dmk_data_p = dmk_track + DMK_TKHDR_SIZE;
  dmk_awaiting_dam = 0;
  dmk_valid_id = 0;
  dmk_full = 0;
  good_sectors = 0;
  errcount = 0;
  backward_am = 0;
  dmk_ignored = 0;
  if (dmk_ignore < 0) {
    int i = dmk_ignore;
    while (i++) *dmk_data_p++ = 0xff;
  }
  cylseen = -1;
  if (dmk_iam_pos >= 0) {
    dmk_awaiting_iam = 1;
  }
  write_splice = 0;
}

void
check_missing_dam()
{
  if (!dmk_awaiting_dam) return;
  dmk_awaiting_dam = 0;
  dmk_valid_id = 0;
  dbyte = ibyte = -1;
  errcount++;
  msg(OUT_ERRORS, "[missing DAM] ");
}

int
dmk_check_wraparound()
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
    msg(OUT_IDS, "[wraparound] ");
    *--dmk_idam_p = 0;
    dmk_awaiting_dam = 0;
    ibyte = -1;
    dmk_full = 1;
    return 1;
  }
  return 0;
}

/* FM/MFM decoding stuff */

int
secsize(int sizecode, int encoding)
{
  if (encoding == MFM) {
    /* 179x can only do sizes 128, 256, 512, 1024, and ignores
       higher-order bits.  If you need to read a 765-formatted disk
       with larger sectors, change maxsize with the -z
       command line option. */
    return 128 << (sizecode % (maxsize + 1));
  } else {
    /* 1771 has two different encodings for sector size, depending on
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
  }
}

void
decode_init()
{
  accum = 0;
  taccum = 0;
  bits = 0;
  ibyte = dbyte = -1;
  premark = 0;
  mark_after = -1;
}

/* Check whether the bit vector encodes an FM address mark */
int
fm_mark(unsigned long long bitvec)
{
  if (write_splice) {
    write_splice--;
    return 0;
  }
  switch (bitvec & 0xffff) {
  case 0xf77a:  /* Index address mark, 0xfc with 0xd7 clock */
  case 0xf57e:  /* ID address mark, 0xfe with 0xc7 clock */
  case 0xf56a:  /* Data address mark, 0xf8 with 0xc7 clock */
  case 0xf56b:  /* Data address mark, 0xf9 with 0xc7 clock */
  case 0xf56e:  /* Data address mark, 0xfa with 0xc7 clock */
  case 0xf56f:  /* Data address mark, 0xfb with 0xc7 clock */
  case 0xf57b:  /* Undefined address mark, 0xfd with 0xc7 clock */
  case 0xb57a:  /* Data address mark (0xf8 to 0xfb) read backward */
    return 1;
  }
  return 0;
}

int
fm_bit(int bit)
{
  static int fmcyl;
  int i;
  unsigned char val = 0;
  int ret = FM;

  accum = (accum << 1) + bit;
  bits++;
  if (mark_after >= 0) mark_after--;

  if (bits < 16) return ret;

  /* Pre-detect address marks to achieve proper byte alignment */
  if (ibyte == -1) {
    if (fm_mark(accum)) {
      if (bits < 32 && bits >= 24) {
	msg(OUT_HEX, "(+%d)", 32-bits);
	bits = 32; /* byte-align by repeating some bits */
      } else if (bits < 24 && bits > 16) {
	msg(OUT_HEX, "(-%d)", bits-16);
	bits = 16; /* byte-align by dropping some bits */
      }
      mark_after = 16;
    }
  }
    
  if (bits < 32) return ret;

  if (mark_after != 0 && ((accum >> 16) & 0xaaaa) != 0xaaaa) {
    if (((accum >> 15) & 0xaaaa) == 0xaaaa) {
      /* Ignore oldest bit */
      bits--;
      msg(OUT_HEX, "(-1)");
      if (bits < 32) return ret;
    } else {
      /* Note bad clock pattern */
      msg(OUT_HEX, "?");
    }
  }
  for (i=0; i<8; i++) {
    val |= (accum & (1ULL << (2*i + 16))) >> (i + 16);
  }

  if (mark_after == 0) {
    switch (val) {
    case 0xfc:
      /* Index address mark, 0xfc with 0xd7 clock */
      check_missing_dam();
      msg(OUT_IDS, "\n#fc ");
      dmk_iam(0xfc, FM);
      bits = 16;
      ibyte = -1;
      dbyte = -1;
      break;

    case 0xfe:
      /* ID address mark, 0xfe with 0xc7 clock */
      if (dmk_awaiting_iam) break;
      check_missing_dam();
      msg(OUT_IDS, "\n#fe ");
      dmk_idam(0xfe, FM);
      bits = 16;
      crc = calc_crc1(0xffff, 0xfe);
      dbyte = -1;
      break;

    case 0xfd: /* Undefined address mark, 0xfd with 0xc7 clock */
      if (dmk_awaiting_dam && dmk_valid_id) ret = RX02;
      /* fall through */

    case 0xf8: /* Data address mark, 0xf8 with 0xc7 clock */
    case 0xf9: /* Data address mark, 0xf9 with 0xc7 clock */
    case 0xfa: /* Data address mark, 0xfa with 0xc7 clock */
    case 0xfb: /* Data address mark, 0xfb with 0xc7 clock */
      if (dmk_awaiting_track_start()) break;
      msg(OUT_HEX, "\n");
      msg(OUT_RAW, "\n");
      msg(OUT_IDS, "#%2x ", val);
      dmk_dam(val, FM);
      bits = 16;
      crc = calc_crc1(0xffff, val);
      ibyte = -1;
      dbyte = secsize(sizecode, FM) + 2;
      break;

    case 0x7c: /* Data address mark read backward */
      backward_am++;
      msg(OUT_ERRORS, "[backward AM] ");
      break;

    default:
      msg(OUT_QUIET, "[BUG]");
      break;
    }
  }

  if (bits < 32) return ret;

  switch (ibyte) {
  default:
    break;
  case 0:
    msg(OUT_IDS, "cyl=");
    fmcyl = val;
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
	
  if (ibyte >= 0 && ibyte <= 3) {
    msg(OUT_IDS, "%02x ", val);
  } else {
    msg(OUT_SAMPLES, "<");
    msg(OUT_HEX, "%02x", val);
    msg(OUT_SAMPLES, ">");
    msg(OUT_HEX, " ", val);
    msg(OUT_RAW, "%c", val);
  }

  dmk_data(val, FM);

  if (ibyte >= 0) ibyte++;
  if (dbyte > 0) dbyte--;
  crc = calc_crc1(crc, val);

  if (dbyte == 0) {
    if (crc == 0) {
      msg(OUT_IDS, "[good data CRC] ");
      if (dmk_valid_id) {
	if (good_sectors == 0) first_encoding = FM;
	good_sectors++;
	cylseen = fmcyl;
      }
    } else {
      msg(OUT_ERRORS, "[bad data CRC] ");
      errcount++;
    }
    msg(OUT_HEX, "\n");
    dbyte = -1;
    dmk_valid_id = 0;
    write_splice = WRITE_SPLICE;
  }

  bits = 16;
  return ret;
}

int
fm_decode(int sample)
{
  int enc;
  msg(OUT_SAMPLES, "%d", sample);
  if (sample <= fmthresh) {
    /* Short */
    msg(OUT_SAMPLES, "s ");
    enc = fm_bit(1);
  } else {
    /* Long */
    msg(OUT_SAMPLES, "l ");
    enc = fm_bit(1);
    if (enc != FM) return enc;
    enc = fm_bit(0);
  }
  return enc;
}

/* Shove any valid bits out of the 16-bit address mark detection window */
void
fm_flush()
{
  int i;
  for (i=0; i<16; i++) {
    fm_bit(!(i&1));
  }
}

int
mfm_valid_clock(unsigned long long accum)
{
  /* Check for valid clock bits */
  unsigned int xclock = ~((accum >> 1) | (accum << 1)) & 0xaaaa;
  unsigned int clock = accum & 0xaaaa;
  /*msg(OUT_ERRORS, "[clock exp %04x got %04x]", xclock, clock);*/
  return (xclock == clock);
}

void
mfm_bit(int bit)
{
  static int mfmcyl;
  accum = (accum << 1) + bit;
  bits++;
  if (mark_after >= 0) mark_after--;

  switch (accum & 0xffffffff) {
  case 0x52245224:
    /* Pre-index mark, 0xc2c2 with missing clock between bits 3 & 4
       (using 0-origin big-endian counting!).  Would be 0x52a452a4
       without missing clock. */
    premark = 0xc2;
    if (bits < 48 && bits > 32) {
      msg(OUT_HEX, "(+%d)", 48-bits);
      bits = 48; /* byte-align by repeating some bits */
    }
    mark_after = bits;
    break;

  case 0x44894489:
    /* Pre-address mark, 0xa1a1 with missing clock between bits 4 & 5
       (using 0-origin big-endian counting!).  Would be 0x44a944a9
       without missing clock. */
    premark = 0xa1;
    if (bits < 48 && bits > 32) {
      msg(OUT_HEX, "(+%d)", 48-bits);
      bits = 48; /* byte-align by repeating some bits */
    }
    mark_after = bits;
    break;

  case 0x55555555:
    if (!premark && ibyte == -1 && dbyte == -1 && !(bits & 1)) {
      /* ff ff in gap.  This should probably be 00 00, so drop 1/2 bit */
      msg(OUT_HEX, "(-1)");
      bits--;
    }
    break;

  case 0x92549254:
    if (ibyte == -1 && dbyte == -1) {
      /* 4e 4e in gap.  This should probably be byte-aligned */
      if (bits < 48 && bits > 32) {
	/* Byte-align by dropping bits */
	msg(OUT_HEX, "(-%d)", bits-32);
	bits = 32;
      }
    }
    break;

  default:
    break;
  }

  if (bits >= 48) {
    int i;
    unsigned char val = 0;

    /* Check for valid clock bits */
    if (!mfm_valid_clock(accum >> 32)) {
      if (!premark && mfm_valid_clock(accum >> 31)) {
	/* Ignore oldest bit */
	bits--;
	msg(OUT_HEX, "(-1)");
	if (bits < 48) return;
      } else {
	/* Note bad clock pattern */
	msg(OUT_HEX, "?");
      }
    }

    for (i=0; i<8; i++) {
      val |= (accum & (1ULL << (2*i + 32))) >> (i + 32);
    }
    
    if (mark_after == 0 && premark == 0xc2) {
      switch (val) {
      case 0xfc:
	/* Index address mark */
	check_missing_dam();
	msg(OUT_IDS, "\n#%02x ", val);
	dmk_iam(val, MFM);
	bits = 32;
	ibyte = -1;
	dbyte = -1;
	premark = 0;
	mark_after = -1;
	return;

      case 0x80:
	/* IDAM or DAM read backwards */
	backward_am++;
	msg(OUT_ERRORS, "[backward AM] ");
	/* fall through */

      default:
	/* Premark with no mark */
	premark = 0;
	mark_after = -1;
	break;
      }
    }

    if (mark_after == 0 && premark == 0xa1) {
      /* Premark sequence initializes CRC */
      crc = calc_crc1(0x968b, 0xa1); /* CRC of a1 a1 a1 */

      switch (val) {
      case 0xfe:
	/* ID address mark */
	if (dmk_awaiting_iam) break;
	check_missing_dam();
	msg(OUT_IDS, "\n#%02x ", val);
	dmk_idam(val, MFM);
	bits = 32;
	crc = calc_crc1(crc, val);
	dbyte = -1;
	premark = 0;
	mark_after = -1;
	return;

      case 0xf8:
      case 0xf9: /* probably unneeded */
      case 0xfa: /* probably unneeded */
      case 0xfb:
	if (dmk_awaiting_track_start()) break;
	/* Data address mark */
	msg(OUT_HEX, "\n");
	msg(OUT_IDS, "#%02x ", val);
	dmk_dam(val, MFM);
	bits = 32;
	crc = calc_crc1(crc, val);
	ibyte = -1;
	dbyte = secsize(sizecode, MFM) + 2;
	premark = 0;
	mark_after = -1;
	return;

      default:
	/* Premark with no mark */
	premark = 0;
	mark_after = -1;
	break;
      }
    }

    switch (ibyte) {
    default:
      break;
    case 0:
      msg(OUT_IDS, "cyl=");
      mfmcyl = val;
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
	
    if (ibyte >= 0 && ibyte <= 3) {
      msg(OUT_IDS, "%02x ", val);
    } else {
      msg(OUT_SAMPLES, "<");
      msg(OUT_HEX, "%02x", val);
      msg(OUT_SAMPLES, ">");
      msg(OUT_HEX, " ", val);
      msg(OUT_RAW, "%c", val);
    }

    dmk_data(val, MFM);

    if (ibyte >= 0) ibyte++;
    if (dbyte > 0) dbyte--;
    crc = calc_crc1(crc, val);

    if (dbyte == 0) {
      if (crc == 0) {
	msg(OUT_IDS, "[good data CRC] ");
	if (dmk_valid_id) {
	  if (good_sectors == 0) first_encoding = MFM;
	  good_sectors++;
	  cylseen = mfmcyl;
	}
      } else {
	msg(OUT_ERRORS, "[bad data CRC] ");
	errcount++;
      }
      msg(OUT_HEX, "\n");
      dbyte = -1;
      dmk_valid_id = 0;
      write_splice = WRITE_SPLICE;
    }

    bits = 32;
  }
}

void
mfm_decode(int sample)
{
  static float adj = 0.0;
  int len;

  msg(OUT_SAMPLES, "%d", sample);
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
  adj = (sample - (len/2.0 * mfmshort * cwclock)) * postcomp;

  msg(OUT_SAMPLES, "%c ", "--sml"[len]);

  mfm_bit(1);
  while (--len) mfm_bit(0);
}

/* Shove any valid bits out of the 32-bit address mark detection window */
void
mfm_flush()
{
  int i;
  for (i=0; i<32; i++) {
    mfm_bit(!(i&1));
  }
}

/*
RX02 DEC-modified MFM

The DEC RX02 floppy disk drive uses a very strange format.  It is
similar to standard FM format, except that if the DAM on a sector is
0xf9 (deleted data) or 0xfd (data), there are twice as many data
bytes, and the sector data and sector CRC are in a variant of MFM.
The MFM encoding is modified to prevent the address mark detector from
sometimes firing inside the MFM data, since a standard MFM stream can
contain sequences that look just an an FM address mark.  (This
DEC-modified MFM should not be confused with MMFM, which is completely
different.)  Here I try to explain how this works.

The FM address marks used in this format look like the following when
decoded at MFM speed.  I've inserted one extra bit of FM-encoded 0
before each one.  This is guaranteed to be present in any standard FM
format -- the format actually requires 6 full bytes of FM-encoded 0
before each address mark -- and is needed by this scheme.  That is, we
assume the address mark detector looks for a 9-bit pattern starting
with the extra 0, not just for the 8-bit mark.  Here "d" marks the bit
positions for FM data, "c" for FM clock.

                        ..d...d...d...d...d...d...d...d...d.
                        c...c...c...c...c...c...c...c...c...
0fc iam  -> 8aa2a2a88 = 100010101010001010100010101010001000
0fe idam -> 8aa222aa8 = 100010101010001000100010101010101000
0f8 dam  -> 8aa222888 = 100010101010001000100010100010001000
0f9 dam  -> 8aa22288a = 100010101010001000100010100010001010
0fb dam  -> 8aa2228aa = 100010101010001000100010100010101010
0fd dam  -> 8aa222a8a = 100010101010001000100010101010001010


The pattern 1000101010100 occurs in each of these marks, with the
final 0 being one of the missing clock bits that characterize them as
marks.  Thus if we can forbid that pattern in the MFM data, the
problem is solved.  This MFM pattern can't be generated with the first
bit as a clock bit, as it has a missing clock when decoded in that
registration.  It could be generated with the first 1 as a data bit,
however, by the data stream 1011110.

Therefore we do the following transform.  Whenever the data stream
contains 011110, instead of encoding it in the normal way as:

   d d d d d d
  x00101010100

we encode it as:

   d d d d d d
  x01000100010

This MFM sequence is otherwise illegal (i.e., unused), since it has
missing clocks, so using it for this purpose does not create a
collision with the encoding of any other data sequence.

We could transform back by looking specifically for this sequence, or
we could apply the simpler rule that when we see a missing clock
between two 0 data bits, we change both data bits to 1's.  The simpler
rule will cause some illegal MFM sequences to be silently accepted,
which is perhaps a drawback.  It also leaves some apparent clock 
violations in the translated data that must be ignored, also a drawback.
However, the other rule doesn't seem to work on a test disk I have;
this might be due to the boundary conditions described next.

What do we do at the start and end of the data area?  Looking at a
sample disk, it appears that the encoding is done as if the data field
were preceded by 00 and followed by ff.  In other words, 11110 at the
start is specially encoded, but 01111 at the end (second CRC byte) is
not.  The following ff (or is it fe?) may actually be written as a
lead-out; I'm not sure.
*/

/* Window used to undo RX02 MFM transform */
#if 1
#define WINDOW 4   /* change aligned 1000 -> 0101 */
#else
#define WINDOW 12  /* change aligned x01000100010 -> x00101010100 */
#endif

/* Check whether the bit vector encodes an RX02 FM address mark.
   bitvec is sampled at twice normal FM rate, and we look at 9
   encoded bits of it (including a leading 0) */
int
rx02_mark(unsigned long long bitvec)
{
  switch (bitvec & 0xfffffffffULL) {
  case 0x8aa2a2a88ULL:  /* Index address mark, 0xfc with 0xd7 clock */
  case 0x8aa222aa8ULL:  /* ID address mark, 0xfe with 0xc7 clock */
  case 0x8aa222888ULL:  /* Data address mark, 0xf8 with 0xc7 clock */
  case 0x8aa22288aULL:  /* Data address mark, 0xf9 with 0xc7 clock */
  case 0x8aa2228a8ULL:  /* Data address mark, 0xfa with 0xc7 clock (unused) */
  case 0x8aa2228aaULL:  /* Data address mark, 0xfb with 0xc7 clock */
  case 0x8aa222a8aULL:  /* Data address mark, 0xfd with 0xc7 clock */
    return 1;
  }
  return 0;
}

void
rx02_bit(int bit)
{
  static int rx02cyl = 0;
  static int rx02dataenc = FM;
  unsigned char val = 0;
  int i;
  accum = (accum << 1) + bit;
  taccum = (taccum << 1) + bit;
  bits++;
  if (mark_after >= 0) mark_after--;

  if (bits < 32) return;

  /* Pre-detect address marks to achieve proper byte alignment */
  if (ibyte == -1 && rx02_mark(accum)) {
    if (bits < 64 && bits >= 48) {
      msg(OUT_HEX, "(+%d)", 64-bits);
      bits = 64; /* byte-align by repeating some bits */
    } else if (bits < 48 && bits > 32) {
      msg(OUT_HEX, "(-%d)", bits-32);
      bits = 32; /* byte-align by dropping some bits */
    }
    mark_after = 32;
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

  if (rx02dataenc == FM || dbyte == -1) {
    /* Try to decode accum in FM */
    if (bits < 64) return;

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
	/* Note bad clock pattern */
	msg(OUT_HEX, "?");
      }
    }

    for (i=0; i<8; i++) {
      val |= (accum & (1ULL << (4*i + 1 + 32))) >> (3*i + 1 + 32);
    }

    if (mark_after == 0) {
      switch (val) {
      case 0xfc:
	/* Index address mark, 0xfc with 0xd7 clock */
	check_missing_dam();
	msg(OUT_IDS, "\n#fc ");
	dmk_iam(0xfc, FM);
	bits = 32;
	ibyte = -1;
	dbyte = -1;
	break;

      case 0xfe:
	/* ID address mark, 0xfe with 0xc7 clock */
	if (dmk_awaiting_iam) break;
	check_missing_dam();
	msg(OUT_IDS, "\n#fe ");
	dmk_idam(0xfe, FM);
	bits = 32;
	crc = calc_crc1(0xffff, 0xfe);
	dbyte = -1;
	break;

      case 0xf8: /* Data address mark, 0xf8 with 0xc7 clock */
      case 0xfa: /* Data address mark, 0xfa with 0xc7 clock (not used) */
      case 0xfb: /* Data address mark, 0xfb with 0xc7 clock */
	if (dmk_awaiting_track_start()) break;
	msg(OUT_HEX, "\n");
	msg(OUT_IDS, "#%2x ", val);
	dmk_dam(val, FM);
	bits = 32;
	crc = calc_crc1(0xffff, val);
	ibyte = -1;
	dbyte = secsize(sizecode, FM) + 2;
	rx02dataenc = FM;
	break;

      case 0xf9: /* Data address mark, 0xf9 with 0xc7 clock */
      case 0xfd: /* Data address mark, 0xfd with 0xc7 clock */
	if (dmk_awaiting_track_start()) break;
	msg(OUT_HEX, "\n");
	msg(OUT_IDS, "#%2x ", val);
	dmk_dam(val, FM);
	bits = 32;
	crc = calc_crc1(0xffff, val);
	ibyte = -1;
	dbyte = 2*secsize(sizecode, FM) + 2;
	rx02dataenc = MFM;
	break;

      default:
	msg(OUT_QUIET, "[BUG]");
	break;
      }
    }

    if (bits < 64) return;

    switch (ibyte) {
    default:
      break;
    case 0:
      msg(OUT_IDS, "cyl=");
      rx02cyl = val;
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

  } else {

    /* Try to decode taccum in MFM */
    if (bits < 16 + 32) return;

#if 0
    /* This should not be needed since there can't be any write splices
       within a RX02-MFM area.  We should always be synchronized by the
       DAM; if we aren't, there was a bit error and resynching won't
       really help much.  So I've disabled it, because I don't want
       it to print "?" (or possibly even do an unwanted 1/2-bit shift)
       when the 4-bit window untransform leaves an apparent clock violation.
    */
    /* Check for valid clock bits */
    if (!mfm_valid_clock(taccum >> 32)) {
      if (mfm_valid_clock(taccum >> (32 - 1))) {
	/* Ignore oldest bit */
	/* Note: not clear this heuristic makes total sense, since the
	   DEC untransform of taccum could have been wrong if we were
	   shifted by one bit. */
	bits--;
	msg(OUT_HEX, "(-1)");
	if (bits < 16 + 32) return;
      } else {
	/* Note bad clock pattern */
	msg(OUT_HEX, "?");
      }
    }
#endif

    for (i=0; i<8; i++) {
      val |= (taccum & (1ULL << (2*i + 32))) >> (i + 32);
    }
  }

  if (ibyte >= 0 && ibyte <= 3) {
    msg(OUT_IDS, "%02x ", val);
  } else {
    msg(OUT_SAMPLES, "<");
    msg(OUT_HEX, "%02x", val);
    msg(OUT_SAMPLES, ">");
    msg(OUT_HEX, " ", val);
    msg(OUT_RAW, "%c", val);
  }

  dmk_data(val, (dbyte == -1 || rx02dataenc == FM) ? FM : MFM);

  if (ibyte >= 0) ibyte++;
  if (dbyte > 0) dbyte--;
  crc = calc_crc1(crc, val);

  if (dbyte == 0) {
    if (crc == 0) {
      msg(OUT_IDS, "[good data CRC] ");
      if (dmk_valid_id) {
	if (good_sectors == 0) first_encoding = RX02;
	good_sectors++;
	cylseen = rx02cyl;
      }
    } else {
      msg(OUT_ERRORS, "[bad data CRC] ");
      errcount++;
    }
    msg(OUT_HEX, "\n");
    dbyte = -1;
    dmk_valid_id = 0;
    write_splice = WRITE_SPLICE;
  }

  bits = 32;
}

void
rx02_decode(int sample)
{
  static float adj = 0.0;
  int len;

  msg(OUT_SAMPLES, "%d", sample);
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
  adj = (sample - (len/2.0 * mfmshort * cwclock)) * postcomp;

  msg(OUT_SAMPLES, "%c ", "--sml"[len]);

  rx02_bit(1);
  while (--len) rx02_bit(0);
}

/* Shove any valid bits out of the 32-bit address mark detection window */
void
rx02_flush()
{
  int i;
  for (i=0; i<32; i++) {
    rx02_bit(!(i&1));
  }
}

int
detect_encoding(int sample, int encoding)
{
  static float prob[3] = { 0.0, 0.0, 0.0 };
  static int confidence = 0;
# define DECAY 0.95
# define ENOUGH 32
  int sml, i;
  int guess = encoding;

  /* RX02 can't be detected automatically by this method */
  if (encoding == RX02) return RX02;

  /* Initialize for new track */
  if (sample == -1) {
    confidence = 0;
    return guess;
  }

  if (sample <= mfmthresh1) {
    /* if MFM, short; if FM, short */
    sml = 0;
  } else if (sample <= mfmthresh2) {
    /* if MFM, medium; if FM, probably invalid */
    sml = 1;
  } else {
    /* if MFM, long; if FM, long */
    sml = 2;
  }
  
  for (i=0; i<3; i++) {
    prob[i] = (prob[i] * DECAY) + ((i == sml) * (1.0 - DECAY));
  }

  if (ibyte == -1 && dbyte == -1) {
    /* We think we're in a gap */
    if (++confidence >= ENOUGH) {
      /* The 0x4e pattern in the early part of MFM gaps has 4
	 mediums, 2 shorts, and no longs.  The 0x00 pattern later
	 has all shorts, which doesn't help (looks like FM 0xff). */
      if (prob[1] > prob[0] + prob[2]) {
	guess = MFM;
      }
      /* The patterns in FM data are all longs and shorts.  The 0xff
	 pattern sometimes used in gaps is all shorts and looks like
	 MFM 0x00, which doesn't help.  The 0x00 pattern is all
	 longs, though.  If we're seeing some longs but roughly zero
	 mediums, guess that it's FM */
      if (prob[2] > 0.1 && prob[1] < 0.05) {
	guess = FM;
      }
      confidence = 0;
    }      
  } else {
    /* Not in a gap, data is not useful */
    confidence = 0;
  }

  return guess;
}

/* Main program */

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
set_kind()
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
   (If !hole, the latter count is meaningless.) */
void
do_histogram(int drive, int track, int side, int histogram[128],
	     int* total_cycles, int* total_samples, float* first_peak)
{
  unsigned char b;
  int i, tc, ts;
  float peak;
  int pwidth, psamps, psampsw;

  tc = 0;
  ts = 0;
  for (i=0; i<128; i++) {
    histogram[i] = 0;
  }
  catweasel_seek(&c.drives[drive], track);
  if (!catweasel_read(&c.drives[drive], side, 1, hole ? 0 : 200, 0)) {
    fprintf(stderr, "cw2dmk: Read error\n");
    cleanup();
    exit(1);
  }
  while ((b = catweasel_get_byte(&c)) != 0xff) {
    histogram[b]++;
    tc += b + 1;  /* not sure if the +1 is right */
    ts++;
  }

#if DEBUG2
  /* Print histogram for debugging */
  for (i=0; i<128; i+=8) {
    printf("%3d: %06d %06d %06d %06d %06d %06d %06d %06d\n",
	   i, histogram[i+0], histogram[i+1], histogram[i+2], histogram[i+3],
	   histogram[i+4], histogram[i+5], histogram[i+6], histogram[i+7]);
  }
#endif

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
  if (pwidth > 16) {
    /* Track is blank */
    peak = -1.0;
  } else {
    /* again not sure of +1.0 */
    peak = ((float) psampsw) / psamps + 1.0; 
  }

  *total_cycles = tc;
  *total_samples = ts;
  *first_peak = peak;
}

/* Guess the kind of drive and media in use */
void
detect_kind(int drive)
{
  int histogram[128];
  int total_cycles, total_samples;
  float peak, rpm, dclock;

  do_histogram(drive, 0, 0, histogram, &total_cycles, &total_samples, &peak);

  if (peak < 0.0) {
    /* Track is blank */
    fprintf(stderr, "cw2dmk: Track 0 is unformatted\n");
    cleanup();
    exit(1);
  } else {
    dclock = 7080.5 / peak;
  }

  /* Total cycles gives us the RPM */
  rpm = 7080500.0 / ((float)total_cycles) * 60.0;

#if DEBUG2
  printf("data clock approx %f kHz\n", dclock);
  printf("drive speed approx %f RPM\n", rpm);
#endif

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
    fprintf(stderr, "data clock approx %f kHz\n", dclock);
    fprintf(stderr, "drive speed approx %f RPM\n", rpm);
    cleanup();
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

  do_histogram(drive, 0, 1, histogram, &total_cycles, &total_samples, &peak);

  if (peak > 0.0) {
    float dclock = 7080.5 / peak;
    if (dclock > 225.0 && dclock < 575.0) {
      res = 2;
    }
  }
  msg(OUT_QUIET + 1, "Detected %d side%s formatted\n", res, plu(res));
  fflush(stdout);
  return res;
}

/* Command-line parameters */
int port = 0;
int tracks = -1;
int sides = -1;
int steps = -1;
int drive = -1;
int uencoding = MIXED;
int retries = 4;
int alternate = 0;

void usage()
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
  printf(" -e encoding   1 = FM (SD), 2 = MFM (DD or HD), 3 = RX02\n");
  printf(" -w fmtimes    Write FM bytes 1 or 2 times [%d]\n", fmtimes);
  printf("\n Special options for hard to read diskettes\n");
  printf(" -x retries    Number of retries on errors [%d]\n", retries);
  printf(" -a alternate  Alternate even/odd tracks on retries with -m2 [%d]\n",
	 alternate);
  printf("               0 = always even\n");
  printf("               1 = always odd\n");
  printf("               2 = even, then odd\n");
  printf("               3 = odd, then even\n");
  printf(" -o postcomp   Amount of read-postcompensation (0.0-1.0) [%.2f]\n",
	 postcomp);
  printf(" -h hole       Track start: 1 = index hole, 0 = anywhere [%d]\n",
	 hole);
  printf(" -g ign        Ignore first ign bytes of track [%d]\n", dmk_ignore);
  printf(" -i ipos       Force IAM to ipos from track start; "
	 "if -1, don't [%d]\n", dmk_iam_pos);
  printf(" -z maxsize    Allow sector sizes up to 128<<maxsize [%d]\n",
	 maxsize);
  printf("\n Fine-tuning options; effective only after the -k option\n");
  printf(" -c clock      Catweasel clock multipler [%d]\n", cwclock);
  printf(" -f threshold  FM threshold for short vs. long [%d]\n", fmthresh);
  printf(" -1 threshold  MFM threshold for short vs. medium [%d]\n",
	 mfmthresh1);
  printf(" -2 threshold  MFM threshold for medium vs. long [%d]\n",
	 mfmthresh2);
  printf(" -l bytes      DMK track length in bytes [%d]\n", dmktracklen);
  printf("\n");
  exit(1);
}

int
main(int argc, char** argv)
{
  int ch, track, side, curenc, headpos, readtime, i;
  int guess_sides = 0, guess_steps = 0, guess_tracks = 0;
  int cw_mk = 1;

  opterr = 0;
  for (;;) {
    ch = getopt(argc, argv, "p:d:v:u:k:m:t:s:e:w:x:a:o:h:g:i:z:c:f:1:2:l:");
    if (ch == -1) break;
    switch (ch) {
    case 'p':
      port = strtol(optarg, NULL, 16);
      if (port < 0 || (port >= MK3_MAX_CARDS && port < MK1_MIN_PORT) ||
	  (port > MK1_MAX_PORT)) {
	fprintf(stderr,
		"cw2dmk: -p must be between %d and %d for MK3/4 cards,\n"
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
    case 'c':
      cwclock = strtol(optarg, NULL, 0);
      if (kind == -1 || (cwclock != 1 && cwclock != 2 && cwclock != 4)) {
	usage();
      }
      break;
    case 'f':
      fmthresh = strtol(optarg, NULL, 0);
      if (kind == -1) usage();
      break;
    case '1':
      mfmthresh1 = strtol(optarg, NULL, 0);
      if (kind == -1) usage();
      break;
    case '2':
      mfmthresh2 = strtol(optarg, NULL, 0);
      if (kind == -1) usage();
      break;
    case 'l':
      dmktracklen = strtol(optarg, NULL, 0);
      if (dmktracklen < 0 || dmktracklen > 0x4000) usage();
      if (kind == -1) usage();
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

  /*
   * We can't detect autodetect kind if there is no index hole.
   * ToDo: Try to autodetect anyway, in case there is a hole but the
   * user gave -h0 because he didn't want us to try to align with it.
   * Complain only if detect_kind times out due to not finding a hole.
   */
  if (hole == 0 && kind == -1) {
    fprintf(stderr, "cw2dmk: If -h0 is given, -k is required too\n");
    exit(1);
  }

  /* Keep drive from spinning endlessly on (expected) signals */
  signal(SIGHUP, handler);
  signal(SIGINT, handler);
  signal(SIGQUIT, handler);
  signal(SIGPIPE, handler);
  signal(SIGTERM, handler);

#if linux
  if (geteuid() != 0) {
    fprintf(stderr, "cw2dmk: Must be setuid to root or be run as root\n");
    return 1;
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
    return 1;
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

  /* Log the command line and version number */
  msg(OUT_ERRORS, "Version: %s  Command line: ", VERSION);
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
    return 1;
  }
  if (cw_mk == 1 && cwclock == 4) {
    fprintf(stderr, "cw2dmk: Catweasel MK1 does not support 4x clock\n");
    cleanup();
    return 1;
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
      cleanup();
      return 1;
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
    cleanup();
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
  good_tracks = 0;
  err_tracks = 0;
  first_encoding = curenc = uencoding;

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

  /* Loop over tracks */
  for (track=0; track<tracks; track++) {
    prevcylseen = cylseen;
    headpos = track * steps + ((steps == 2) ? (alternate & 1) : 0);

    /* Loop over sides */
    for (side=0; side<sides; side++) {
      int retry = 0;

      /* Loop over retries */
      do {
	unsigned char b = 0, oldb;
#if DEBUG3
	int histogram[128], i;
	for (i=0; i<128; i++) histogram[i] = 0;
#endif
	if (retry) {
	  msg(OUT_TSUMMARY, "[retry %d] ", retry);
	} else {
	  msg(OUT_TSUMMARY, "Track %d, side %d: ", track, side);
	}
	fflush(stdout);

	/* Seek to correct track */
	if ((steps == 2) && (retry > 0) && (alternate & 2)) {
	  headpos ^= 1;
	}
	catweasel_seek(&c.drives[drive], headpos);
	curenc = detect_encoding(-1, first_encoding); /* initialize detector */
	dmk_init_track();
	decode_init();
#if DEBUG5
	if (c.mk == 1) {
	  catweasel_fillmem(&c, DEBUG5_BYTE);
	}
#endif

	/* Do read */
	if (hole && readtime > 0 && !catweasel_await_index(&c.drives[drive])) {
	  fprintf(stderr, "cw2dmk: No index hole detected\n");
	  cleanup();
	  return 1;
	}
	if (!catweasel_read(&c.drives[drive], side, cwclock,
			    readtime, (readtime != 0))) {
	  fprintf(stderr, "cw2dmk: Read error\n");
	  cleanup();
	  return 1;
	}

	/* Loop over samples */
	oldb = 0;
	index_edge = 0;
	while (!dmk_full) {
	  b = catweasel_get_byte(&c);
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
	   * Index hole edge check.  Also counts the artificial end
	   * mark that catweasel_read appends to the data.
	   */ 
	  if ((oldb ^ b) & 0x80) {
	    index_edge++;
	    if (index_edge > 4) {
	      /*
	       * Unless we have hard-sectored media or have read 3
	       * revolutions, index_edge > 4 must mean we've hit the
	       * end mark.  XXX Would be better to positively detect
	       * the end mark rather than conflating it with index edges.
	       */
	      msg(OUT_HEX, "[end of data] ");
	      break;
	    }
	    if (hole) {
	      msg(OUT_HEX, (b & 0x80) ? "{" : "}");
	    }
	  }
	  oldb = b;
	  b &= 0x7f;
#if DEBUG3
	  histogram[b]++;
#endif

	  /* Process this sample */
	  if (uencoding == MIXED) {
	    int newenc = detect_encoding(b, curenc);
	    if (newenc != curenc) {
	      check_missing_dam();
	      msg(OUT_IDS, "\n");
	      msg(OUT_ERRORS, "[%s->%s] ", enc_name[curenc], enc_name[newenc]);
	      if (!(curenc == MIXED && newenc == FM)) decode_init();
	      curenc = newenc;
	    }
	  }
	  if (curenc == FM || curenc == MIXED) {
	    int newenc = fm_decode(b);
	    if (uencoding == MIXED && newenc == RX02 &&
		track < 2 && side == 0) {
	      msg(OUT_IDS, "\n");
	      msg(OUT_QUIET + 1, "[apparently RX02 encoding; restarting]\n");
	      uencoding = RX02; /* slight kludge */
	      if (guess_steps) {
		steps = 1;
		if (guess_tracks) tracks = TRACKS_GUESS / steps;
	      }
	      goto restart;
	    }
	  } else if (curenc == MFM) {
	    mfm_decode(b);
	  } else {
	    rx02_decode(b);
	  }
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
	if (curenc == FM || curenc == MIXED) {
	  fm_flush();
	} else if (curenc == MFM) {
	  mfm_flush();
	} else {
	  rx02_flush(b);
	}
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
      } while (++retry <= retries && errcount > 0);
      total_retries += retry - 1;
      fflush(stdout);
      dmk_write();
    }
  }
 done:

  cleanup();
  msg(OUT_SUMMARY, "\nTotals:\n");
  msg(OUT_SUMMARY, "%d good track%s, %d good sector%s\n",
      good_tracks, plu(good_tracks),
      total_good_sectors, plu(total_good_sectors));
  msg(OUT_SUMMARY, "%d bad track%s, %d unrecovered error%s, %d retr%s\n",
      err_tracks, plu(err_tracks), total_errcount, plu(total_errcount),
      total_retries, (total_retries == 1) ? "y" : "ies");
  if (flippy) {
    msg(OUT_SUMMARY, "Possibly a flippy disk; check reverse side too\n");
  }
  return 0;
}
