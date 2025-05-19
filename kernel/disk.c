#include "include/types.h"
#include "include/param.h"
#include "include/memlayout.h"
#include "include/riscv.h"
#include "include/buf.h"
#include "include/sdcard.h"
#include "include/dmac.h"

// 通过 SPI 初始化 SD卡，并初始化 sdcard_lock
void disk_init(void)
{
    sdcard_init();
}

// 读取一个扇区
void disk_read(struct buf *b)
{
    sdcard_read_sector(b->data, b->sectorno);
}

// 写入一个扇区
void disk_write(struct buf *b)
{
    sdcard_write_sector(b->data, b->sectorno);
}

void disk_intr(void)
{
    dmac_intr(DMAC_CHANNEL0);
}
