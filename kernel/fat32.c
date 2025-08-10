#include "include/param.h"
#include "include/types.h"
#include "include/riscv.h"
#include "include/spinlock.h"
#include "include/sleeplock.h"
#include "include/buf.h"
#include "include/proc.h"
#include "include/stat.h"
#include "include/fat32.h"
#include "include/string.h"
#include "include/printf.h"

/* fields that start with "_" are something we don't use */

typedef struct short_name_entry
{
    char name[CHAR_SHORT_NAME]; // 文件名，8+3 格式，共11字节（不含空格填充）
    uint8 attr;                 // 属性字段，如目录、系统文件、只读等
    uint8 _nt_res;              // Windows NT 保留（NT使用）
    uint8 _crt_time_tenth;      // 创建时间的 1/10 秒部分（0-199）
    uint16 _crt_time;           // 创建时间（小时:分钟:秒）
    uint16 _crt_date;           // 创建日期（年:月:日）
    uint16 _lst_acce_date;      // 最后访问日期
    uint16 fst_clus_hi;         // 首簇号的高 16 位（FAT32 专用）
    uint16 _lst_wrt_time;       // 最后写入时间
    uint16 _lst_wrt_date;       // 最后写入日期
    uint16 fst_clus_lo;         // 首簇号的低 16 位
    uint32 file_size;           // 文件大小（单位：字节）
} __attribute__((packed, aligned(4))) short_name_entry_t;

typedef struct long_name_entry
{
    uint8 order;         // 序号，表示该条目在多个LFN条目中的顺序，最高位为1表示这是最后一个条目（LAST_LONG_ENTRY）
    wchar name1[5];      // LFN 名称的第1部分，5个UTF-16字符
    uint8 attr;          // 属性，固定为 ATTR_LONG_NAME（0x0F）
    uint8 _type;         // 类型，通常为0（保留字段）
    uint8 checksum;      // 对应短文件名的校验和，用于验证LFN对应哪个短文件名
    wchar name2[6];      // LFN 名称的第2部分，6个UTF-16字符
    uint16 _fst_clus_lo; // 保留字段，必须为0（因为LFN条目不使用起始簇号）
    wchar name3[2];      // LFN 名称的第3部分，2个UTF-16字符
} __attribute__((packed, aligned(4))) long_name_entry_t;

// 所有目录项大小均为 32 字节
union dentry
{
    short_name_entry_t sne;
    long_name_entry_t lne;
};

static struct
{
    uint32 first_data_sec; // 第一个数据扇区号
    uint32 data_sec_cnt;   // 数据扇区数
    uint32 data_clus_cnt;  // 数据区总共有多少个簇
    uint32 byts_per_clus;  // 每簇的字节数

    // FAT32 引导扇区
    struct
    {
        uint16 byts_per_sec; // 每扇区字节数
        uint8 sec_per_clus;  // 每簇扇区数
        uint16 rsvd_sec_cnt; // 保留扇区数
        uint8 fat_cnt;       // FAT 表数量
        uint32 hidd_sec;     // 隐藏扇区数
        uint32 tot_sec;      // 总扇区数
        uint32 fat_sz;       // 每个 FAT 占用的扇区大小
        uint32 root_clus;    // 根目录起始簇号
    } bpb;

} fat;

// 条目缓存
static struct entry_cache
{
    struct spinlock lock;                   // 保护缓存的自旋锁
    struct dirent entries[ENTRY_CACHE_NUM]; // 缓存的条目
} ecache;

static struct dirent root;

