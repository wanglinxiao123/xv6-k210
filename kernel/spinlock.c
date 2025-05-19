// Mutual exclusion spin locks.

#include "include/types.h"
#include "include/param.h"
#include "include/memlayout.h"
#include "include/spinlock.h"
#include "include/riscv.h"
#include "include/proc.h"
#include "include/intr.h"
#include "include/printf.h"

// 初始化 spinlock
void initlock(struct spinlock *lk, char *name)
{
    lk->name = name;
    lk->locked = 0;
    lk->cpu = 0;
}

// 尝试获取自旋锁
void acquire(struct spinlock *lk)
{
    push_off();
    if (holding(lk))
        panic("acquire");

    while (__sync_lock_test_and_set(&lk->locked, 1) != 0)
        ;
    // 确保之前的操作已经完成，之后的操作不能被重新排序
    __sync_synchronize();
    lk->cpu = mycpu();
}

// 释放自旋锁
void release(struct spinlock *lk)
{
    if (!holding(lk))
        panic("release");

    lk->cpu = 0;
    __sync_synchronize();
    __sync_lock_release(&lk->locked);
    pop_off();
}

// 检查当前 CPU 是否拥有锁 lk
int holding(struct spinlock *lk)
{
    int r;
    r = (lk->locked && lk->cpu == mycpu());
    return r;
}
