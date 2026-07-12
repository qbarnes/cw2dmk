# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Tim Mann's Catweasel floppy read/write tools: C programs (GNU99) that use
Catweasel MK1/MK3/MK4 universal floppy controller hardware to read and write
floppy disk images in the DMK format (used by TRS-80 emulators), plus
hardware-independent DMK/JV1/JV3 image converters. Builds for both Linux and
MS-DOS (via DJGPP cross-compilation).

## Build Commands

```sh
make                # build all programs and man-page .txt files
make progs          # build just the binaries
make manpages       # regenerate .txt docs from .man (nroff) sources
make clean          # remove binaries and objects
make veryclean      # also remove generated .txt, firmware.h, tarballs
make release        # build source + binary release tarballs
```

Linux build dependencies: `gcc`, `make`, `pciutils-devel`/`libpci-dev`,
`groff-base`, `util-linux` (hexdump for generating firmware.h). See
BUILDING.md.

Cross-build for MS-DOS:
```sh
make TARGET_OS=MSDOS CC=/usr/local/djgpp/bin/i586-pc-msdosdjgpp-gcc
```

There is no test suite. Two files have self-test mains built with `-DTEST`:
```sh
make crc            # standalone CRC test program from crc.c
make parselog       # standalone log-parser test program from parselog.c
```

The `cwsdpmi` directory is a git submodule (clone with `--recursive`, over
ssh). `firmware.h` is generated from `firmware/rel2f2.cw4` by hexdump.

## Programs and Architecture

Each program is a single .c file linked against shared objects; there is no
library layer beyond that.

**Hardware-dependent tools** (link `catweasl.o` + `cwpci.o`, need `-lpci` on
Linux):
- `cw2dmk` — reads a real floppy via Catweasel into a DMK image. The largest
  and most complex program; contains the unified FM/MFM/RX02 decoder.
- `dmk2cw` — writes a DMK image back to a real floppy.
- `cwhist` — displays a histogram of raw Catweasel flux samples for a track
  (useful for diagnosing media/drive issues).

**Hardware-independent converters** (no Catweasel needed):
- `dmk2jv3` — converts DMK images to JV3 format.
- `jv2dmk` — converts JV1/JV3 images to DMK.

**Shared code:**
- `catweasl.c` / `cwfloppy.h` — low-level Catweasel controller driver
  (originally from Michael Krause's cwfloppy driver), handles MK1/MK3/MK4
  differences and firmware loading (MK4 firmware embedded via generated
  `firmware.h`).
- `cwpci.c` — PCI detection of the Catweasel card (libpci on Linux, direct
  port I/O on MS-DOS).
- `crc.c` — CRC-16-CCITT, `#include`d directly into the tools rather than
  linked.
- `secsize.c` — sector-size decoding per controller quirk, also `#include`d.
- `parselog.c` — re-parses cw2dmk log-level-7 output to recover raw
  Catweasel samples, enabling decoder reruns without hardware.
- `dmk.h`, `jv3.h` — disk image format constants.
- `kind.h` — table of disk "kinds" (drive RPM / data rate combinations) with
  Catweasel clock and FM/MFM threshold parameters per kind.

**Key design document:** `decoder.txt` explains cw2dmk's unified decoder —
it translates the flux sample stream into bits at the MFM rate through a
64-bit shift register (`accum`), scans simultaneously for FM, MFM, and RX02
address marks, and switches encodings instantly when a mark is recognized.
Read this before touching decoder logic in cw2dmk.c.

## Conventions

- MS-DOS remains a first-class target: keep code DJGPP-compatible and guard
  OS-specific code with `#if __DJGPP__` / Linux alternatives (see catweasl.c
  and cwpci.c for the pattern).
- Version string lives in `version.h` and is extracted by the Makefile for
  release tarball names.
- Documentation for each tool is in its `.man` page; the `.txt` files are
  generated from them — edit the `.man` source, not the `.txt`.
