#include "include/types.h"
#include "include/param.h"
#include "include/proc.h"
#include "include/intr.h"
#include "include/printf.h"

// 关闭中断
void push_off(void)
{
    int old = intr_get();

    intr_off();
    if (mycpu()->noff == 0)
        mycpu()->intena = old;
    mycpu()->noff += 1;
}

// 打开中断
void pop_off(void)
{
    struct cpu *c = mycpu();

    if (intr_get())
        panic("pop_off - interruptible");

    if (c->noff < 1)
    {
        panic("pop_off");
    }

    c->noff -= 1;
    if (c->noff == 0 && c->intena)
    {
        intr_on();
    }
}