#ifndef __FAT32_H
#define __FAT32_H

#include "sleeplock.h"
#include "stat.h"

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04 // 该项属于系统
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10 // 该项是目录
#define ATTR_ARCHIVE 0x20
#define ATTR_LONG_NAME 0x0F

#define LAST_LONG_ENTRY 0x40
#define FAT32_EOC 0x0ffffff8
#define EMPTY_ENTRY 0xe5
#define END_OF_ENTRY 0x00
#define CHAR_LONG_NAME 13
#define CHAR_SHORT_NAME 11

#define FAT32_MAX_FILENAME 255
#define FAT32_MAX_PATH 260
#define ENTRY_CACHE_NUM 50

struct dirent
{
    char filename[FAT32_MAX_FILENAME + 1];
    uint8 attribute;   // 条目属性
    uint32 first_clus; // 条目起始簇号
    uint32 file_size;  // 文件大小

    uint32 cur_clus; // 条目当前簇号
    uint clus_cnt;   // 已经遍历到第几个簇

    /* for OS */
    uint8 dev;             // 磁盘号
    uint8 dirty;           // 是否被修改
    short valid;           // 该项是否被填充数据
    int ref;               // 引用计数
    uint32 off;            // 相对于父目录列表的偏移
    struct dirent *parent; // 父条目指针
    struct dirent *next;   // 链表后节点
    struct dirent *prev;   // 链表前节点
    struct sleeplock lock;
};

int fat32_init(void);
struct dirent *dirlookup(struct dirent *entry, char *filename, uint *poff);
char *formatname(char *name);
void emake(struct dirent *dp, struct dirent *ep, uint off);
struct dirent *ealloc(struct dirent *dp, char *name, int attr);
struct dirent *edup(struct dirent *entry);
void eupdate(struct dirent *entry);
void etrunc(struct dirent *entry);
void eremove(struct dirent *entry);
void eput(struct dirent *entry);
void estat(struct dirent *ep, struct stat *st);
void elock(struct dirent *entry);
void eunlock(struct dirent *entry);
int enext(struct dirent *dp, struct dirent *ep, uint off, int *count);
struct dirent *ename(char *path);
struct dirent *enameparent(char *path, char *name);
int eread(struct dirent *entry, int user_dst, uint64 dst, uint off, uint n);
int ewrite(struct dirent *entry, int user_src, uint64 src, uint off, uint n);

#endif