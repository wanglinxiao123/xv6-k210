#include "include/param.h"
#include "include/types.h"
#include "include/memlayout.h"
#include "include/elf.h"
#include "include/riscv.h"
#include "include/vm.h"
#include "include/kalloc.h"
#include "include/proc.h"
#include "include/printf.h"
#include "include/string.h"

pagetable_t kernel_pagetable; // 内核根页表
extern char etext[];          // 内核代码结束地址
extern char trampoline[];     // trampoline.S

// 初始化内核页表，映射外设地址、内核段、数据段、TRAMPOLINE
void kvminit()
{
    kernel_pagetable = (pagetable_t)kalloc();
    memset(kernel_pagetable, 0, PGSIZE);

    // 映射外设地址
    kvmmap(UART_V, UART, PGSIZE, PTE_R | PTE_W);
    kvmmap(CLINT_V, CLINT, 0x10000, PTE_R | PTE_W);
    kvmmap(PLIC_V, PLIC, 0x4000, PTE_R | PTE_W);
    kvmmap(PLIC_V + 0x200000, PLIC + 0x200000, 0x4000, PTE_R | PTE_W);
    kvmmap(GPIOHS_V, GPIOHS, 0x1000, PTE_R | PTE_W);
    kvmmap(DMAC_V, DMAC, 0x1000, PTE_R | PTE_W);
    kvmmap(SPI_SLAVE_V, SPI_SLAVE, 0x1000, PTE_R | PTE_W);
    kvmmap(FPIOA_V, FPIOA, 0x1000, PTE_R | PTE_W);
    kvmmap(SPI0_V, SPI0, 0x1000, PTE_R | PTE_W);
    kvmmap(SPI1_V, SPI1, 0x1000, PTE_R | PTE_W);
    kvmmap(SPI2_V, SPI2, 0x1000, PTE_R | PTE_W);
    kvmmap(SYSCTL_V, SYSCTL, 0x1000, PTE_R | PTE_W);

    kvmmap(I2C0_V, I2C0, 0x1000, PTE_R | PTE_W);
    kvmmap(I2C1_V, I2C1, 0x1000, PTE_R | PTE_W);
    kvmmap(I2C2_V, I2C2, 0x1000, PTE_R | PTE_W);
    
    // 映射内核段、只读
    kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

    // 映射数据段
    kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

    // 统一的 trap 入口
    kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// 刷新页表寄存器
void kvminithart()
{
    w_satp(MAKE_SATP(kernel_pagetable));
    sfence_vma();
}

// 根页表为 pagetable，找到 va 对应的页表项物理地址
// alloc = 1 则分配空间
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc)
{
    if (va >= MAXVA)
    {
        panic("walk");
    }

    for (int level = 2; level > 0; level--)
    {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V)
        {
            pagetable = (pagetable_t)PTE2PA(*pte);
        }
        else
        {
            if (!alloc || (pagetable = (pde_t *)kalloc()) == NULL)
            {
                return NULL;
            }
            memset(pagetable, 0, PGSIZE);
            *pte = PA2PTE(pagetable) | PTE_V;
        }
    }

    return &pagetable[PX(0, va)];
}

// 从页表 pagetable 中找到 va 对应的 pa
// 页表项必须具备 PTE_V | PTE_U
uint64 walkaddr(pagetable_t pagetable, uint64 va)
{
    pte_t *pte;
    uint64 pa;

    if (va >= MAXVA)
    {
        return NULL;
    }

    pte = walk(pagetable, va, 0);
    if (pte == 0)
    {
        return NULL;
    }
    if ((*pte & PTE_V) == 0)
    {
        return NULL;
    }
    if ((*pte & PTE_U) == 0)
    {
        return NULL;
    }

    pa = PTE2PA(*pte);
    return pa;
}

// 将 (pa, size) 映射到 (kernel_pagetable, va, size)
// 设置 perm | PTE_V
// 调用 mappages()
void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
    if (mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    {
        panic("kvmmap");
    }
}

// 获取 (kernel_pagetable, va) 对应的 pa
uint64 kvmpa(uint64 va)
{
    return kwalkaddr(kernel_pagetable, va);
}

