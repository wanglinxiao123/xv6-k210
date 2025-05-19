
#include "include/types.h"
#include "include/param.h"
#include "include/memlayout.h"
#include "include/riscv.h"
#include "include/sbi.h"
#include "include/plic.h"
#include "include/proc.h"
#include "include/printf.h"

// 设置磁盘中断和串口中断的优先级
void plicinit(void)
{
    writed(1, PLIC_V + DISK_IRQ * sizeof(uint32));
    writed(1, PLIC_V + UART_IRQ * sizeof(uint32));
}

// 启动 DISK_IRQ 和 UART_IRQ 中断
void plicinithart(void)
{
    int hart = cpuid();
    uint32 *hart_m_enable = (uint32 *)PLIC_MENABLE(hart);
    *(hart_m_enable) = readd(hart_m_enable) | (1 << DISK_IRQ);
    uint32 *hart0_m_int_enable_hi = hart_m_enable + 1;
    *(hart0_m_int_enable_hi) = readd(hart0_m_int_enable_hi) | (1 << (UART_IRQ % 32));
}

// ask the PLIC what interrupt we should serve.
int plic_claim(void)
{
    int hart = cpuid();
    int irq;
#ifndef QEMU
    irq = *(uint32 *)PLIC_MCLAIM(hart);
#else
    irq = *(uint32 *)PLIC_SCLAIM(hart);
#endif
    return irq;
}

// tell the PLIC we've served this IRQ.
void plic_complete(int irq)
{
    int hart = cpuid();
#ifndef QEMU
    *(uint32 *)PLIC_MCLAIM(hart) = irq;
#else
    *(uint32 *)PLIC_SCLAIM(hart) = irq;
#endif
}
