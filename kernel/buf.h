struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  uint lastuse; // 用于跟踪缓冲区buffer上次使用的时间，以便应用LRU策略
  struct buf *next;
  uchar data[BSIZE];
};

