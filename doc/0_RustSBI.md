# RustSBI做什么
- 主要是三点：初始化系统资源（串口、时钟）、为操作系统提供服务（中断转发、系统调用、异常处理）、跳转到操作系统内核`0x80020000`
### 具体来说
- 设置堆栈指针
- 设置异常处理函数
- 初始化时钟系统、串口等外设
- 初始化中断、设置中断委托
- `mepc`跳转地址
- 中断处理函数中实现中断转发
- 提供SBI系统调用，由内核态执行`ecall`
- 没有`S`态的外部中断，改为产生`S`态的软件中断
- 适配一些异常指令，比如刷新页表指令版本不同

# 总结
## 进行初始化操作
### 公共操作
1. 设置堆栈指针
2. 设置异常或中断时跳入 _start_trap (mtvec)
### 主核操作
3. 初始化堆分配器
4. 初始化时钟系统
5. 配置串口、绑定标准输入输出
6. 注册 RustSBI框架的 IPI 实现（msip::set_ipi、msip::clear_ipi）
7. 初始化定时器循环调用（mtimecmp）
8. 注册处理重启请求、直接写死
9. 配置优先级 >= 0 的中断都可以被触发
### 公共操作
10. 委托M定时器中断、软件中断给S
11. 委托M模式异常给S模式
12. 使能M软件中断
13. 设置 mepc 为 _s_mode_start，mstatus 的 MPP 设置为 S模式，mret 进入 S 模式跳转到 _s_mode_start
14. _s_mode_start 中跳转到 0x80020000
## 异常或中断处理函数
### 汇编函数
- mscratch 在陷入中断前后，保存中断时的 SP
- mscratch 在中断处理期间，保存陷入中断时原先的 SP
- 在汇编函数中保存和恢复 ra、以及通用寄存器
### rust 中断处理函数
#### S 模式下执行 ecall
##### sbi_set_mie
- 使能 M 外部中断、M 定时器中断
- 返回 (SBI_SUCCESS, 0)
##### sbi_console_putchar
- 串口输出 a0 存放的字符
##### sbi_console_getchar
- 将串口输入的字符存放到 a0
#### 中断转发
- M 软件中断转发到 S 软件中断
- M 定时器中断转发到 S 定时器中断，禁止 M 外部中断、 M 定时器中断
- M 模式外部中断转发到 S 软件中断，stval 设置为 9，禁止 M 外部中断、 M 定时器中断
#### 非法指令
- `rdtime`和`sfence.vma`指令则重新适配
# SBI调用方式
```C
// 调用 ecall, which = a7, arg0-arg3 = a0-a3
// 返回 a0
#define SBI_CALL(which, arg0, arg1, arg2, arg3)

// SBI 调用 0-4 个参数
SBI_CALL_0
SBI_CALL_1
SBI_CALL_2
SBI_CALL_3
SBI_CALL_4

// 调用号 1，ch = a0
// 串口输出 ch
void sbi_console_putchar(int ch)


// 调用号 2，返回 a0
int sbi_console_getchar(void)
```

# 主函数
```rust
// 核心 0 直接返回 true
// 若是核心 1，则等待软件中断后返回 false
// 先启动 M 软件中断、再循环等待软件中断、最后关闭软件中断
fn mp_hook() -> bool

	
fn main() -> !
// 汇编代码设置堆栈指针 (sp)

// 如果是主核，清零bss段，将Flash中的 .data 拷贝到 RAM 的 .data 段中
// 如果非主核，等待 M 模式下的软件中断

// 设置异常或中断时跳入 _start_trap (mtvec)

// 如果是主核，就初始化对应的资源：
// 1. 初始化堆分配器
// 2. 初始化时钟系统
// 3. 配置串口、绑定标准输入输出
// 4. 注册 RustSBI框架的 IPI 实现（msip::set_ipi、msip::clear_ipi）
// 5. 初始化定时器循环调用（mtimecmp）
// 6. 注册处理重启请求、直接写死
// 7. 配置优先级 >= 0 的中断都可以被触发
// 8. 初始化 IO16 为输入，设置电平变化时触发中断
// 9. 终端打印关键信息
	
// 将 M 定时器中断、软件中断委托给 S
// 委托 M 模式异常给 S 模式
// 使能 M 软件中断
// 设置 mret 跳转到 mepc 地址 _s_mode_start
// 设置 MPP 为 S 模式
// mret 进入 S 模式

// 跳转到 _s_mode_start（0x80020000）
```
# 机器模式下的异常或中断处理函数
## 汇编向量
```asm
// mscratch 保存非0，则使用 mscratch 保存的 SP，是 S/U 模式跳入
// mscratch 保存0，则使用 SP，说明是 M 模式跳入

// 在 SP 指向的栈上保存通用寄存器
// 调用 _start_trap_rust
// 从 SP 指向的栈上恢复通用寄存器
// mret 返回 M 模式
```
## rust函数
### S 模式下执行 ecall
1. 如果系统调用号为 (0x0A000004, 0x210)
- 将函数地址保存到 DEVINTRENTRY
- 使能 M 外部中断
- 返回 (SBI_SUCCESS, 0)
2. 如果系统调用号为 (0x0A000005)
- 使能 M 外部中断、M 定时器中断
- 返回 (SBI_SUCCESS, 0)
3. 如果系统调用号为 (0x01)
- 串口输出 a0 存放的字符
4. 如果系统调用号为 (0x02)
- 将串口输入的字符存放到 a0
5. 其他ecall
- 若 a7 为 0 且读到了机器模式定时器中断、就使能 M 外部中断
- 调用真正的 SBI 处理函数
### M 软件中断转发到 S 软件中断
### M 定时器中断转发到 S 定时器中断
- 同时禁止 M 外部中断、M 定时器中断
### M 模式外部中断转发到 S 软件中断
- 将 M 模式外部中断转发为 S 模式软中断，stval 设置为 9
- 禁止 M 外部中断、 M 定时器中断
### 非法指令
- `rdtime`和`sfence.vma`指令则重新适配

# cargo.toml
```rust
[dependencies]
rustsbi = "0.1.1"

// 提供与 RISC-V 架构相关的底层功能
// 启用了 inline-asm 功能以使用内联汇编
riscv = { git = "https://github.com/rust-embedded/riscv", features = ["inline-asm"] }

// 一个简单的链表内存分配器
linked_list_allocator = "0.8"

// 针对 K210 的硬件抽象层
k210-hal = { git = "https://github.com/riscv-rust/k210-hal" }

// 提供初始化 .bss 和 .data段的功能
r0 = "1.0"



// 设定默认构建目标
[build]
target = "riscv64gc-unknown-none-elf"

// 告诉编译器链接时要使用 link-k210.ld
[target.riscv64gc-unknown-none-elf]
rustflags = [
    "-C", "link-arg=-Tlink-k210.ld",
]
```