// 读取保留区域、初始化 fat 结构体
// 初始化根条目
// 初始化条目缓存项，并形成一个环形链表
int fat32_init()
{
    struct buf *b = bread(0, 0);
    if (strncmp((char const *)(b->data + 82), "FAT32", 5))
    {
        panic("not FAT32 volume");
    }

    memmove(&fat.bpb.byts_per_sec, b->data + 11, 2);  // 每扇区字节数
    fat.bpb.sec_per_clus = *(b->data + 13);           // 每簇扇区数
    fat.bpb.rsvd_sec_cnt = *(uint16 *)(b->data + 14); // 保留扇区数
    fat.bpb.fat_cnt = *(b->data + 16);                // FAT 表数量
    fat.bpb.hidd_sec = *(uint32 *)(b->data + 28);     // 隐藏扇区数
    fat.bpb.tot_sec = *(uint32 *)(b->data + 32);      // 总扇区数
    fat.bpb.fat_sz = *(uint32 *)(b->data + 36);       // 每个 FAT 占用的扇区大小
    fat.bpb.root_clus = *(uint32 *)(b->data + 44);    // 根目录起始簇号

    fat.first_data_sec = fat.bpb.rsvd_sec_cnt + fat.bpb.fat_cnt * fat.bpb.fat_sz; // 第一个数据扇区号 = 保留区域大小 + FAT 表数量 * 每个 FAT 占用的扇区大小
    fat.data_sec_cnt = fat.bpb.tot_sec - fat.first_data_sec;                      // 数据扇区数 = 总扇区数 - 数据起始扇区
    fat.data_clus_cnt = fat.data_sec_cnt / fat.bpb.sec_per_clus;                  // 数据区总共有多少个簇
    fat.byts_per_clus = fat.bpb.sec_per_clus * fat.bpb.byts_per_sec;              // 每簇的字节数 = 每簇扇区数 * 每扇区字节数

    brelse(b);

    // 确认扇区大小
    if (BSIZE != fat.bpb.byts_per_sec)
    {
        panic("byts_per_sec != BSIZE");
    }

    initlock(&ecache.lock, "ecache");

    // 初始化根目录
    memset(&root, 0, sizeof(root));
    initsleeplock(&root.lock, "entry");
    root.attribute = (ATTR_DIRECTORY | ATTR_SYSTEM);
    root.first_clus = root.cur_clus = fat.bpb.root_clus;
    root.valid = 1;
    root.prev = &root;
    root.next = &root;

    // 初始化目录缓存项，并形成一个环形链表
    for (struct dirent *de = ecache.entries; de < ecache.entries + ENTRY_CACHE_NUM; de++)
    {
        de->dev = 0;
        de->valid = 0;
        de->ref = 0;
        de->dirty = 0;
        de->parent = 0;
        de->next = root.next;
        de->prev = &root;
        initsleeplock(&de->lock, "entry");
        root.next->prev = de;
        root.next = de;
    }
    return 0;
}

// 计算簇号 cluster 对应的第一个扇区号
static inline uint32 first_sec_of_clus(uint32 cluster)
{
    return ((cluster - 2) * fat.bpb.sec_per_clus) + fat.first_data_sec;
}

// 找到 cluster 簇在第 fat_num FAT表中对应的扇区号
static inline uint32 fat_sec_of_clus(uint32 cluster, uint8 fat_num)
{
    return fat.bpb.rsvd_sec_cnt + (cluster << 2) / fat.bpb.byts_per_sec + fat.bpb.fat_sz * (fat_num - 1);
}

// 计算 cluster 簇在扇区中的偏移
static inline uint32 fat_offset_of_clus(uint32 cluster)
{
    return (cluster << 2) % fat.bpb.byts_per_sec;
}

// 根据当前簇号，从 FAT 表中读取下一个簇号
static uint32 read_fat(uint32 cluster)
{
    // 如果簇已经结束，则直接返回
    if (cluster >= FAT32_EOC)
    {
        return cluster;
    }

    // 簇号超出范围就直接返回
    if (cluster > fat.data_clus_cnt + 1)
    {
        return 0;
    }

    // 读取下一个簇号
    uint32 fat_sec = fat_sec_of_clus(cluster, 1);
    struct buf *b = bread(0, fat_sec);
    uint32 next_clus = *(uint32 *)(b->data + fat_offset_of_clus(cluster));
    brelse(b);

    return next_clus;
}

// 将 FAT 表中的 cluster 簇的值写为 content
static int write_fat(uint32 cluster, uint32 content)
{
    if (cluster > fat.data_clus_cnt + 1)
    {
        return -1;
    }

    uint32 fat_sec = fat_sec_of_clus(cluster, 1);
    struct buf *b = bread(0, fat_sec);

    uint off = fat_offset_of_clus(cluster);
    *(uint32 *)(b->data + off) = content;
    bwrite(b);

    brelse(b);
    return 0;
}

// 清零对应的簇
static void zero_clus(uint32 cluster)
{
    uint32 sec = first_sec_of_clus(cluster);
    struct buf *b;
    for (int i = 0; i < fat.bpb.sec_per_clus; i++)
    {
        b = bread(0, sec++);
        memset(b->data, 0, BSIZE);
        bwrite(b);
        brelse(b);
    }
}

// 遍历 FAT 表，找到空闲簇，在表中标记为已分配
// 清除数据扇区中对应的簇
static uint32 alloc_clus(uint8 dev)
{
    struct buf *b;
    uint32 sec = fat.bpb.rsvd_sec_cnt;                                // FAT 区起始扇区号
    uint32 const ent_per_sec = fat.bpb.byts_per_sec / sizeof(uint32); // 每个扇区的 FAT 表项数量

    for (uint32 i = 0; i < fat.bpb.fat_sz; i++, sec++)
    {
        b = bread(dev, sec);

        for (uint32 j = 0; j < ent_per_sec; j++)
        {
            // 找到空闲簇
            if (((uint32 *)(b->data))[j] == 0)
            {
                // 标记该空闲簇为已分配
                ((uint32 *)(b->data))[j] = FAT32_EOC + 7;
                bwrite(b);
                brelse(b);

                // 清零簇内容
                uint32 clus = i * ent_per_sec + j;
                zero_clus(clus);

                return clus;
            }
        }

        brelse(b);
    }

    panic("no clusters");
}

