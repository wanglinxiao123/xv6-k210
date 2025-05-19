//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//
#include <stdarg.h>

#include "include/types.h"
#include "include/param.h"
#include "include/spinlock.h"
#include "include/sleeplock.h"
#include "include/file.h"
#include "include/memlayout.h"
#include "include/riscv.h"
#include "include/proc.h"
#include "include/sbi.h"

#define INPUT_BUF 128

#define BACKSPACE 0x100
#define C(x) ((x) - '@') // Control-x

// 串口输出一个字符
void consputc(int c)
{
    if (c == BACKSPACE)
    {
        sbi_console_putchar('\b');
        sbi_console_putchar(' ');
        sbi_console_putchar('\b');
    }
    else
    {
        sbi_console_putchar(c);
    }
}

struct
{
    struct spinlock lock;
    char buf[INPUT_BUF];
    uint r; // 读指针
    uint w; // 写指针
    uint e; // 编辑指针
} cons;

// 将内核或者用户空间的(src, n)输出到控制台
// user_src = 1 则为用户空间
int consolewrite(int user_src, uint64 src, int n)
{
    int i;
    acquire(&cons.lock);
    for (i = 0; i < n; i++)
    {
        char c;
        if (either_copyin(&c, user_src, src + i, 1) == -1)
            break;
        sbi_console_putchar(c);
    }
    release(&cons.lock);
    return i;
}

// 从控制台读取 n 个数据到(dst, n)
// 没有数据时将会被阻塞
// user_dst = 1 则为用户空间
// 遇到文件末尾、换行符则返回
int consoleread(int user_dst, uint64 dst, int n)
{
    uint target;
    int c;
    char cbuf;

    target = n;

    acquire(&cons.lock);
    while (n > 0)
    {
        // 等待控制台输入数据
        while (cons.r == cons.w)
        {
            if (myproc()->killed)
            {
                release(&cons.lock);
                return -1;
            }
            sleep(&cons.r, &cons.lock);
        }

        // 获取一个字符
        c = cons.buf[cons.r++ % INPUT_BUF];

        // 如果读到了 ^D 就回退一个
        if (c == C('D'))
        {
            if (n < target)
            {
                cons.r--;
            }
            break;
        }

        // 从缓冲区拿一个字符
        cbuf = c;
        if (either_copyout(user_dst, dst, &cbuf, 1) == -1)
            break;

        dst++;
        --n;

        // 遇到换行符就弹出
        if (c == '\n')
        {
            break;
        }
    }
    release(&cons.lock);

    return target - n;
}

// 处理串口输入的一个进程
// CTRL+P 打印进程列表
// CTRL+U 删除整行
// 回车回退一个字符
// 其他情况将字符填入缓冲区，遇见换行、文件末尾则更新写指针、唤醒进程
void consoleintr(int c)
{
    acquire(&cons.lock);

    switch (c)
    {
    // CTRL+P 打印进程列表
    case C('P'):
        procdump();
        break;

    // CTRL+U 删除整行
    case C('U'):
        while (cons.e != cons.w && cons.buf[(cons.e - 1) % INPUT_BUF] != '\n')
        {
            cons.e--;
            consputc(BACKSPACE);
        }
        break;

    // 回车回退一个字符
    case C('H'): // Backspace
    case '\x7f':
        if (cons.e != cons.w)
        {
            cons.e--;
            consputc(BACKSPACE);
        }
        break;

    
    default:
        if (c != 0 && cons.e - cons.r < INPUT_BUF)
        {
            // 遇到换行则退出
            if (c == '\r')
                break;

            // 回显给用户控制台
            consputc(c);

            // 将读取到的字符添加到缓冲区
            cons.buf[cons.e++ % INPUT_BUF] = c;

            // 遇到换行、文件末尾、缓冲区满则更新写指针，唤醒进程
            if (c == '\n' || c == C('D') || cons.e == cons.r + INPUT_BUF)
            {
                cons.w = cons.e;
                wakeup(&cons.r);
            }
        }
        break;
    }

    release(&cons.lock);
}

// 初始化自旋锁
// 关联读写系统调用
void consoleinit(void)
{
    initlock(&cons.lock, "cons");
    cons.e = cons.w = cons.r = 0;
    devsw[CONSOLE].read = consoleread;
    devsw[CONSOLE].write = consolewrite;
}
