#include <stdarg.h>

#include "include/types.h"
#include "include/param.h"
#include "include/riscv.h"
#include "include/spinlock.h"
#include "include/console.h"
#include "include/printf.h"

volatile int panicked = 0;

static char digits[] = "0123456789abcdef";

// 用于串行化 printf
static struct
{
    struct spinlock lock;
    int locking; // 是否使用自旋锁机制
} pr;

// 串口输出字符串
void printstring(const char *s)
{
    while (*s)
    {
        consputc(*s++);
    }
}

// 初始化 printf 用到的自旋锁
void printfinit(void)
{
    initlock(&pr.lock, "pr");
    pr.locking = 1; // changed, used to be 1
}

// 格式化输出 int
static void printint(int xx, int base, int sign)
{
    char buf[16];
    int i;
    uint x;

    if (sign && (sign = xx < 0))
    {
        x = -xx;
    }
    else
    {
        x = xx;
    }

    i = 0;

    do
    {
        buf[i++] = digits[x % base];
    } while ((x /= base) != 0);

    if (sign)
    {
        buf[i++] = '-';
    }

    while (--i >= 0)
    {
        consputc(buf[i]);
    }
}

// 格式化输出 uint64 指针
static void printptr(uint64 x)
{
    int i;
    consputc('0');
    consputc('x');
    for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    {
        consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
    }
}

// 格式化输出，只支持 %d, %x, %p, %s.
void printf(char *fmt, ...)
{
    va_list ap;
    int i, c;
    int locking;
    char *s;

    locking = pr.locking;
    if (locking)
        acquire(&pr.lock);

    if (fmt == 0)
        panic("null fmt");

    va_start(ap, fmt);
    for (i = 0; (c = fmt[i] & 0xff) != 0; i++)
    {
        if (c != '%')
        {
            consputc(c);
            continue;
        }
        c = fmt[++i] & 0xff;
        if (c == 0)
            break;
        switch (c)
        {
        case 'd':
            printint(va_arg(ap, int), 10, 1);
            break;
        case 'x':
            printint(va_arg(ap, int), 16, 1);
            break;
        case 'p':
            printptr(va_arg(ap, uint64));
            break;
        case 's':
            if ((s = va_arg(ap, char *)) == 0)
                s = "(null)";
            for (; *s; s++)
                consputc(*s);
            break;
        case '%':
            consputc('%');
            break;
        default:
            // Print unknown % sequence to draw attention.
            consputc('%');
            consputc(c);
            break;
        }
    }
    if (locking)
        release(&pr.lock);
}

// 触发异常的情况
void panic(char *s)
{
    printf("panic: ");
    printf(s);
    printf("\n");
    backtrace();
    panicked = 1;
    for (;;)
        ;
}

// 回溯堆栈
void backtrace()
{
    uint64 *fp = (uint64 *)r_fp();
    uint64 *bottom = (uint64 *)PGROUNDUP((uint64)fp);

    printf("backtrace:\n");
    while (fp < bottom)
    {
        uint64 ra = *(fp - 1);    // 返回地址
        printf("%p\n", ra - 4);   // 打印返回地址
        fp = (uint64 *)*(fp - 2); // 上一个栈帧
    }
}

// 打印 LOGO
void print_logo()
{
    printf("WELCOME TO XV6-K210\n");
}