// 将 FAT 表对应的 cluster 簇下标写为 0
static void free_clus(uint32 cluster)
{
    write_fat(cluster, 0);
}

// write = 1, 则将 (data, n) 写入到 (cluster, off, n)
// write = 0, 则将 (cluster, off, n) 写入到 (data, n)
static uint rw_clus(uint32 cluster, int write, int user, uint64 data, uint off, uint n)
{
    // 验证参数有效
    if (off + n > fat.byts_per_clus)
    {
        panic("offset out of range");
    }

    // 计算 (cluster, off) 对应的第一个扇区号
    uint sec = first_sec_of_clus(cluster) + off / fat.bpb.byts_per_sec;
    off = off % fat.bpb.byts_per_sec;

    struct buf *bp;

    uint tot, m;
    int bad = 0;
    for (tot = 0; tot < n; tot += m, off += m, data += m, sec++)
    {
        bp = bread(0, sec);
        m = BSIZE - off % BSIZE;
        if (n - tot < m)
        {
            m = n - tot;
        }

        if (write)
        {
            if ((bad = either_copyin(bp->data + (off % BSIZE), user, data, m)) != -1)
            {
                bwrite(bp);
            }
        }
        else
        {
            bad = either_copyout(user, data, bp->data + (off % BSIZE), m);
        }
        brelse(bp);

        if (bad == -1)
        {
            break;
        }
    }
    return tot;
}

// 根据文件偏移量 off 找到对应的簇号，并更新 entry->cur_clus 和 entry->clus_cnt
// alloc = 1，在簇号不满足时进行分配

// 找到目录项 entry 偏移 off 处的簇号
// 如果 off > 当前总簇数，则向后拓展
// 如果 off < 当前访问的簇数，则重新遍历
// alloc = 1 则分配空间
static int reloc_clus(struct dirent *entry, uint off, int alloc)
{
    // 计算 off 对应的起始簇下标
    int clus_num = off / fat.byts_per_clus;

    // 如果 off 对应的簇号大于当前簇数，就向后扩展
    while (clus_num > entry->clus_cnt)
    {
        int clus = read_fat(entry->cur_clus);
        if (clus >= FAT32_EOC)
        {
            if (alloc)
            {
                clus = alloc_clus(entry->dev);    // 从 dev 中找到空闲簇，清零簇内容并记录在 FAT 表中
                write_fat(entry->cur_clus, clus); // 将 FAT 表中的 cluster 簇的值写为 content
            }
            else
            {
                entry->cur_clus = entry->first_clus;
                entry->clus_cnt = 0;
                return -1;
            }
        }
        entry->cur_clus = clus;
        entry->clus_cnt++;
    }

    // 如果目标簇号比当前记录的簇数还小，需要重新从头遍历
    if (clus_num < entry->clus_cnt)
    {
        entry->cur_clus = entry->first_clus;
        entry->clus_cnt = 0;

        while (entry->clus_cnt < clus_num)
        {
            entry->cur_clus = read_fat(entry->cur_clus);
            if (entry->cur_clus >= FAT32_EOC)
            {
                panic("reloc_clus");
            }
            entry->clus_cnt++;
        }
    }
    return off % fat.byts_per_clus;
}

// 将 (entry, off, n) 的内容写入到 (dst, n)
int eread(struct dirent *entry, int user_dst, uint64 dst, uint off, uint n)
{
    // 参数校验
    if (off > entry->file_size || off + n < off || (entry->attribute & ATTR_DIRECTORY))
    {
        return 0;
    }
    if (off + n > entry->file_size)
    {
        n = entry->file_size - off;
    }

    uint tot, m;
    for (tot = 0; entry->cur_clus < FAT32_EOC && tot < n; tot += m, off += m, dst += m)
    {
        // 根据文件偏移量 off 找到对应的簇号，并更新 entry->cur_clus 和 entry->clus_cnt
        reloc_clus(entry, off, 0);
        m = fat.byts_per_clus - off % fat.byts_per_clus;
        if (n - tot < m)
        {
            m = n - tot;
        }

        // 将 (entry->cur_clus, off % fat.byts_per_clus, m) 写入到 (dst, m)
        if (rw_clus(entry->cur_clus, 0, user_dst, dst, off % fat.byts_per_clus, m) != m)
        {
            break;
        }
    }
    return tot;
}

