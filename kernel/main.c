// Copyright (c) 2006-2019 Frans Kaashoek, Robert Morris, Russ Cox,
//                         Massachusetts Institute of Technology

#include "include/types.h"
#include "include/param.h"
#include "include/memlayout.h"
#include "include/riscv.h"
#include "include/sbi.h"
#include "include/console.h"
#include "include/printf.h"
#include "include/kalloc.h"
#include "include/timer.h"
#include "include/trap.h"
#include "include/proc.h"
#include "include/plic.h"
#include "include/vm.h"
#include "include/disk.h"
#include "include/buf.h"
#ifndef QEMU
#include "include/sdcard.h"
#include "include/fpioa.h"
#include "include/dmac.h"
#endif

// tp 保存 hartid
static inline void inithartid(unsigned long hartid)
{
    asm volatile("mv tp, %0" : : "r"(hartid & 0x1));
}

volatile static int started = 0;

void main(unsigned long hartid, unsigned long dtb_pa)
{
    inithartid(hartid); // 将 hartid 保存到 tp 寄存器中

    // 核心 0
    if (hartid == 0)
    {
        consoleinit();    // 初始化控制台的自旋锁和读写回调函数
        printfinit();     // 初始化 printf 用到的自旋锁
        print_logo();     // 打印 LOGO
        kinit();          // 初始化自旋锁，将空余空间回收到链表
        kvminit();        // 初始化内核页表，映射外设地址、内核段、数据段、TRAMPOLINE
        kvminithart();    // 刷新页表寄存器
        timerinit();      // 初始化保护变量 ticks 的自旋锁
        trapinithart();   // 设置 S 模式中断向量、启动 S 模式外部中断、软件中断、定时器中断，设置下次的定时时间
        procinit();       // 初始化保护 PID 和每个 proc 的自旋锁
        plicinit();       // 设置磁盘中断和串口中断的优先级
        plicinithart();   // 启动 DISK_IRQ 和 UART_IRQ 中断
        fpioa_pin_init(); // 设置对应的管脚功能为 SPI
        dmac_init();      // 调用 SDK 初始化 DMA
        disk_init();      // 初始化 SD 卡为 SPI 模式
        binit();          // 构建双向环形链表，初始化每一个buf的睡眠锁
        fileinit();       // 初始化文件描述符列表和自旋锁
        userinit();       // 为 init 进程分配资源、映射物理页面到 pagetable 和 kpagetable
        printf("hart 0 init done\n");

        // 向其他的核发送 IPI
        for (int i = 1; i < NCPU; i++)
        {
            unsigned long mask = 1 << i;
            sbi_send_ipi(&mask);
        }
        __sync_synchronize();
        started = 1;
    }
    // 核心 1
    else
    {
        while (started == 0)
            ;
        __sync_synchronize();

        kvminithart();  // 刷新页表寄存器
        trapinithart(); // 设置 S 模式中断向量、启动 S 模式外部中断、软件中断、定时器中断，设置下次的定时时间
        plicinithart(); // 启动 DISK_IRQ 和 UART_IRQ 中断
        printf("hart 1 init done\n");
    }

    // 执行线程调度函数
    scheduler();
}
