#ifndef __FILE_H
#define __FILE_H

struct file
{
    enum
    {
        FD_NONE,
        FD_PIPE,
        FD_ENTRY,
        FD_DEVICE
    } type;            // 文件描述符类型
    int ref;           // 引用计数
    char readable;     // 是否可读
    char writable;     // 是否可写
    struct pipe *pipe; // FD_PIPE
    struct dirent *ep; // 文件描述符对应的目录项
    uint off;          // FD_ENTRY使用，表示访问目录或者文件的偏移量
    short major;       // FD_DEVICE
};

// #define major(dev)  ((dev) >> 16 & 0xFFFF)
// #define minor(dev)  ((dev) & 0xFFFF)
// #define	mkdev(m,n)  ((uint)((m)<<16| (n)))

// map major device number to device functions.
struct devsw
{
    int (*read)(int, uint64, int);
    int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

#define CONSOLE 1

struct file *filealloc(void);
void fileclose(struct file *);
struct file *filedup(struct file *);
void fileinit(void);
int fileread(struct file *, uint64, int n);
int filestat(struct file *, uint64 addr);
int filewrite(struct file *, uint64, int n);
int dirnext(struct file *f, uint64 addr);

#endif