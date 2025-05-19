#ifndef __BUF_H
#define __BUF_H

#define BSIZE 512

#include "sleeplock.h"

struct buf
{
    int valid;             // 是否有数据从磁盘读入
    int disk;              // 是否有磁盘占有该 buf
    uint dev;              // 磁盘设备号
    uint sectorno;         // 要读/写的磁盘扇区号
    struct sleeplock lock; // 睡眠锁
    uint refcnt;           // 引用计数
    struct buf *prev;
    struct buf *next;
    uchar data[BSIZE]; // 数据缓冲区
};

void binit(void);
struct buf *bread(uint, uint);
void brelse(struct buf *);
void bwrite(struct buf *);

#endif