// 获取页表 (kpt, va) 对应的 pa
uint64 kwalkaddr(pagetable_t kpt, uint64 va)
{
    uint64 off = va % PGSIZE;
    pte_t *pte;
    uint64 pa;

    pte = walk(kpt, va, 0);
    if (pte == 0)
    {
        panic("kvmpa");
    }
    if ((*pte & PTE_V) == 0)
    {
        panic("kvmpa");
    }
    pa = PTE2PA(*pte);
    return pa + off;
}

// 将(pagetable, va, size) 映射到 (pa, size)
// 设置 PTE_V | perm
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
    uint64 a, last;
    pte_t *pte;
    a = PGROUNDDOWN(va);
    last = PGROUNDDOWN(va + size - 1);

    for (;;)
    {
        if ((pte = walk(pagetable, a, 1)) == NULL)
        {
            return -1;
        }

        if (*pte & PTE_V)
        {
            panic("remap");
        }

        *pte = PA2PTE(pa) | perm | PTE_V;
        if (a == last)
        {
            break;
        }

        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

// 取消(pagetable, va, npages) 的映射
// do_free = 1 则释放物理页面的空间
void vmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
    uint64 a;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
    {
        panic("vmunmap: not aligned");
    }

    for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
    {
        if ((pte = walk(pagetable, a, 0)) == 0)
        {
            panic("vmunmap: walk");
        }
        if ((*pte & PTE_V) == 0)
        {
            panic("vmunmap: not mapped");
        }
        if (PTE_FLAGS(*pte) == PTE_V)
        {
            panic("vmunmap: not a leaf");
        }
        if (do_free)
        {
            uint64 pa = PTE2PA(*pte);
            kfree((void *)pa);
        }
        *pte = 0;
    }
}

// 创建一个页表
pagetable_t uvmcreate()
{
    pagetable_t pagetable;
    pagetable = (pagetable_t)kalloc();
    if (pagetable == NULL)
    {
        return NULL;
    }
    memset(pagetable, 0, PGSIZE);
    return pagetable;
}

// 分配一页物理页，将 (src, sz) 拷贝到物理页中
// (pagetable, 0, sz) 和 (kpagetable, 0, sz) 都映射到该物理页
void uvminit(pagetable_t pagetable, pagetable_t kpagetable, uchar *src, uint sz)
{
    char *mem;

    if (sz >= PGSIZE)
    {
        panic("inituvm: more than a page");
    }

    mem = kalloc();
    memset(mem, 0, PGSIZE);
    mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
    mappages(kpagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X);
    memmove(mem, src, sz);
}

// 将 pagetable 和 kpagetable 的大小从 oldsz 增长到 newsz
// 分配页表的同时映射物理页
uint64 uvmalloc(pagetable_t pagetable, pagetable_t kpagetable, uint64 oldsz, uint64 newsz)
{
    char *mem;
    uint64 a;

    if (newsz < oldsz)
    {
        return oldsz;
    }

    oldsz = PGROUNDUP(oldsz);
    for (a = oldsz; a < newsz; a += PGSIZE)
    {
        mem = kalloc();
        if (mem == NULL)
        {
            uvmdealloc(pagetable, kpagetable, a, oldsz);
            return 0;
        }
        memset(mem, 0, PGSIZE);

        if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0)
        {
            kfree(mem);
            uvmdealloc(pagetable, kpagetable, a, oldsz);
            return 0;
        }

        if (mappages(kpagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R) != 0)
        {
            int npages = (a - oldsz) / PGSIZE;
            vmunmap(pagetable, oldsz, npages + 1, 1);
            vmunmap(kpagetable, oldsz, npages, 0);
            return 0;
        }
    }
    return newsz;
}

// 将 pagetable 和 kpagetable 的大小从 oldsz 减小到 newsz
// 对于 kpagetable 只取消映射、对于 pagetable 取消映射且释放物理页面
uint64 uvmdealloc(pagetable_t pagetable, pagetable_t kpagetable, uint64 oldsz, uint64 newsz)
{
    if (newsz >= oldsz)
    {
        return oldsz;
    }

    if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
    {
        int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        vmunmap(kpagetable, PGROUNDUP(newsz), npages, 0);
        vmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
    }

    return newsz;
}

