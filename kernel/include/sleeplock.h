#ifndef __SLEEPLOCK_H
#define __SLEEPLOCK_H

#include "types.h"
#include "spinlock.h"

struct spinlock;

struct sleeplock
{
    uint locked;        // 是否上锁
    struct spinlock lk; // 保护睡眠锁的自旋锁
    char *name;         // 睡眠锁的名称
    int pid;            // 持有锁的PID
};

void acquiresleep(struct sleeplock *);
void releasesleep(struct sleeplock *);
int holdingsleep(struct sleeplock *);
void initsleeplock(struct sleeplock *, char *);

#endif