// 将 (src, n) 写入到 (entry, off, n)
int ewrite(struct dirent *entry, int user_src, uint64 src, uint off, uint n)
{
    // 参数校验
    if (off > entry->file_size || off + n < off || (uint64)off + n > 0xffffffff || (entry->attribute & ATTR_READ_ONLY))
    {
        return -1;
    }

    // 如果是空文件，就先分配一个簇为首簇
    if (entry->first_clus == 0)
    {
        entry->cur_clus = entry->first_clus = alloc_clus(entry->dev);
        entry->clus_cnt = 0;
        entry->dirty = 1;
    }

    uint tot, m;
    for (tot = 0; tot < n; tot += m, off += m, src += m)
    {
        // 根据文件偏移量 off 找到对应的簇号，并更新 entry->cur_clus 和 entry->clus_cnt
        reloc_clus(entry, off, 1);
        m = fat.byts_per_clus - off % fat.byts_per_clus;
        if (n - tot < m)
        {
            m = n - tot;
        }

        // 则将 (data, n) 写入到 (cluster, off, n)
        if (rw_clus(entry->cur_clus, 1, user_src, src, off % fat.byts_per_clus, m) != m)
        {
            break;
        }
    }

    if (n > 0)
    {
        if (off > entry->file_size)
        {
            entry->file_size = off;
            entry->dirty = 1;
        }
    }

    return tot;
}

// 从 parent 目录开始，获取一个 name 的缓存（直接返回或者新分配）
static struct dirent *eget(struct dirent *parent, char *name)
{
    struct dirent *ep;
    acquire(&ecache.lock);

    // 如果指定了 name, 先从缓存中查找有没有 parent 的子目录 name
    if (name)
    {
        for (ep = root.next; ep != &root; ep = ep->next)
        {
            if (ep->valid == 1 && ep->parent == parent && strncmp(ep->filename, name, FAT32_MAX_FILENAME) == 0)
            {
                if (ep->ref++ == 0)
                {
                    ep->parent->ref++;
                }
                release(&ecache.lock);
                return ep;
            }
        }
    }

    // 通过 LRU 算法找到一个缓存项并返回
    for (ep = root.prev; ep != &root; ep = ep->prev)
    {
        if (ep->ref == 0)
        {
            ep->ref = 1;
            ep->dev = parent->dev;
            ep->off = 0;
            ep->valid = 0;
            ep->dirty = 0;
            release(&ecache.lock);
            return ep;
        }
    }
    panic("eget: insufficient ecache");
    return 0;
}

// 检查文件名 name 的合法化
char *formatname(char *name)
{
    static char illegal[] = {'\"', '*', '/', ':', '<', '>', '?', '\\', '|', 0};
    char *p;

    // 去除前导符号
    while (*name == ' ' || *name == '.')
    {
        name++;
    }

    // 遍历到了非法字符，就直接返回
    for (p = name; *p; p++)
    {
        char c = *p;
        if (c < 0x20 || strchr(illegal, c))
        {
            return 0;
        }
    }

    // 去除末尾的空格
    while (p-- > name)
    {
        if (*p != ' ')
        {
            p[1] = '\0';
            break;
        }
    }

    return name;
}

// 将任意合法长文件名转换成兼容 FAT 8.3 格式的短文件名
static void generate_shortname(char *shortname, char *name)
{
    static char illegal[] = {'+', ',', ';', '=', '[', ']', 0}; // these are legal in l-n-e but not s-n-e
    int i = 0;

    // 找到最后一个 '.'
    char c, *p = name;
    for (int j = strlen(name) - 1; j >= 0; j--)
    {
        if (name[j] == '.')
        {
            p = name + j;
            break;
        }
    }

    while (i < CHAR_SHORT_NAME && (c = *name++))
    {
        // 正准备处理扩展名
        if (i == 8 && p)
        {
            // 已经写过了扩展名的内容
            if (p + 1 < name)
            {
                break;
            }
            // 设置下一次处理扩展名
            else
            {
                name = p + 1, p = 0;
                continue;
            }
        }

        // 忽略空格
        if (c == ' ')
        {
            continue;
        }

        // 如果是最后一个点
        if (c == '.')
        {
            if (name > p)
            {
                memset(shortname + i, ' ', 8 - i);
                i = 8, p = 0;
            }
            continue;
        }

        // 小写转大写
        if (c >= 'a' && c <= 'z')
        {
            c += 'A' - 'a';
        }
        // 替换非法字符为下划线
        else if (strchr(illegal, c) != NULL)
        {
            c = '_';
        }

        shortname[i++] = c;
    }

    // 文件名右侧填充空格
    while (i < CHAR_SHORT_NAME)
    {
        shortname[i++] = ' ';
    }
}

