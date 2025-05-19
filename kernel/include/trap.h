#ifndef __TRAP_H
#define __TRAP_H

struct trapframe
{
  // T 为临时寄存器，函数调用时不需要保存
  // S 为保存寄存器，跨函数调用保持不变
  // A 为参数寄存器，用于传递函数参数
  /*   0 */ uint64 kernel_satp;   // 当前内核页表的地址
  /*   8 */ uint64 kernel_sp;     // 进程的内核栈顶地址
  /*  16 */ uint64 kernel_trap;   // 用户态进入到内核是要执行的 trap() 处理函数
  /*  24 */ uint64 epc;           // 用户态 PC
  /*  32 */ uint64 kernel_hartid; // cpu核心ID
  
  /*  40 */ uint64 ra;            // 返回地址寄存器
  /*  48 */ uint64 sp;            // 用户态栈指针
  /*  56 */ uint64 gp;            // 全局变量的基址
  /*  64 */ uint64 tp;            // 线程指针
  
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

void            trapinithart(void);
void            usertrapret(void);
void            trapframedump(struct trapframe *tf);

#endif