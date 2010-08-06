/* Some constants for JV3 format */

/* Flags */
#define JV3_DENSITY     0x80  /* 1=dden, 0=sden */
#define JV3_DAM         0x60  /* data address mark; values follow */
#define JV3_DAMSDFB     0x00
#define JV3_DAMSDFA     0x20
#define JV3_DAMSDF9     0x40
#define JV3_DAMSDF8     0x60
#define JV3_DAMDDFB     0x00
#define JV3_DAMDDF8     0x20
#define JV3_SIDE        0x10  /* 0=side 0, 1=side 1 */
#define JV3_ERROR       0x08  /* 0=ok, 1=CRC error */
#define JV3_NONIBM      0x04  /* 0=normal, 1=short (for VTOS 3.0, xtrs only) */
#define JV3_SIZE        0x03  /* in used sectors: 0=256,1=128,2=1024,3=512
				 in free sectors: 0=512,1=1024,2=128,3=256 */
#define JV3_FREE        0xff  /* in track/sector fields */
#define JV3_FREEF       0xfc  /* in flags field, or'd with size code */

#define JV3_SECSTART   (34*256) /* start of sectors within file */
#define JV3_SECSPERBLK ((int)(JV3_SECSTART/3))
#define JV3_SECSMAX    (2*JV3_SECSPERBLK)
#define JV3_TRACKSMAX  255

/* Max bytes per unformatted track. */
#define TRKSIZE_SD    3125  /* 250kHz / 5 Hz [300rpm] / (2 * 8) */
                         /* or 300kHz / 6 Hz [360rpm] / (2 * 8) */
#define TRKSIZE_DD    6250  /* 250kHz / 5 Hz [300rpm] / 8 */
                         /* or 300kHz / 6 Hz [360rpm] / 8 */
#define TRKSIZE_8SD   5208  /* 500kHz / 6 Hz [360rpm] / (2 * 8) */
#define TRKSIZE_8DD  10416  /* 500kHz / 6 Hz [360rpm] / 8 */
#define TRKSIZE_5HD  10416  /* 500kHz / 6 Hz [360rpm] / 8 */
#define TRKSIZE_3HD  12500  /* 500kHz / 5 Hz [300rpm] / 8 */


