
#include "include/types.h"
#include "include/param.h"
#include "include/memlayout.h"
#include "include/riscv.h"
#include "include/spinlock.h"
#include "include/proc.h"
#include "include/intr.h"
#include "include/kalloc.h"
#include "include/printf.h"
#include "include/string.h"
#include "include/fat32.h"
#include "include/file.h"
#include "include/trap.h"
#include "include/vm.h"

struct cpu cpus[NCPU];
struct proc proc[NPROC];
struct proc *initproc; // 指向 init 线程

// PID 及保存的自旋锁
int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
extern void swtch(struct context *, struct context *);
static void wakeup1(struct proc *chan);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

void reg_info(void)
{
    printf("register info: {\n");
    printf("sstatus: %p\n", r_sstatus());
    printf("sip: %p\n", r_sip());
    printf("sie: %p\n", r_sie());
    printf("sepc: %p\n", r_sepc());
    printf("stvec: %p\n", r_stvec());
    printf("satp: %p\n", r_satp());
    printf("scause: %p\n", r_scause());
    printf("stval: %p\n", r_stval());
    printf("sp: %p\n", r_sp());
    printf("tp: %p\n", r_tp());
    printf("ra: %p\n", r_ra());
    printf("}\n");
}

// 初始化保护 PID 和每个 proc 的自旋锁
void procinit(void)
{
    memset(proc, 0, sizeof(proc));
    struct proc *p;
    initlock(&pid_lock, "nextpid");

    for (p = proc; p < &proc[NPROC]; p++)
    {
        initlock(&p->lock, "proc");
    }

    memset(cpus, 0, sizeof(cpus));
}

// 获取 CPU 核心，必须在中断禁用时调用
int cpuid()
{
    int id = r_tp();
    return id;
}

int mycpuid(void)
{
    push_off();
    int id = cpuid();
    pop_off();
    return id;
}

// 获取 CPU 结构体，必须在中断禁用时调用
struct cpu *mycpu(void)
{
    int id = cpuid();
    struct cpu *c = &cpus[id];
    return c;
}

// 获取当前 CPU 运行的 proc
struct proc *myproc(void)
{
    push_off();
    struct cpu *c = mycpu();
    struct proc *p = c->proc;
    pop_off();
    return p;
}

// 获取一个 PID
int allocpid()
{
    int pid;
    acquire(&pid_lock);
    pid = nextpid;
    nextpid = nextpid + 1;
    release(&pid_lock);
    return pid;
}

// 找到 UNUSED 的进程 p
// 为 p 分配用户页表和内核页表、内核堆栈
// 用户页表映射 TRAMPOLINE 和 TRAPFRAME
// 内核页表与原内核页表相同，但添加 VKSTACK 映射
// 初始化 swich 对应的上下文
static struct proc *allocproc(void)
{
    // 找到 UNUSED 的进程
    struct proc *p;
    for (p = proc; p < &proc[NPROC]; p++)
    {
        acquire(&p->lock);
        if (p->state == UNUSED)
        {
            goto found;
        }
        else
        {
            release(&p->lock);
        }
    }
    return NULL;

found:
    p->pid = allocpid();

    // 分配 trapframe 内存
    if ((p->trapframe = (struct trapframe *)kalloc()) == NULL)
    {
        release(&p->lock);
        return NULL;
    }

    // 为进程 p 创建一个新页表，映射 TRAMPOLINE 和 TRAPFRAME
    // 为进程 p 创建一个唯一的的内核页表，与内核页表指向同样的值，并添加 VKSTACK 映射
    // 分配一个物理页，映射到 (kpt, VKSTACK, PGSIZE)
    if ((p->pagetable = proc_pagetable(p)) == NULL || (p->kpagetable = proc_kpagetable()) == NULL)
    {
        freeproc(p);
        release(&p->lock);
        return NULL;
    }

    p->kstack = VKSTACK;

    // 初始化 swich 对应的上下文
    memset(&p->context, 0, sizeof(p->context));
    p->context.ra = (uint64)forkret;
    p->context.sp = p->kstack + PGSIZE;

    return p;
}