// 返回短文件名的校验码
uint8 cal_checksum(uchar *shortname)
{
    uint8 sum = 0;
    for (int i = CHAR_SHORT_NAME; i != 0; i--)
    {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *shortname++;
    }
    return sum;
}

// 在文件系统的 dp 目录中插入一个目录项 ep
// off == 0，说明是第一个目录项，创建 .
// off == [1, 32]，说明是第二个目录项，创建 ..
// 对于一般目录项，先写 LFN, 最后写短文件名目录项 ep
void emake(struct dirent *dp, struct dirent *ep, uint off)
{
    // 检验目录项
    if (!(dp->attribute & ATTR_DIRECTORY))
    {
        panic("emake: not dir");
    }
    if (off % sizeof(union dentry))
    {
        panic("emake: not aligned");
    }

    union dentry de;
    memset(&de, 0, sizeof(de));
    if (off <= 32)
    {
        if (off == 0)
        {
            strncpy(de.sne.name, ".          ", sizeof(de.sne.name));
        }
        else
        {
            strncpy(de.sne.name, "..         ", sizeof(de.sne.name));
        }

        de.sne.attr = ATTR_DIRECTORY;                              // 目录属性
        de.sne.fst_clus_hi = (uint16)(ep->first_clus >> 16);       // 首簇的高16位
        de.sne.fst_clus_lo = (uint16)(ep->first_clus & 0xffff);    // 首簇的低16位
        de.sne.file_size = 0;                                      // 文件大小
        off = reloc_clus(dp, off, 1);                              // 根据文件偏移量 off 找到对应的簇号，并更新 dp->cur_clus 和 dp->clus_cnt
        rw_clus(dp->cur_clus, 1, 0, (uint64)&de, off, sizeof(de)); // 将 de 写入到 (dp->cur_clus, off)
    }
    else
    {
        // 计算需要多少个 LFN 项
        int entcnt = (strlen(ep->filename) + CHAR_LONG_NAME - 1) / CHAR_LONG_NAME;

        // 生成对应的短文件名
        char shortname[CHAR_SHORT_NAME + 1];
        memset(shortname, 0, sizeof(shortname));
        generate_shortname(shortname, ep->filename);

        de.lne.checksum = cal_checksum((uchar *)shortname); // 计算校验和
        de.lne.attr = ATTR_LONG_NAME;                       // 设置目录属性

        for (int i = entcnt; i > 0; i--)
        {
            // 记录是最后一项 LFN 项
            if ((de.lne.order = i) == entcnt)
            {
                de.lne.order |= LAST_LONG_ENTRY;
            }

            char *p = ep->filename + (i - 1) * CHAR_LONG_NAME;
            uint8 *w = (uint8 *)de.lne.name1;
            int end = 0;
            for (int j = 1; j <= CHAR_LONG_NAME; j++)
            {
                if (end)
                {
                    *w++ = 0xff; // on k210, unaligned reading is illegal
                    *w++ = 0xff;
                }
                else
                {
                    if ((*w++ = *p++) == 0)
                    {
                        end = 1;
                    }
                    *w++ = 0;
                }

                // 切换写入Unicode字符的不同字段
                switch (j)
                {
                case 5:
                    w = (uint8 *)de.lne.name2;
                    break;
                case 11:
                    w = (uint8 *)de.lne.name3;
                    break;
                }
            }

            uint off2 = reloc_clus(dp, off, 1);                         // 根据文件偏移量 off 找到对应的簇号，并更新 dp->cur_clus 和 dp->clus_cnt
            rw_clus(dp->cur_clus, 1, 0, (uint64)&de, off2, sizeof(de)); // 则将 (data, n) 写入到 (dp->cur_clus, off, n)
            off += sizeof(de);
        }

        memset(&de, 0, sizeof(de));
        strncpy(de.sne.name, shortname, sizeof(de.sne.name));
        de.sne.attr = ep->attribute;
        de.sne.fst_clus_hi = (uint16)(ep->first_clus >> 16);    // first clus high 16 bits
        de.sne.fst_clus_lo = (uint16)(ep->first_clus & 0xffff); // low 16 bits
        de.sne.file_size = ep->file_size;                       // filesize is updated in eupdate()
        off = reloc_clus(dp, off, 1);
        rw_clus(dp->cur_clus, 1, 0, (uint64)&de, off, sizeof(de));
    }
}

// 在 dp 目录下创建一个新条目 name，附带属性 attr
struct dirent *ealloc(struct dirent *dp, char *name, int attr)
{
    // 验证是目录项
    if (!(dp->attribute & ATTR_DIRECTORY))
    {
        panic("ealloc not dir");
    }

    // 验证参数合法
    if (dp->valid != 1 || !(name = formatname(name)))
    {
        return NULL;
    }

