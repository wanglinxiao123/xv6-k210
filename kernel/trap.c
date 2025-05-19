#include "include/types.h"
#include "include/param.h"
#include "include/memlayout.h"
#include "include/riscv.h"
#include "include/spinlock.h"
#include "include/proc.h"
#include "include/sbi.h"
#include "include/plic.h"
#include "include/trap.h"
#include "include/syscall.h"
#include "include/printf.h"
#include "include/console.h"
#include "include/timer.h"
#include "include/disk.h"

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
extern void kernelvec();

int devintr();

// 设置 S 模式中断向量、启动 S 模式外部中断、软件中断、定时器中断
// 设置下次的定时时间
void trapinithart(void)
{
    w_stvec((uint64)kernelvec);                      // S 模式的 trap 向量
    w_sstatus(r_sstatus() | SSTATUS_SIE);            // 允许 S 模式中断
    w_sie(r_sie() | SIE_SEIE | SIE_SSIE | SIE_STIE); // 启动 S 模式外部中断、软件中断、定时器中断
    set_next_timeout();                              // 设置下次的定时时间
}

// 如果是系统调用，执行
// 尝试处理外部中断、定时器中断
// 如果是定时器中断，调用 yield
void usertrap(void)
{
    int which_dev = 0;

    if ((r_sstatus() & SSTATUS_SPP) != 0)
    {
        panic("usertrap: not from user mode");
    }

    w_stvec((uint64)kernelvec);

    struct proc *p = myproc();
    p->trapframe->epc = r_sepc();

    // 如果是系统调用
    if (r_scause() == 8)
    {
        if (p->killed)
        {
            exit(-1);
        }

        p->trapframe->epc += 4;
        intr_on();
        syscall();
    }
    // 处理外部中断、定时器中断
    else if ((which_dev = devintr()) != 0)
    {
    }
    else
    {
        printf("\nusertrap(): unexpected scause %p pid=%d %s\n", r_scause(), p->pid, p->name);
        printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
        p->killed = 1;
    }

    if (p->killed)
    {
        exit(-1);
    }

    if (which_dev == 2)
    {
        yield();
    }

    usertrapret();
}

void usertrapret(void)
{
    struct proc *p = myproc();

    // 先关闭中断
    intr_off();

    // 设置 trap 向量
    w_stvec(TRAMPOLINE + (uservec - trampoline));

    // 保存寄存器到 p->trapframe
    p->trapframe->kernel_satp = r_satp();         // kernel page table
    p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
    p->trapframe->kernel_trap = (uint64)usertrap;
    p->trapframe->kernel_hartid = r_tp(); // hartid for cpuid()

    // 设置返回用户模式，打开中断
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP;
    x |= SSTATUS_SPIE;
    w_sstatus(x);

    // 更新 sepc
    w_sepc(p->trapframe->epc);

    // 调用 userret
    uint64 satp = MAKE_SATP(p->pagetable);
    uint64 fn = TRAMPOLINE + (userret - trampoline);
    ((void (*)(uint64, uint64))fn)(TRAPFRAME, satp);
}

// 处理中断
// 如果是软件中断、r_stval为 9（SBI外部中断转发）
// UART_IRQ 则 读取串口字符，添加到 consoleintr
// DISK_IRQ 则 disk_intr
// 如果是 S 模式定时器中断，则 timer_tick、yield
void kerneltrap()
{
    int which_dev = 0;
    uint64 sepc = r_sepc();       // 异常程序计数器
    uint64 sstatus = r_sstatus(); // 状态寄存器
    uint64 scause = r_scause();   // 异常原因

    // 确保中断是从 S 模式陷入
    if ((sstatus & SSTATUS_SPP) == 0)
    {
        panic("kerneltrap: not from supervisor mode");
    }
    // 全局中断是否被启用
    if (intr_get() != 0)
    {
        panic("kerneltrap: interrupts enabled");
    }

    if ((which_dev = devintr()) == 0)
    {
        printf("\nscause %p\n", scause);
        printf("sepc=%p stval=%p hart=%d\n", r_sepc(), r_stval(), r_tp());
        struct proc *p = myproc();
        if (p != 0)
        {
            printf("pid: %d, name: %s\n", p->pid, p->name);
        }
        panic("kerneltrap");
    }

    // 如果是定时器中断则调用 yield()
    if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    {
        yield();
    }

    w_sepc(sepc);
    w_sstatus(sstatus);
}

// 处理中断
// 如果是软件中断、r_stval为 9（SBI外部中断转发）
// UART_IRQ 则 读取串口字符，添加到 consoleintr
// DISK_IRQ 则 disk_intr
// 如果是 S 模式定时器中断，则 timer_tick
int devintr(void)
{
    uint64 scause = r_scause();

    if (0x8000000000000001L == scause && 9 == r_stval())
    {
        int irq = plic_claim();
        if (UART_IRQ == irq)
        {
            int c = sbi_console_getchar();
            if (-1 != c)
            {
                consoleintr(c);
            }
        }
        else if (DISK_IRQ == irq)
        {
            disk_intr();
        }
        else if (irq)
        {
            printf("unexpected interrupt irq = %d\n", irq);
        }

        if (irq)
        {
            plic_complete(irq);
        }
        w_sip(r_sip() & ~2);
        sbi_set_mie();
        return 1;
    }
    else if (0x8000000000000005L == scause)
    {
        timer_tick();
        return 2;
    }
    else
    {
        return 0;
    }
}

// 打印 trapframe 保存的寄存器
void trapframedump(struct trapframe *tf)
{
    printf("a0: %p\t", tf->a0);
    printf("a1: %p\t", tf->a1);
    printf("a2: %p\t", tf->a2);
    printf("a3: %p\n", tf->a3);
    printf("a4: %p\t", tf->a4);
    printf("a5: %p\t", tf->a5);
    printf("a6: %p\t", tf->a6);
    printf("a7: %p\n", tf->a7);
    printf("t0: %p\t", tf->t0);
    printf("t1: %p\t", tf->t1);
    printf("t2: %p\t", tf->t2);
    printf("t3: %p\n", tf->t3);
    printf("t4: %p\t", tf->t4);
    printf("t5: %p\t", tf->t5);
    printf("t6: %p\t", tf->t6);
    printf("s0: %p\n", tf->s0);
    printf("s1: %p\t", tf->s1);
    printf("s2: %p\t", tf->s2);
    printf("s3: %p\t", tf->s3);
    printf("s4: %p\n", tf->s4);
    printf("s5: %p\t", tf->s5);
    printf("s6: %p\t", tf->s6);
    printf("s7: %p\t", tf->s7);
    printf("s8: %p\n", tf->s8);
    printf("s9: %p\t", tf->s9);
    printf("s10: %p\t", tf->s10);
    printf("s11: %p\t", tf->s11);
    printf("ra: %p\n", tf->ra);
    printf("sp: %p\t", tf->sp);
    printf("gp: %p\t", tf->gp);
    printf("tp: %p\t", tf->tp);
    printf("epc: %p\n", tf->epc);
}