// 递归删除页表 pagetable 所占用的空间
// 并不释放页表映射的物理页面
void freewalk(pagetable_t pagetable)
{
    for (int i = 0; i < 512; i++)
    {
        pte_t pte = pagetable[i];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
        {
            uint64 child = PTE2PA(pte);
            freewalk((pagetable_t)child);
            pagetable[i] = 0;
        }
        else if (pte & PTE_V)
        {
            panic("freewalk: leaf");
        }
    }

    kfree((void *)pagetable);
}

// 删除 (pagetable, sz) 的物理页映射，回收物理页内存
// 递归删除所有的页表
void uvmfree(pagetable_t pagetable, uint64 sz)
{
    if (sz > 0)
    {
        vmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
    }

    freewalk(pagetable);
}

// 根据 (old, sz) 申请新的物理页，并填充同样的数据
// 将这片物理页映射到页表 new 和 knew
int uvmcopy(pagetable_t old, pagetable_t new, pagetable_t knew, uint64 sz)
{
    pte_t *pte;
    uint64 pa, i = 0, ki = 0;
    uint flags;
    char *mem;

    while (i < sz)
    {
        if ((pte = walk(old, i, 0)) == NULL)
        {
            panic("uvmcopy: pte should exist");
        }

        if ((*pte & PTE_V) == 0)
        {
            panic("uvmcopy: page not present");
        }

        pa = PTE2PA(*pte);
        flags = PTE_FLAGS(*pte);
        if ((mem = kalloc()) == NULL)
        {
            goto err;
        }

        memmove(mem, (char *)pa, PGSIZE);
        if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0)
        {
            kfree(mem);
            goto err;
        }

        i += PGSIZE;
        if (mappages(knew, ki, PGSIZE, (uint64)mem, flags & ~PTE_U) != 0)
        {
            goto err;
        }
        ki += PGSIZE;
    }
    return 0;

err:
    vmunmap(knew, 0, ki / PGSIZE, 0);
    vmunmap(new, 0, i / PGSIZE, 1);
    return -1;
}

// 删除 (pagetable, va) 页表项对应的 PTE_U
void uvmclear(pagetable_t pagetable, uint64 va)
{
    pte_t *pte;
    pte = walk(pagetable, va, 0);
    if (pte == NULL)
    {
        panic("uvmclear");
    }
    *pte &= ~PTE_U;
}

// 将 (src, len) 拷贝到 (pagetable, dstva, len)
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
    uint64 n, va0, pa0;
    while (len > 0)
    {
        va0 = PGROUNDDOWN(dstva);
        pa0 = walkaddr(pagetable, va0);

        if (pa0 == NULL)
        {
            return -1;
        }

        n = PGSIZE - (dstva - va0);
        if (n > len)
        {
            n = len;
        }

        memmove((void *)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
    return 0;
}

// 将 (src, len) 拷贝到 (dstva, len)
int copyout2(uint64 dstva, char *src, uint64 len)
{
    uint64 sz = myproc()->sz;
    if (dstva + len > sz || dstva >= sz)
    {
        return -1;
    }
    memmove((void *)dstva, src, len);
    return 0;
}

// 将 (pagetable, srcva, len) 拷贝到 (dst, len)
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
    uint64 n, va0, pa0;

    while (len > 0)
    {
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == NULL)
        {
            return -1;
        }

        n = PGSIZE - (srcva - va0);
        if (n > len)
        {
            n = len;
        }

        memmove(dst, (void *)(pa0 + (srcva - va0)), n);

        len -= n;
        dst += n;
        srcva = va0 + PGSIZE;
    }
    return 0;
}

// 将 (srcva, len) 拷贝到 (dst, len)
int copyin2(char *dst, uint64 srcva, uint64 len)
{
    uint64 sz = myproc()->sz;
    if (srcva + len > sz || srcva >= sz)
    {
        return -1;
    }
    memmove(dst, (void *)srcva, len);
    return 0;
}

// 将 (pagetable, srcva, max) 拷贝到 (dst, max), 遇到 '\0' 停止
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
    uint64 n, va0, pa0;
    int got_null = 0;

    while (got_null == 0 && max > 0)
    {
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == NULL)
        {
            return -1;
        }

        n = PGSIZE - (srcva - va0);
        if (n > max)
        {
            n = max;
        }

        char *p = (char *)(pa0 + (srcva - va0));
        while (n > 0)
        {
            if (*p == '\0')
            {
                *dst = '\0';
                got_null = 1;
                break;
            }
            else
            {
                *dst = *p;
            }

            --n;
            --max;
            p++;
            dst++;
        }
        srcva = va0 + PGSIZE;
    }

    if (got_null)
    {
        return 0;
    }
    else
    {
        return -1;
    }
}