    // 搜索 dp 目录中的 filename 条目，如果已被缓存，直接返回
    struct dirent *ep;
    uint off = 0;
    if ((ep = dirlookup(dp, name, &off)) != 0)
    {
        return ep;
    }

    // 获取一个缓存
    ep = eget(dp, name);
    elock(ep);

    ep->attribute = attr;
    ep->file_size = 0;
    ep->first_clus = 0;
    ep->parent = edup(dp);
    ep->off = off;
    ep->clus_cnt = 0;
    ep->cur_clus = 0;
    ep->dirty = 0;
    strncpy(ep->filename, name, FAT32_MAX_FILENAME);
    ep->filename[FAT32_MAX_FILENAME] = '\0';

    // 如果是目录，就为该目录项生成 '.' 和 '..'
    if (attr == ATTR_DIRECTORY)
    { 
        ep->attribute |= ATTR_DIRECTORY;
        ep->cur_clus = ep->first_clus = alloc_clus(dp->dev);
        emake(ep, ep, 0);
        emake(ep, dp, 32);
    }
    else
    {
        ep->attribute |= ATTR_ARCHIVE;
    }

    emake(dp, ep, off);
    ep->valid = 1;
    eunlock(ep);
    return ep;
}

// entry 引用++
struct dirent *edup(struct dirent *entry)
{
    if (entry != 0)
    {
        acquire(&ecache.lock);
        entry->ref++;
        release(&ecache.lock);
    }
    return entry;
}

// 查找 entry 在磁盘中的位置并更新
void eupdate(struct dirent *entry)
{
    // 没有必要更新磁盘
    if (!entry->dirty || entry->valid != 1)
    {
        return;
    }

    // 查找目录项 entry 在父目录项的 位置
    uint entcnt = 0;
    uint32 off = reloc_clus(entry->parent, entry->off, 0);
    rw_clus(entry->parent->cur_clus, 0, 0, (uint64)&entcnt, off, 1);
    entcnt &= ~LAST_LONG_ENTRY;
    off = reloc_clus(entry->parent, entry->off + (entcnt << 5), 0);

    // 读取 entry 实际存放的位置
    union dentry de;
    rw_clus(entry->parent->cur_clus, 0, 0, (uint64)&de, off, sizeof(de));

    de.sne.fst_clus_hi = (uint16)(entry->first_clus >> 16);    // 条目起始簇号
    de.sne.fst_clus_lo = (uint16)(entry->first_clus & 0xffff); // 条目起始簇号
    de.sne.file_size = entry->file_size;                       // 文件大小
    rw_clus(entry->parent->cur_clus, 1, 0, (uint64)&de, off, sizeof(de));
    entry->dirty = 0;
}

// 从 FAT32 的目录表中删除条目 entry
void eremove(struct dirent *entry)
{
    // 验证 entry 有效
    if (entry->valid != 1)
    {
        return;
    }

    uint entcnt = 0;
    uint32 off = entry->off;
    uint32 off2 = reloc_clus(entry->parent, off, 0);
    rw_clus(entry->parent->cur_clus, 0, 0, (uint64)&entcnt, off2, 1);
    entcnt &= ~LAST_LONG_ENTRY;

    uint8 flag = EMPTY_ENTRY;
    for (int i = 0; i <= entcnt; i++)
    {
        rw_clus(entry->parent->cur_clus, 1, 0, (uint64)&flag, off2, 1);
        off += 32;
        off2 = reloc_clus(entry->parent, off, 0);
    }

    entry->valid = -1;
}

// 删除 entry 对应的文件实际数据簇
// 更新 entry 的文件大小为 0
void etrunc(struct dirent *entry)
{
    for (uint32 clus = entry->first_clus; clus >= 2 && clus < FAT32_EOC;)
    {
        uint32 next = read_fat(clus);
        free_clus(clus);
        clus = next;
    }
    entry->file_size = 0;
    entry->first_clus = 0;
    entry->dirty = 1;
}

// 获取目录项的睡眠锁
void elock(struct dirent *entry)
{
    if (entry == 0 || entry->ref < 1)
    {
        panic("elock");
    }

    acquiresleep(&entry->lock);
}

// 释放目录项的睡眠锁
void eunlock(struct dirent *entry)
{
    if (entry == 0 || !holdingsleep(&entry->lock) || entry->ref < 1)
    {
        panic("eunlock");
    }
    releasesleep(&entry->lock);
}

