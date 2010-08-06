/* Some constants for DMK format */
#define DMK_WRITEPROT     0
#define DMK_NTRACKS       1
#define DMK_TRACKLEN      2
#define DMK_TRACKLEN_SIZE 2
#define DMK_OPTIONS       4
#define DMK_FORMAT        0x0c
#define DMK_FORMAT_SIZE   4
#define DMK_HDR_SIZE      0x10
#define DMK_TKHDR_SIZE    0x80    /* Space reserved for IDAM pointers */

/* Conventional track lengths used by DMK's emulator, plus one for
 * 3.5" HD.  These are okay for images formatted by the emulator, but
 * they are a little too small for cw2dmk when reading a real disk,
 * because the real disk may have been written in a drive whose motor
 * was 1% slow or so.  The "nominal" value in the comments is the
 * ideal track length assuming that the drive motor and the data clock
 * are both exactly at their specified speeds.
 */
#define DMK_TRACKLEN_5SD  0x0cc0 /* DMK_TKHDR_SIZE + 3136 (nominal 3125) */
#define DMK_TRACKLEN_5    0x1900 /* DMK_TKHDR_SIZE + 6272 (nominal 6250) */
#define DMK_TRACKLEN_8SD  0x14e0 /* DMK_TKHDR_SIZE + 5216 (nominal 5208) */
#define DMK_TRACKLEN_8    0x2940 /* DMK_TKHDR_SIZE + 10432 (nominal 10416) */
#define DMK_TRACKLEN_3HD  0x3180 /* DMK_TKHDR_SIZE + 12544 (nominal 12500) */

/* Track lengths for cw2dmk reads.  Allows for 2% slower than nominal
 * drive.  The values are rounded up to a multiple of 32 as the DMK
 * values were (just for paranoia's sake, in case someone was
 * depending on that).  The user will still have to specify a longer
 * track explicitly to read Atari 800 disks made in a 288 RPM (4% slow) drive.
 */
#define CW_TRACKLEN_5SD  0x0d00 /* DMK_TKHDR_SIZE + 3200 (nominal 3125) */
#define CW_TRACKLEN_5    0x1980 /* DMK_TKHDR_SIZE + 6400 (nominal 6250) */
#define CW_TRACKLEN_8SD  0x1560 /* DMK_TKHDR_SIZE + 5344 (nominal 5208) */
#define CW_TRACKLEN_8    0x2A40 /* DMK_TKHDR_SIZE + 10688 (nominal 10416) */
#define CW_TRACKLEN_3HD  0x3260 /* DMK_TKHDR_SIZE + 12768 (nominal 12500) */

/* Bit assignments in options */
#define DMK_SSIDE_OPT     0x10
#define DMK_SDEN_OPT      0x40
#define DMK_IGNDEN_OPT    0x80
#define DMK_RX02_OPT      0x20    /* DMK extension, set if -e3 */

/* Bit assignments in IDAM pointers */
#define DMK_DDEN_FLAG     0x8000
#define DMK_EXTRA_FLAG    0x4000  /* unused */
#define DMK_IDAMP_BITS    0x3fff

typedef struct {
  unsigned char writeprot;
  unsigned char ntracks;
  unsigned short tracklen;
  unsigned char options;
  unsigned char padding[7];
  unsigned long mbz;
} dmk_header_t;
