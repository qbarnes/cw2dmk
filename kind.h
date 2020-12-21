typedef struct {
  const char* description;
  int cwclock;
  int fmthresh;   /* <= this value: short */
  int mfmthresh1; /* <= this value: short */
  int mfmthresh2; /* <= this value: short or medium */
  int dmktracklen;/* conventional DMK track length for this kind */
  int cwtracklen; /* longer DMK track length for this kind, for cw reads */
  float mfmshort; /* nominal MFM short assuming 1x clock multiplier */
  int hd;         /* value for HD line when writing */
  int readtime;   /* read time in ms assuming RPM and clock are precise */
} kind_desc;

/* Note: kind numbers are used in usage message and on command line */
#define NKINDS 4
kind_desc kinds[NKINDS] = {
 
  /* 360 RPM, MFM data 300 kHz */
  { "5.25\" SD/DD disk in 1.2MB drive",
    2 /*14.161MHz*/, 70, 56, 79,
    DMK_TRACKLEN_5, CW_TRACKLEN_5, 23.600, 0, 167 },

  /* 300 RPM, MFM data 250 kHz */
  { "5.25\" SD/DD disk in 360KB/720KB drive, or 3.5\" SD/DD disk",
    2 /*14.161MHz*/, 84, 70, 92,
    DMK_TRACKLEN_5, CW_TRACKLEN_5, 28.322, 0, 200 },

  /* 360 RPM, MFM data 500 kHz */
  { "5.25\" HD disk, or 8\" SD/DD disk",
    2 /*14.161MHz*/, 42, 35, 46,
    DMK_TRACKLEN_8, CW_TRACKLEN_8, 14.161, 1, 167 },

  /* 300 RPM, MFM data 500 kHz */
  { "3.5\" HD disk", 
    2 /*14.161MHz*/, 42, 35, 46,
    DMK_TRACKLEN_3HD, CW_TRACKLEN_3HD, 14.161, 1, 200 },
};