// 若计数改为 0，则回收目录项缓存，将目录的新状态同步到磁盘中
void eput(struct dirent *entry)
{
    acquire(&ecache.lock);

    // 判断是否需要进一步处理
    if (entry != &root && entry->valid != 0 && entry->ref == 1)
    {
        // 释放 entry 目录项缓存
        acquiresleep(&entry->lock);
        entry->next->prev = entry->prev;
        entry->prev->next = entry->next;
        entry->next = root.next;
        entry->prev = &root;
        root.next->prev = entry;
        root.next = entry;
        release(&ecache.lock);

        // 删除 entry 对应的文件实际数据簇
        if (entry->valid == -1)
        {
            etrunc(entry);
        }
        // 同步数据到磁盘
        else
        {
            elock(entry->parent);
            eupdate(entry);
            eunlock(entry->parent);
        }
        releasesleep(&entry->lock);

        // 自动递归释放父节点
        struct dirent *eparent = entry->parent;
        acquire(&ecache.lock);
        entry->ref--;
        release(&ecache.lock);
        if (entry->ref == 0)
        {
            eput(eparent);
        }

        return;
    }

    entry->ref--;
    release(&ecache.lock);
}

// 获取目录项的统计信息
void estat(struct dirent *de, struct stat *st)
{
    strncpy(st->name, de->filename, STAT_MAX_NAME);
    st->type = (de->attribute & ATTR_DIRECTORY) ? T_DIR : T_FILE;
    st->dev = de->dev;
    st->size = de->file_size;
}

// 如果 dentry 是长文件名，将 13 字节的文件名复制到 buffer
// 如果 dentry 是短文件名，恢复原来的文件名
static void read_entry_name(char *buffer, union dentry *d)
{
    // 如果是长文件名，拼接 13 个文件名
    if (d->lne.attr == ATTR_LONG_NAME)
    {
        wchar temp[NELEM(d->lne.name1)];
        memmove(temp, d->lne.name1, sizeof(temp));
        snstr(buffer, temp, NELEM(d->lne.name1));
        buffer += NELEM(d->lne.name1);
        snstr(buffer, d->lne.name2, NELEM(d->lne.name2));
        buffer += NELEM(d->lne.name2);
        snstr(buffer, d->lne.name3, NELEM(d->lne.name3));
    }
    // 如果是短文件名，拼接原文件名，添加 .
    else
    {
        memset(buffer, 0, CHAR_SHORT_NAME + 2);
        int i;
        for (i = 0; d->sne.name[i] != ' ' && i < 8; i++)
        {
            buffer[i] = d->sne.name[i];
        }

        if (d->sne.name[8] != ' ')
        {
            buffer[i++] = '.';
        }

        for (int j = 8; j < CHAR_SHORT_NAME; j++, i++)
        {
            if (d->sne.name[j] == ' ')
            {
                break;
            }
            buffer[i] = d->sne.name[j];
        }
    }
}

// 将磁盘上的 dentry 元信息读取给 entry
static void read_entry_info(struct dirent *entry, union dentry *d)
{
    entry->attribute = d->sne.attr;                                              // 属性
    entry->first_clus = ((uint32)d->sne.fst_clus_hi << 16) | d->sne.fst_clus_lo; // 起始簇号
    entry->file_size = d->sne.file_size;                                         // 文件大小
    entry->cur_clus = entry->first_clus;                                         // 将当前簇号设置为起始簇号
    entry->clus_cnt = 0;                                                         // 设置已读簇数量为0
}

// 从 (ep, off) 开始遍历目录项
// 返回 -1 表示遍历到了目录项列表尾
// 返回  0 表示找到了 count 个目录项
// 返回  1 表示读到了目录项，将文件属性拷贝给 ep
int enext(struct dirent *dp, struct dirent *ep, uint off, int *count)
{
    // 确保 dp 是目录
    if (!(dp->attribute & ATTR_DIRECTORY))
    {
        panic("enext not dir");
    }
    // 确保 ep 是无效的
    if (ep->valid)
    {
        panic("enext ep valid");
    }
    // 确保偏移量 32 字节对齐
    if (off % 32)
    {
        panic("enext not align");
    }
    // 如果 dp 的缓存无效，直接返回
    if (dp->valid != 1)
    {
        return -1;
    }

    union dentry de;
    int cnt = 0; // 空闲目录项
    memset(ep->filename, 0, FAT32_MAX_FILENAME + 1);

    // 遍历从 off 开始的目录项
    for (int off2; (off2 = reloc_clus(dp, off, 0)) != -1; off += 32)
    {
        // 目录项结束则返回
        if (rw_clus(dp->cur_clus, 0, 0, (uint64)&de, off2, 32) != 32 || de.lne.order == END_OF_ENTRY)
        {
            return -1;
        }
        // 遇见空闲目录项
        if (de.lne.order == EMPTY_ENTRY)
        {
            cnt++;
            continue;
        }
        // 如果前面已有空闲目录项，但是当前目录项不空闲，则空闲项数量
        else if (cnt)
        {
            *count = cnt;
            return 0;
        }

        // 如果是长文件名，设置 count 为 长文件名数量 + 1
        // 将长文件名的一部分复制到 ep->filename
        if (de.lne.attr == ATTR_LONG_NAME)
        {
            int lcnt = de.lne.order & ~LAST_LONG_ENTRY;
            if (de.lne.order & LAST_LONG_ENTRY)
            {
                *count = lcnt + 1;
                count = 0;
            }
            read_entry_name(ep->filename + (lcnt - 1) * CHAR_LONG_NAME, &de);
        }
        // 如果是短文件名
        // 设置 count 为 1
        // 最后读取元信息到 ep
        else
        {
            if (count)
            {
                *count = 1;
                read_entry_name(ep->filename, &de);
            }
            read_entry_info(ep, &de);
            return 1;
        }
    }

    return -1;
}

