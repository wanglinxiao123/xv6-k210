// Timer Interrupt handler

#include "include/types.h"
#include "include/param.h"
#include "include/riscv.h"
#include "include/sbi.h"
#include "include/spinlock.h"
#include "include/timer.h"
#include "include/printf.h"
#include "include/proc.h"

struct spinlock tickslock; // 用于保护变量 ticks
uint ticks;

// 初始化保护变量 ticks 的自旋锁
void timerinit()
{
    initlock(&tickslock, "time");
}

// 设置下次的定时时间
void set_next_timeout()
{
    sbi_set_timer(r_time() + INTERVAL);
}

// ticks++，设置下次定时时间
void timer_tick()
{
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
    set_next_timeout();
}
