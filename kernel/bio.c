#include "include/types.h"
#include "include/param.h"
#include "include/spinlock.h"
#include "include/sleeplock.h"
#include "include/riscv.h"
#include "include/buf.h"
#include "include/sdcard.h"
#include "include/printf.h"
#include "include/disk.h"

struct
{
    struct spinlock lock;
    struct buf buf[NBUF];
    struct buf head; // 磁盘缓冲块头
} bcache;

// 构建双向环形链表，初始化每一个buf的睡眠锁
void binit(void)
{
    struct buf *b;
    initlock(&bcache.lock, "bcache");

    bcache.head.prev = &bcache.head;
    bcache.head.next = &bcache.head;

    for (b = bcache.buf; b < bcache.buf + NBUF; b++)
    {
        b->refcnt = 0;
        b->sectorno = ~0;
        b->dev = ~0;
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        initsleeplock(&b->lock, "buffer");
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }
}

// 如果扇区已经被缓存，就直接返回
// 如果没有被缓存，就通过 LRU 选择一个缓存块返回
static struct buf *bget(uint dev, uint sectorno)
{
    struct buf *b;

    acquire(&bcache.lock);

    // 如果扇区已经被缓存，就直接返回
    for (b = bcache.head.next; b != &bcache.head; b = b->next)
    {
        if (b->dev == dev && b->sectorno == sectorno)
        {
            b->refcnt++;
            release(&bcache.lock);
            acquiresleep(&b->lock);
            return b;
        }
    }

    // 如果没有被缓存，就通过 LRU 选择一个缓存块返回
    for (b = bcache.head.prev; b != &bcache.head; b = b->prev)
    {
        if (b->refcnt == 0)
        {
            b->dev = dev;
            b->sectorno = sectorno;
            b->valid = 0;
            b->refcnt = 1;
            release(&bcache.lock);
            acquiresleep(&b->lock);
            return b;
        }
    }
    panic("bget: no buffers");
}

// 先对扇区进行缓存，再将扇区数据读取到缓存中
struct buf *bread(uint dev, uint sectorno)
{
    struct buf *b;

    b = bget(dev, sectorno);
    if (!b->valid)
    {
        disk_read(b);
        b->valid = 1;
    }

    return b;
}

// 将缓存 b 的数据写入磁盘
void bwrite(struct buf *b)
{
    if (!holdingsleep(&b->lock))
    {
        panic("bwrite");
    }

    disk_write(b);
}

// 引用计数--，如果为0，则回收缓存块
void brelse(struct buf *b)
{
    if (!holdingsleep(&b->lock)){
        panic("brelse");
    }
    releasesleep(&b->lock);

    acquire(&bcache.lock);

    b->refcnt--;
    if (b->refcnt == 0)
    {
        b->next->prev = b->prev;
        b->prev->next = b->next;
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }

    release(&bcache.lock);
}

// 引用计数++
void bpin(struct buf *b)
{
    acquire(&bcache.lock);
    b->refcnt++;
    release(&bcache.lock);
}

// 引用计数--
void bunpin(struct buf *b)
{
    acquire(&bcache.lock);
    b->refcnt--;
    release(&bcache.lock);
}