// 搜索 dp 目录中的 filename 条目，返回内存中的 dirent
struct dirent *dirlookup(struct dirent *dp, char *filename, uint *poff)
{
    // 验证是否为目录
    if (!(dp->attribute & ATTR_DIRECTORY))
    {
        panic("dirlookup not DIR");
    }

    // 如果是自身目录，直接返回
    if (strncmp(filename, ".", FAT32_MAX_FILENAME) == 0)
    {
        return edup(dp);
    }
    // 如果是上一级目录，返回上一级条目
    else if (strncmp(filename, "..", FAT32_MAX_FILENAME) == 0)
    {
        if (dp == &root)
        {
            return edup(&root);
        }
        return edup(dp->parent);
    }

    // 如果当前目录的缓存内容无效，直接返回
    if (dp->valid != 1)
    {
        return NULL;
    }

    // 从 parent 目录开始，获取一个 name 的缓存（直接返回或者新分配）
    struct dirent *ep = eget(dp, filename);
    if (ep->valid == 1)
    {
        return ep;
    }

    // 计算文件名所需的目录项数
    int len = strlen(filename);
    int entcnt = (len + CHAR_LONG_NAME - 1) / CHAR_LONG_NAME + 1;

    int count = 0;
    int type;
    uint off = 0;

    // 目录项回到第一簇
    reloc_clus(dp, 0, 0);

    while ((type = enext(dp, ep, off, &count) != -1))
    {
        // 找到了若干目录项
        if (type == 0)
        {
            if (poff && count >= entcnt)
            {
                *poff = off;
                poff = 0;
            }
        }
        else if (strncmp(filename, ep->filename, FAT32_MAX_FILENAME) == 0)
        {
            ep->parent = edup(dp);
            ep->off = off;
            ep->valid = 1;
            return ep;
        }
        off += count << 5;
    }

    if (poff)
    {
        *poff = off;
    }

    eput(ep);
    return NULL;
}

// 从 path 中提取下一个路径元素，拷贝放入 name 中
static char *skipelem(char *path, char *name)
{
    while (*path == '/')
    {
        path++;
    }
    if (*path == 0)
    {
        return NULL;
    }
    char *s = path;
    while (*path != '/' && *path != 0)
    {
        path++;
    }
    int len = path - s;
    if (len > FAT32_MAX_FILENAME)
    {
        len = FAT32_MAX_FILENAME;
    }
    name[len] = 0;
    memmove(name, s, len);

    while (*path == '/')
    {
        path++;
    }
    return path;
}

// 依据 path 进行查找
// name 输出最后一级的名字
// parent = 1 则返回父目录的 dirent
// parent = 0 则返回子目录的 dirent
static struct dirent *lookup_path(char *path, int parent, char *name)
{
    struct dirent *entry, *next;
    // 路径为根目录
    if (*path == '/')
    {
        entry = edup(&root);
    } 
    // 路径不为根目录
    else if (*path != '\0')
    {
        entry = edup(myproc()->cwd);
    }
    else
    {
        return NULL;
    }

    while ((path = skipelem(path, name)) != 0)
    {
        elock(entry);
        if (!(entry->attribute & ATTR_DIRECTORY))
        {
            eunlock(entry);
            eput(entry);
            return NULL;
        }
        if (parent && *path == '\0')
        {
            eunlock(entry);
            return entry;
        }
        if ((next = dirlookup(entry, name, 0)) == 0)
        {
            eunlock(entry);
            eput(entry);
            return NULL;
        }
        eunlock(entry);
        eput(entry);
        entry = next;
    }
    if (parent)
    {
        eput(entry);
        return NULL;
    }
    return entry;
}

// 按照 path 进行查找，返回最后一级的 dirent
struct dirent *ename(char *path)
{
    char name[FAT32_MAX_FILENAME + 1];
    return lookup_path(path, 0, name);
}

// 按照 path 进行查找，返回最后一级父目录的 dirent
struct dirent *enameparent(char *path, char *name)
{
    return lookup_path(path, 1, name);
}
