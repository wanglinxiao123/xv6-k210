#ifndef __PROC_H
#define __PROC_H

#include "param.h"
#include "riscv.h"
#include "types.h"
#include "spinlock.h"
#include "file.h"
#include "fat32.h"
#include "trap.h"

// Saved registers for kernel context switches.
struct context
{
    uint64 ra;
    uint64 sp; // 内核的堆栈指针

    // callee-saved
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
};

struct cpu
{
    struct proc *proc;      // cpu 运行的进程
    struct context context; // 用于 swtch 切换 scheduler
    int noff;               // push_off 的深度
    int intena;             // push_off 前中断是否被打开
};

extern struct cpu cpus[NCPU];

enum procstate
{
    UNUSED,
    SLEEPING,
    RUNNABLE,
    RUNNING,
    ZOMBIE
};

// Per-process state
struct proc
{
    struct spinlock lock;

    // 自旋锁保护如下成员
    enum procstate state; // 进程状态
    struct proc *parent;  // 指向父进程
    void *chan;           // 等待在 chan 上
    int killed;           // If non-zero, have been killed
    int xstate;           // 进程退出时的状态码
    int pid;              // 进程 ID

    // these are private to the process, so p->lock need not be held.
    uint64 kstack;               // 内核堆栈的虚拟指针
    uint64 sz;                   // 进程的用户空间
    pagetable_t pagetable;       // User page table
    pagetable_t kpagetable;      // Kernel page table
    struct trapframe *trapframe; // trapframe 结构体
    struct context context;      // swtch() 对应的上下文
    struct file *ofile[NOFILE];  // 进程打开的文件
    struct dirent *cwd;          // Current directory
    char name[16];               // 进程名称
    int tmask;                   // trace 掩码
};

void reg_info(void);
int cpuid(void);
void exit(int);
int fork(void);
int growproc(int);
pagetable_t proc_pagetable(struct proc *);
void proc_freepagetable(pagetable_t, uint64);
int kill(int);
struct cpu *mycpu(void);
struct cpu *getmycpu(void);
struct proc *myproc();
void procinit(void);
void scheduler(void) __attribute__((noreturn));
void sched(void);
void setproc(struct proc *);
void sleep(void *, struct spinlock *);
void userinit(void);
int wait(uint64);
void wakeup(void *);
void yield(void);
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void procdump(void);
uint64 procnum(void);
void test_proc_init(int);

#endif