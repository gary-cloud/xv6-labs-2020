struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
#ifdef LAB_LOCK
  uint ticks;
#else
  struct buf *prev; // LRU cache list
#endif
  struct buf *next;
  uchar data[BSIZE];
};

