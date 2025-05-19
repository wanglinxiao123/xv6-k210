#include "include/types.h"
#include "include/riscv.h"
#include "include/param.h"
#include "include/spinlock.h"
#include "include/sleeplock.h"
#include "include/fat32.h"
#include "include/file.h"
#include "include/pipe.h"
#include "include/stat.h"
#include "include/proc.h"
#include "include/printf.h"
#include "include/string.h"
#include "include/vm.h"

struct devsw devsw[NDEV];

// 文件描述符列表
struct
{
    struct spinlock lock;
    struct file file[NFILE];
} ftable;

// 初始化文件描述符列表和自旋锁
void fileinit(void)
{
    initlock(&ftable.lock, "ftable");
    struct file *f;
    for (f = ftable.file; f < ftable.file + NFILE; f++)
    {
        memset(f, 0, sizeof(struct file));
    }
}

// 从文件描述符列表中获取一个文件描述符
struct file *filealloc(void)
{
    struct file *f;
    acquire(&ftable.lock);

    for (f = ftable.file; f < ftable.file + NFILE; f++)
    {
        if (f->ref == 0)
        {
            f->ref = 1;
            release(&ftable.lock);
            return f;
        }
    }

    release(&ftable.lock);
    return NULL;
}

// 描述符 f 引用计数++
struct file *filedup(struct file *f)
{
    acquire(&ftable.lock);
    if (f->ref < 1)
    {
        panic("filedup");
    }

    f->ref++;
    release(&ftable.lock);
    return f;
}

// 描述符 f 引用计数--
// 计数为 0 则关闭管道、或回收目录项缓存
void fileclose(struct file *f)
{
    struct file ff;
    acquire(&ftable.lock);

    if (f->ref < 1)
    {
        panic("fileclose");
    }

    if (--f->ref > 0)
    {
        release(&ftable.lock);
        return;
    }

    ff = *f;
    f->ref = 0;
    f->type = FD_NONE;
    release(&ftable.lock);

    if (ff.type == FD_PIPE)
    {
        pipeclose(ff.pipe, ff.writable);
    }
    else if (ff.type == FD_ENTRY)
    {
        eput(ff.ep);
    }
    else if (ff.type == FD_DEVICE)
    {
    }
}

// 将文件描述符对应的目录项统计信息复制到 addr 中
int filestat(struct file *f, uint64 addr)
{
    struct stat st;

    if (f->type == FD_ENTRY)
    {
        elock(f->ep);
        estat(f->ep, &st);
        eunlock(f->ep);

        if (copyout2(addr, (char *)&st, sizeof(st)) < 0)
        {
            return -1;
        }
        return 0;
    }

    return -1;
}

// 从文件描述符 f 中读取数据到 (addr, n)
int fileread(struct file *f, uint64 addr, int n)
{
    int r = 0;

    if (f->readable == 0)
    {
        return -1;
    }

    switch (f->type)
    {
    // 如果是管道，则读取数据
    case FD_PIPE:
        r = piperead(f->pipe, addr, n);
        break;
    // 如果是设备类型，调用回调函数
    case FD_DEVICE:
        if (f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
        {
            return -1;
        }
        r = devsw[f->major].read(1, addr, n);
        break;
    // 如果是文件条目，读取文件内容
    case FD_ENTRY:
        elock(f->ep);
        if ((r = eread(f->ep, 1, addr, f->off, n)) > 0)
        {
            f->off += r;
        }
        eunlock(f->ep);
        break;
    default:
        panic("fileread");
    }

    return r;
}

// 将 (addr, n) 的数据写入到 f
int filewrite(struct file *f, uint64 addr, int n)
{
    int ret = 0;

    if (f->writable == 0)
    {
        return -1;
    }
    // 如果是管道，则写入数据
    if (f->type == FD_PIPE)
    {
        ret = pipewrite(f->pipe, addr, n);
    }
    // 如果是设备类型，则调用回调函数
    else if (f->type == FD_DEVICE)
    {
        if (f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
        {
            return -1;
        }
        ret = devsw[f->major].write(1, addr, n);
    }
    // 如果是文件条目，写入文件条目到 (addr, n)
    else if (f->type == FD_ENTRY)
    {
        elock(f->ep);
        if (ewrite(f->ep, 1, addr, f->off, n) == n)
        {
            ret = n;
            f->off += n;
        }
        else
        {
            ret = -1;
        }
        eunlock(f->ep);
    }
    else
    {
        panic("filewrite");
    }

    return ret;
}

// 逐项访问 f 对应的目录，将统计信息拷贝到 addr
int dirnext(struct file *f, uint64 addr)
{
    // 确认文件描述符 f 是目录
    if (f->readable == 0 || !(f->ep->attribute & ATTR_DIRECTORY))
    {
        return -1;
    }

    struct dirent de;
    struct stat st;
    int count = 0;
    int ret;
    elock(f->ep);

    // 找到有效目录项
    while ((ret = enext(f->ep, &de, f->off, &count)) == 0)
    {
        f->off += count * 32;
    }

    eunlock(f->ep);
    if (ret == -1)
    {
        return 0;
    }

    f->off += count * 32;
    estat(&de, &st);
    if (copyout2(addr, (char *)&st, sizeof(st)) < 0)
    {
        return -1;
    }

    return 1;
}