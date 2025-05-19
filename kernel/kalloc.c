// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "include/types.h"
#include "include/param.h"
#include "include/memlayout.h"
#include "include/riscv.h"
#include "include/spinlock.h"
#include "include/kalloc.h"
#include "include/string.h"
#include "include/printf.h"

void freerange(void *pa_start, void *pa_end);

extern char kernel_end[]; // first address after kernel.

struct run
{
    struct run *next;
};

struct
{
    struct spinlock lock;
    struct run *freelist; // 内存链表头
    uint64 npage;         // 空余的页面数
} kmem;

// 初始化自旋锁，将空余空间回收到链表
void kinit()
{
    initlock(&kmem.lock, "kmem");
    kmem.freelist = 0;
    kmem.npage = 0;
    freerange(kernel_end, (void *)PHYSTOP);
}

// 回收 (pa_start, pa_end) 对应的物理页面
void freerange(void *pa_start, void *pa_end)
{
    char *p;
    p = (char *)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
        kfree(p);
}

// 回收 pa 对应的物理页面
void kfree(void *pa)
{
    struct run *r;

    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < kernel_end || (uint64)pa >= PHYSTOP)
    {
        panic("kfree");
    }
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    acquire(&kmem.lock);

    r->next = kmem.freelist;
    kmem.freelist = r;
    kmem.npage++;

    release(&kmem.lock);
}

// 从空闲链表头中获取 4KB，返回物理地址
void *kalloc(void)
{
    struct run *r;

    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r)
    {
        kmem.freelist = r->next;
        kmem.npage--;
    }
    release(&kmem.lock);

    if (r)
    {
        memset((char *)r, 5, PGSIZE);
    }

    return (void *)r;
}

// 统计空闲物理内存总数
uint64 freemem_amount(void)
{
    return kmem.npage << PGSHIFT;
}
