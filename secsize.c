/*
 * cw2dmk: Dump floppy disk from Catweasel to .dmk format.
 * Copyright (C) 2000,2022 Timothy Mann
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

int
secsize(int sizecode, int encoding, int maxsize, unsigned int quirk)
{
  int size;

  switch (encoding) {
  case MFM:
    /* 179x can only do sizes 128, 256, 512, 1024, and ignores
       higher-order bits.  If you need to read a 765-formatted disk
       with larger sectors, change maxsize with the -z
       command line option. */
    size = 128 << (sizecode % (maxsize + 1));
    break;

  case FM:
  default:
    /* WD1771 has two different encodings for sector size, depending on
       a bit in the read/write command that is not recorded on disk.
       We guess IBM encoding if the size is <= maxsize, non-IBM
       if larger.  This doesn't really matter for demodulating the
       data bytes, only for checking the CRC.  */
    if (sizecode <= maxsize) {
      /* IBM */
      size = 128 << sizecode;
    } else {
      /* non-IBM */
      size = 16 * (sizecode ? sizecode : 256);
    }
    break;

  case RX02:
    size = 256 << (sizecode % (maxsize + 1));
    break;
  }

  if (quirk & QUIRK_EXTRA_DATA) {
    size += 4;
  }
  return size;
}