// 将 (srcva, max) 拷贝到 (dst, max)，遇到 '\0' 停止
int copyinstr2(char *dst, uint64 srcva, uint64 max)
{
    int got_null = 0;
    uint64 sz = myproc()->sz;
    while (srcva < sz && max > 0)
    {
        char *p = (char *)srcva;
        if (*p == '\0')
        {
            *dst = '\0';
            got_null = 1;
            break;
        }
        else
        {
            *dst = *p;
        }
        --max;
        srcva++;
        dst++;
    }
    if (got_null)
    {
        return 0;
    }
    else
    {
        return -1;
    }
}

// 创建一个页表 kpt, 和内核页表指向同样的二级页表
// 分配一个物理页，映射到 (kpt, VKSTACK, PGSIZE)
pagetable_t proc_kpagetable()
{
    pagetable_t kpt = (pagetable_t)kalloc();
    if (kpt == NULL)
    {
        return NULL;
    }

    memmove(kpt, kernel_pagetable, PGSIZE);

    char *pstack = kalloc();
    if (pstack == NULL)
    {
        goto fail;
    }

    if (mappages(kpt, VKSTACK, PGSIZE, (uint64)pstack, PTE_R | PTE_W) != 0)
    {
        goto fail;
    }

    return kpt;

fail:
    kvmfree(kpt, 1);
    return NULL;
}

// 释放页表 kpt 占用的空间，但不释放物理页面
void kfreewalk(pagetable_t kpt)
{
    for (int i = 0; i < 512; i++)
    {
        pte_t pte = kpt[i];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
        {
            kfreewalk((pagetable_t)PTE2PA(pte));
            kpt[i] = 0;
        }
        else if (pte & PTE_V)
        {
            break;
        }
    }
    kfree((void *)kpt);
}

// 将 kpt 的页表项写0，并释放指向子页表占用的空间，但不释放物理页面
void kvmfreeusr(pagetable_t kpt)
{
    pte_t pte;
    for (int i = 0; i < PX(2, MAXUVA); i++)
    {
        pte = kpt[i];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
        {
            kfreewalk((pagetable_t)PTE2PA(pte));
            kpt[i] = 0;
        }
    }
}

// stack_free = 1，则释放 (kpt, VKSTACK, 1 page)，回收物理页面
// 释放 kpt 页表本身占用的空间，但不释放物理页面
void kvmfree(pagetable_t kpt, int stack_free)
{
    if (stack_free)
    {
        vmunmap(kpt, VKSTACK, 1, 1);
        pte_t pte = kpt[PX(2, VKSTACK)];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
        {
            kfreewalk((pagetable_t)PTE2PA(pte));
        }
    }

    kvmfreeusr(kpt);
    kfree(kpt);
}

// 打印页表
void vmprint(pagetable_t pagetable)
{
    const int capacity = 512;
    printf("page table %p\n", pagetable);
    for (pte_t *pte = (pte_t *)pagetable; pte < pagetable + capacity; pte++)
    {
        if (*pte & PTE_V)
        {
            pagetable_t pt2 = (pagetable_t)PTE2PA(*pte);
            printf("..%d: pte %p pa %p\n", pte - pagetable, *pte, pt2);

            for (pte_t *pte2 = (pte_t *)pt2; pte2 < pt2 + capacity; pte2++)
            {
                if (*pte2 & PTE_V)
                {
                    pagetable_t pt3 = (pagetable_t)PTE2PA(*pte2);
                    printf(".. ..%d: pte %p pa %p\n", pte2 - pt2, *pte2, pt3);

                    for (pte_t *pte3 = (pte_t *)pt3; pte3 < pt3 + capacity; pte3++)
                        if (*pte3 & PTE_V)
                            printf(".. .. ..%d: pte %p pa %p\n", pte3 - pt3, *pte3, PTE2PA(*pte3));
                }
            }
        }
    }
    return;
}