// 释放 p 占用的所有资源
// p->trapframe、内核栈、内核页表本身
// 用户页和其映射的物理空间
static void freeproc(struct proc *p)
{
    if (p->trapframe)
    {
        kfree((void *)p->trapframe);
    }
    p->trapframe = 0;

    // 释放内核页表占用的空间、释放内核栈
    if (p->kpagetable)
    {
        kvmfree(p->kpagetable, 1);
    }
    p->kpagetable = 0;

    // 释放用户页表和其映射的物理页
    if (p->pagetable)
    {
        proc_freepagetable(p->pagetable, p->sz);
    }

    p->pagetable = 0;
    p->sz = 0;
    p->pid = 0;
    p->parent = 0;
    p->name[0] = 0;
    p->chan = 0;
    p->killed = 0;
    p->xstate = 0;
    p->state = UNUSED;
}

// 创建一个新页表，映射 TRAMPOLINE 和 TRAPFRAME
pagetable_t proc_pagetable(struct proc *p)
{
    // 创建一个页表
    pagetable_t pagetable;
    pagetable = uvmcreate();
    if (pagetable == 0)
    {
        return NULL;
    }

    // 映射 TRAMPOLINE
    if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) < 0)
    {
        uvmfree(pagetable, 0);
        return NULL;
    }

    // 映射 TRAPFRAME
    if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
    {
        vmunmap(pagetable, TRAMPOLINE, 1, 0);
        uvmfree(pagetable, 0);
        return NULL;
    }

    return pagetable;
}

// 释放页表和其映射的物理页
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
    vmunmap(pagetable, TRAMPOLINE, 1, 0);
    vmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// 为 init 进程分配资源、映射物理页面到 pagetable 和 kpagetable
void userinit(void)
{
    // 为 init 线程分配资源
    struct proc *p;
    p = allocproc();
    initproc = p;

    // 分配一页物理页，将 initcode 程序拷贝到物理页中
    // (pagetable, 0, sz) 和 (kpagetable, 0, sz) 都映射到该物理页
    uvminit(p->pagetable, p->kpagetable, initcode, sizeof(initcode));
    p->sz = PGSIZE;

    // 设置 PC 和 sp
    p->trapframe->epc = 0x0;   // user program counter
    p->trapframe->sp = PGSIZE; // user stack pointer

    safestrcpy(p->name, "initcode", sizeof(p->name));

    p->state = RUNNABLE;
    p->tmask = 0;
    release(&p->lock);
}

// 增长或收缩进程的 用户页表和内核页表
// 对于 kpagetable 只取消映射、对于 pagetable 取消映射且释放物理页面
int growproc(int n)
{
    uint sz;
    struct proc *p = myproc();
    sz = p->sz;

    if (n > 0)
    {
        if ((sz = uvmalloc(p->pagetable, p->kpagetable, sz, sz + n)) == 0)
        {
            return -1;
        }
    }

    else if (n < 0)
    {
        sz = uvmdealloc(p->pagetable, p->kpagetable, sz, sz + n);
    }

    p->sz = sz;
    return 0;
}

// 从旧进程 fork 一个新进程
int fork(void)
{
    int i, pid;
    struct proc *np;
    struct proc *p = myproc();

    // 申请进程空间
    if ((np = allocproc()) == NULL)
    {
        return -1;
    }

    // 拷贝 当前进程页表 到 新进程
    if (uvmcopy(p->pagetable, np->pagetable, np->kpagetable, p->sz) < 0)
    {
        freeproc(np);
        release(&np->lock);
        return -1;
    }
    np->sz = p->sz;

    np->parent = p;
    np->tmask = p->tmask;

    // 拷贝 trapframe
    *(np->trapframe) = *(p->trapframe);

    // Cause fork to return 0 in the child.
    np->trapframe->a0 = 0;

    // increment reference counts on open file descriptors.
    for (i = 0; i < NOFILE; i++)
        if (p->ofile[i])
            np->ofile[i] = filedup(p->ofile[i]);
    np->cwd = edup(p->cwd);

    safestrcpy(np->name, p->name, sizeof(p->name));
    pid = np->pid;
    np->state = RUNNABLE;

    release(&np->lock);
    return pid;
}

