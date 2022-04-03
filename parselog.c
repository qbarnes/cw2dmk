/*
 * parselog.c: Parse cw2dmk log level 7 to recover Catweasel samples.
 * Copyright (C) 2022 Timothy Mann
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

static int unread_track = -1, unread_side, unread_pass;
static int prev_track = -1;
static int hibit;

/*
 * XXX Currently parses only the physical track/side/pass messages and
 * the samples.  Everything else is skipped.  Could be useful to parse
 * more.  Ideas:
 *
 * Parse the command line and the "Trying" messages to get a default
 * value for disk kind (-k), since that probably was correctly set to
 * begin with.
 *
 * Parse the command line for the Catweasel clock rate (-c), in
 * case the user specified that differently from the default for this
 * disk kind.
 */

/*
 * Find start of next unread track capture in file.  If called
 * repeatedly without calling parse_sample, returns the same values
 * again.  Return physical track number, or EOF if no more track
 * captures in file.  Return physical side and pass numbers in *side
 * and *pass.
 */
int
parse_track(FILE *log_file, int *side, int *pass)
{
  int ret;

  hibit = 0;

  if (unread_track == -1) {
    for (;;) {
      // Try to read track start message here.
      ret = fscanf(log_file, "Track %d, side %d, pass %d:",
                   &unread_track, &unread_side, &unread_pass);
      if (ret == EOF) {
        *side = *pass = 0;
        return EOF;
      }
      if (ret == 3) {
        break; // success
      }
      if (ret == 2) {
        // Log from an old cw2dmk version without pass numbers
        unread_pass = 1;
        break; // success
      }

      // Try to read old-style (original 4.4 and earlier) retry message.
      ret = fscanf(log_file, "; retry %d]", &unread_pass);
      if (ret == 1) {
        unread_pass++;
        unread_track = prev_track;
        break;
      }
      (void) fgetc(log_file);
    }
    prev_track = unread_track;
  }

  *side = unread_side;
  *pass = unread_pass;
  return unread_track;
}

/*
 * Return next sample from current track capture, or EOF if no more
 * samples in track capture.  Mark current capture as read.
 */
int
parse_sample(FILE *log_file)
{
  int ret, c, sample;
  char junk[2];

  unread_track = -1;

  for (;;) {
    // Try to read a sample here.
    ret = fscanf(log_file, " %d%1[sml]%*[ ]", &sample, &junk[0]);
    if (ret == EOF) {
      return EOF;
    }
    if (ret == 2) {
      return sample | hibit;
    }

    // No sample here; what is it?
    c = fgetc(log_file);
    switch (c) {
    case EOF:
      // End of file
      return EOF;
    case 'T':
      // Looks like start of next track; push back 'T' for parse_track to read.
      ungetc(c, log_file);
      return EOF;
    case '<':
      // Decoded byte; skip through closing '>'.
      do {
        c = fgetc(log_file);
      } while (c != EOF && c != '>');
      break;
    case '(':
      // Number of bits added/dropped; skip through closing ')'.
      do {
        c = fgetc(log_file);
      } while (c != EOF && c != ')');
      break;
    case '[':
      // Informational message; usually just skip through closing ']'.
      do {
        c = fgetc(log_file);
        if (c == ';') {
          // Assume old-style retry message starts here.
          ungetc(c, log_file);
          return EOF;
        }
      } while (c != EOF && c != ']');
      break;
    case '{':
      // Leading index edge; turn on high bit in upcoming sample(s).
      /*
       * Note: due to a bug in logging (XXX to be fixed later), this may
       * also mean the next sample will be the end of track marker,
       * which was originally 0x80 but logged as 0x00.  Either way,
       * setting the high bit will reconstitute the original sample.
       */
      hibit = 0x80;
      break;
    case '}':
      // Trailing index edge; turn off high bit in upcoming sample(s).
      hibit = 0;
      break;
    case '?':
      // Decoder saw missing/extra clock; skip.
      break;
    default:
      // Anything else; skip.
      break;
    }
  }
}

#if TEST
int
main(int argc, char **argv)
{
  int track, side, pass, sample;
  FILE *log_file;

  if (argc != 2) {
    fprintf(stderr, "usage: %s logfile\n", argv[0]);
    return 2;
  }
  log_file = fopen(argv[1], "r");
  if (log_file == NULL) {
    perror(argv[1]);
    return 1;
  }
  do {
    track = parse_track(log_file, &side, &pass);
    printf("=== track %d, side %d, pass %d ===\n", track, side, pass);
    do {
      sample = parse_sample(log_file);
      printf("%d ", sample);
    } while (sample != EOF);
    printf("\n");
  } while (track != EOF);
  return 0;
}
#endif