// 将 p 进程的子进程交由 init 进程管理
void reparent(struct proc *p)
{
    struct proc *pp;

    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
        if (pp->parent == p)
        {
            acquire(&pp->lock);
            pp->parent = initproc;
            release(&pp->lock);
        }
    }
}

// 关闭文件描述符
// 将 该进程的子进程 交由 init 进程管理
// 调用 sched 进入调度器
// 唤醒 initproc 和 该进程的父进程
void exit(int status)
{
    struct proc *p = myproc();

    if (p == initproc)
    {
        panic("init exiting");
    }

    // 关闭当前进程打开的文件
    for (int fd = 0; fd < NOFILE; fd++)
    {
        if (p->ofile[fd])
        {
            struct file *f = p->ofile[fd];
            fileclose(f);
            p->ofile[fd] = 0;
        }
    }

    eput(p->cwd);
    p->cwd = 0;

    // 唤醒 initproc
    acquire(&initproc->lock);
    wakeup1(initproc);
    release(&initproc->lock);

    acquire(&p->lock);
    struct proc *original_parent = p->parent;
    release(&p->lock);

    // 将进程交由 init 进程管理
    acquire(&original_parent->lock);
    acquire(&p->lock);
    reparent(p);
    wakeup1(original_parent);

    p->xstate = status;
    p->state = ZOMBIE;

    release(&original_parent->lock);

    // 进入调度器
    sched();

    panic("zombie exit");
}

// 等待一个子进程退出，转换为僵尸进程后回收他的资源
// 如果子进程均没有退出，就阻塞等待
// 如果没有子进程，则返回 -1
int wait(uint64 addr)
{
    struct proc *np;
    int havekids, pid;
    struct proc *p = myproc();

    acquire(&p->lock);

    for (;;)
    {
        havekids = 0;
        for (np = proc; np < &proc[NPROC]; np++)
        {
            if (np->parent == p)
            {
                acquire(&np->lock);
                havekids = 1;

                // 如果找到了僵尸进程，就回收他的空间，并返回 PID
                if (np->state == ZOMBIE)
                {
                    pid = np->pid;

                    if (addr != 0 && copyout2(addr, (char *)&np->xstate, sizeof(np->xstate)) < 0)
                    {
                        release(&np->lock);
                        release(&p->lock);
                        return -1;
                    }

                    freeproc(np);
                    release(&np->lock);
                    release(&p->lock);
                    return pid;
                }

                release(&np->lock);
            }
        }

        // 没有子进程，则返回 -1
        if (!havekids || p->killed)
        {
            release(&p->lock);
            return -1;
        }

        // 阻塞在自身
        sleep(p, &p->lock);
    }
}

// scheduler 循环寻找 RUNNABLE 的资源并进行调度
void scheduler(void)
{
    struct proc *p;
    struct cpu *c = mycpu();
    extern pagetable_t kernel_pagetable;

    c->proc = 0;
    for (;;)
    {
        intr_on();

        int found = 0;
        for (p = proc; p < &proc[NPROC]; p++)
        {
            acquire(&p->lock);
            if (p->state == RUNNABLE)
            {
                p->state = RUNNING;
                c->proc = p;

                // 刷新页表为 p->kpagetable
                w_satp(MAKE_SATP(p->kpagetable));
                sfence_vma();

                swtch(&c->context, &p->context);

                // 加载回内核页表
                w_satp(MAKE_SATP(kernel_pagetable));
                sfence_vma();

                c->proc = 0;
                found = 1;
            }
            release(&p->lock);
        }

        if (found == 0)
        {
            intr_on();
            asm volatile("wfi");
        }
    }
}

// 将当前 CPU 从 myproc 切换到 mycpu()->context
void sched(void)
{
    int intena;
    struct proc *p = myproc();

    if (!holding(&p->lock))
    {
        panic("sched p->lock");
    }

    if (mycpu()->noff != 1)
    {
        panic("sched locks");
    }

    if (p->state == RUNNING)
    {
        panic("sched running");
    }

    if (intr_get())
    {
        panic("sched interruptible");
    }

    intena = mycpu()->intena;
    swtch(&p->context, &mycpu()->context);
    mycpu()->intena = intena;
}

// 切换 CPU 进程
void yield(void)
{
    struct proc *p = myproc();
    acquire(&p->lock);
    p->state = RUNNABLE;
    sched();
    release(&p->lock);
}

// fork 出来子进程的第一次 swtch 执行的函数
// 执行 usertrapret 返回用户空间
void forkret(void)
{
    static int first = 1;
    release(&myproc()->lock);

    if (first)
    {
        // File system initialization must be run in the context of a
        // regular process (e.g., because it calls sleep), and thus cannot
        // be run from main().
        // printf("[forkret]first scheduling\n");
        first = 0;
        fat32_init();
        myproc()->cwd = ename("/");
    }

    usertrapret();
}

// 将当前线程阻塞在 chan，并释放 lk，在唤醒时重新持有
void sleep(void *chan, struct spinlock *lk)
{
    struct proc *p = myproc();

    // 释放lk
    if (lk != &p->lock)
    {
        acquire(&p->lock);
        release(lk);
    }

    // 标记为调度状态
    p->chan = chan;
    p->state = SLEEPING;

    sched();

    // 清除阻塞进程的 chan
    p->chan = 0;

    // 重新获取lk
    if (lk != &p->lock)
    {
        release(&p->lock);
        acquire(lk);
    }
}

// 将等待在 chan 上的进程唤醒
void wakeup(void *chan)
{
    struct proc *p;

    for (p = proc; p < &proc[NPROC]; p++)
    {
        acquire(&p->lock);
        if (p->state == SLEEPING && p->chan == chan)
        {
            p->state = RUNNABLE;
        }
        release(&p->lock);
    }
}

// 如果进程交由自身等待，则将其唤醒
static void wakeup1(struct proc *p)
{
    if (!holding(&p->lock))
    {
        panic("wakeup1");
    }
    if (p->chan == p && p->state == SLEEPING)
    {
        p->state = RUNNABLE;
    }
}

// 标记 pid 对应的 proc 为 killed状态
int kill(int pid)
{
    struct proc *p;

    for (p = proc; p < &proc[NPROC]; p++)
    {
        acquire(&p->lock);
        if (p->pid == pid)
        {
            p->killed = 1;
            if (p->state == SLEEPING)
            {
                p->state = RUNNABLE;
            }
            release(&p->lock);
            return 0;
        }
        release(&p->lock);
    }
    return -1;
}

// 将 (src, len) 拷贝到 (dstva, len)
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
    // struct proc *p = myproc();
    if (user_dst)
    {
        // return copyout(p->pagetable, dst, src, len);
        return copyout2(dst, src, len);
    }
    else
    {
        memmove((char *)dst, src, len);
        return 0;
    }
}

// 将 (srcva, len) 拷贝到 (dst, len)
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
    // struct proc *p = myproc();
    if (user_src)
    {
        // return copyin(p->pagetable, dst, src, len);
        return copyin2(dst, src, len);
    }
    else
    {
        memmove(dst, (char *)src, len);
        return 0;
    }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
    static char *states[] = {
        [UNUSED] "unused",
        [SLEEPING] "sleep ",
        [RUNNABLE] "runble",
        [RUNNING] "run   ",
        [ZOMBIE] "zombie"};
    struct proc *p;
    char *state;

    printf("\nPID\tSTATE\tNAME\tMEM\n");
    for (p = proc; p < &proc[NPROC]; p++)
    {
        if (p->state == UNUSED)
            continue;
        if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
            state = states[p->state];
        else
            state = "???";
        printf("%d\t%s\t%s\t%d", p->pid, state, p->name, p->sz);
        printf("\n");
    }
}

// 打印 UNUSED 的进程数
uint64 procnum(void)
{
    int num = 0;
    struct proc *p;
    for (p = proc; p < &proc[NPROC]; p++)
    {
        if (p->state != UNUSED)
        {
            num++;
        }
    }
    return num;
}
