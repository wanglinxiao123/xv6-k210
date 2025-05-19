// #! 属性宏
#![no_std]  // 编译器不要使用标准库
#![no_main] // 程序没有传统意义上的 main 函数

// feature 启动编译器特性
#![feature(alloc_error_handler)] // 自定义分配失败时的错误处理函数
#![feature(global_asm)] // 允许全局汇编
#![feature(llvm_asm)]   // 允许将汇编代码嵌入到 Rust 函数中

// #[cfg()] 是条件编译属性，表示非单元测试
#[cfg(not(test))]
use core::alloc::Layout; // 用于描述数据在内存中布局的结构体

#[cfg(not(test))]
use core::panic::PanicInfo; // 用于处理 panic! 时的信息结构体

// K210 硬件抽象层，包含 时钟、FPIOA、外设访问控制
use k210_hal::{clock::Clocks, fpioa, pac, prelude::*};

// 基于链表分配器的内存分配器，用于在裸机环境中提供堆内存分配功能
use linked_list_allocator::LockedHeap;

// enter_privileged: 让程序进入特权模式，通常用于执行低级操作
use rustsbi::{enter_privileged};

// 提供了访问和操作 RISC-V 特权寄存器、指令等的能力
// mcause:  用于处理异常或中断的原因
// mideleg: 哪些中断由机器模式以外的特权模式处理
// medeleg: 哪些异常由机器模式以外的特权模式处理
// mepc:    发生异常或中断时的程序计数器PC
// mhartid: 存储当前硬件线程的ID
// mie:     启动或禁用特定的中断源
// mip:     标识哪些中断处于挂起状态
// misa:    用于表示 RISC-V 架构的扩展支持
// mstatus: 保存处理器的状态信息
// mtval:   保存导致异常或中断的具体值
// mtvec:   指定异常或中断发生时跳转的地址
// satp:    控制虚拟地址到物理地址的转换
use riscv::register::{
    mcause::{self, Exception, Interrupt, Trap},
    medeleg, mepc, mhartid, mideleg, mie, mip, misa::{self, MXL},
    mstatus::{self, MPP},
    mtval,
    mtvec::{self, TrapMode},
    satp,
};

// 引入 serial.rs
#[macro_use]
mod serial;

// #[global_allocator] 说明这个静态变量将作为程序的全局内存分配器
// 定义静态全局变量 ALLOCATOR, 类型为 LockedHeap
#[global_allocator]
static ALLOCATOR: LockedHeap = LockedHeap::empty();

// 静态可变变量 DEVINTRENTRY, 类型为 usize
static mut DEVINTRENTRY: usize = 0;


// panic 时调用的处理函数
#[cfg(not(test))]
#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    println!("[rustsbi] {}", info);
    loop {}
}

// 内存分配失败时调用的处理函数
#[cfg(not(test))]
#[alloc_error_handler]
fn oom(layout: Layout) -> ! {
    println!("[rustsbi] out of memory for layout {:?}", layout);
    loop {}
}

// 核心 0 直接返回 true
// 若是核心 1，则等待软件中断后返回 false
fn mp_hook() -> bool {
    use riscv::asm::wfi;       // Wait For Interrupt 指令
    use k210_hal::clint::msip; // 用于访问 MSIP

    // 核心为0，则直接返回
    let hartid = mhartid::read();
    if hartid == 0 {
        true
    } else {
        unsafe {
            msip::clear_ipi(hartid); // 清除当前核心的软件中断
            mie::set_msoft();        // 启用机器级软件中断

            // 等待中断，如果收到软件中断，就退出循环
            loop {
                wfi();
                if mip::read().msoft() {
                    break;
                }
            }

            mie::clear_msoft();       // 关闭软件中断
            msip::clear_ipi(hartid);  // 清除当前核心的软件中断
        }
        false
    }
}

// 将该函数导出为 _start
// 将 main 函数放在 .text.entry 段
// main 函数永远不会返回

// 分配堆栈空间
// 设置堆栈指针 sp
#[export_name = "_start"]
#[link_section = ".text.entry"] // this is stable
fn main() -> ! {
    // llvm_asm! 嵌入汇编
    // 设置堆栈指针 sp
    unsafe {
        llvm_asm!(
            "
        // 获取当前核心ID到 a2
        csrr    a2, mhartid

        // 获取系统允许的最大 hart_id 到 t0
        lui     t0, %hi(_max_hart_id)
        add     t0, t0, %lo(_max_hart_id)

        // 非法核心则跳转
        bgtu    a2, t0, _start_abort

        // 加载堆栈的起始地址到 sp
        la      sp, _stack_start

        // 获取对应的栈空间大小到 t0
        lui     t0, %hi(_hart_stack_size)
        add     t0, t0, %lo(_hart_stack_size)

    // 如果支持乘法, t0 = a2 * t0
    .ifdef __riscv_mul
        mul     t0, a2, t0
    .else
        beqz    a2, 2f  // Jump if single-hart
        mv      t1, a2
        mv      t2, t0
    1:
        add     t0, t0, t2
        addi    t1, t1, -1
        bnez    t1, 1b
    2:
    .endif
        // 设置堆栈指针，向下偏移  
        sub     sp, sp, t0
        // 清零 mscratch
        csrw    mscratch, zero
        j _start_success

    // 卡死等待
    _start_abort:
        wfi
        j _start_abort


    _start_success:
        
    "
        )
    };

    // 如果是主核，清零bss段，将Flash中的 .data 拷贝到 RAM 的 .data 段中
    if mp_hook() {
        extern "C" {
            static mut _ebss: u32;  // BSS  段结束地址
            static mut _sbss: u32;  // BSS  段起始地址
            static mut _edata: u32; // data 段结束地址
            static mut _sdata: u32; // data 段起始地址
            static _sidata: u32;    // FLASH 中的 .data 初始化值所在地址
        }
        unsafe {
            r0::zero_bss(&mut _sbss, &mut _ebss);
            r0::init_data(&mut _sdata, &mut _edata, &_sidata);
        } 
    }
   
    // 设置异常或中断时跳入 _start_trap
    extern "C" {
        fn _start_trap();
    }
    unsafe {
        mtvec::write(_start_trap as usize, TrapMode::Direct);
    }


    if mhartid::read() == 0 {
        // 用 _sheap 和 _heap_size 初始化堆分配器        
        extern "C" {
            fn _sheap();
            fn _heap_size();
        }
        let sheap = &mut _sheap as *mut _ as usize;
        let heap_size = &_heap_size as *const _ as usize;
        unsafe {
            ALLOCATOR.lock().init(sheap, heap_size);
        }

        // 获取硬件外设句柄
        let p = pac::Peripherals::take().unwrap();
        // 返回系统控制器
        let mut sysctl = p.SYSCTL.constrain();
        // 外设 fpioa 由 apb0 控制
        let fpioa = p.FPIOA.split(&mut sysctl.apb0);
        // 初始化时钟系统        
        let clocks = Clocks::new();

        // 配置串口、绑定标准输入输出
        let _uarths_tx = fpioa.io5.into_function(fpioa::UARTHS_TX);
        let _uarths_rx = fpioa.io4.into_function(fpioa::UARTHS_RX);
        let serial = p.UARTHS.configure(115_200.bps(), &clocks);
		serial::init(serial);


        // 给 RustSBI 框架注册 IPI 实现  
        struct Ipi;
        impl rustsbi::Ipi for Ipi {
            // 最大核心 ID
            fn max_hart_id(&self) -> usize {
                1
            }
            // 软中断脉冲信号
            fn send_ipi_many(&mut self, hart_mask: rustsbi::HartMask) {
                use k210_hal::clint::msip;
                for i in 0..=1 {
                    if hart_mask.has_bit(i) {
                        msip::set_ipi(i);
                        msip::clear_ipi(i);
                    }
                }
            }
        }
        use rustsbi::init_ipi;
        init_ipi(Ipi);

        // 定时器循环调用
        struct Timer;
        impl rustsbi::Timer for Timer {
            fn set_timer(&mut self, stime_value: u64) {
                use k210_hal::clint::mtimecmp;
                mtimecmp::write(mhartid::read(), stime_value);
                unsafe { mip::clear_mtimer() };
            }
        }
        use rustsbi::init_timer;
        init_timer(Timer);


        // 处理重启请求、直接写死
        struct Reset;
        impl rustsbi::Reset for Reset {
            fn system_reset(&self, reset_type: usize, reset_reason: usize) -> rustsbi::SbiRet {
                println!("[rustsbi] reset triggered! todo: shutdown all harts on k210; program halt. Type: {}, reason: {}", reset_type, reset_reason);
                loop {}
            }
        }
        use rustsbi::init_reset;
        init_reset(Reset);


        use k210_hal::plic::Priority;
        use k210_hal::pac::Interrupt;
        use k210_hal::gpiohs::Edge;

        // 配置所有优先级 ≥ 0 的中断都可以被触发
        unsafe { 
            pac::PLIC::set_threshold(mhartid::read(), Priority::P0);
        }

        // 初始化 io16 为输入，设置电平变化时触发中断        
        let gpiohs = p.GPIOHS.split();
        fpioa.io16.into_function(fpioa::GPIOHS0);
        let mut boot = gpiohs.gpiohs0.into_pull_up_input();
        boot.trigger_on_edge(Edge::RISING | Edge::FALLING);
        unsafe {
            pac::PLIC::set_priority(Interrupt::GPIOHS0, Priority::P1);
            pac::PLIC::unmask(mhartid::read(), Interrupt::GPIOHS0);
        }
        boot.clear_interrupt_pending_bits();
    }

    unsafe {
        // 将 M 定时器中断、软件中断委托给 S
        mideleg::set_stimer();
        mideleg::set_ssoft();

        // 委托 M 模式异常给 S模式
        medeleg::set_instruction_misaligned();
        medeleg::set_breakpoint();
        medeleg::set_user_env_call();
        medeleg::set_instruction_fault();
        medeleg::set_load_fault();
        medeleg::set_store_fault();

        // 使能 M 软件中断
        mie::set_msoft();
    }

    // 终端输出关键信息
    if mhartid::read() == 0 {
        println!("[rustsbi] RustSBI version {}", rustsbi::VERSION);
        println!("{}", rustsbi::LOGO);
        println!("[rustsbi] Platform: K210 (Version {})", env!("CARGO_PKG_VERSION"));
        let isa = misa::read();
        if let Some(isa) = isa {
            let mxl_str = match isa.mxl() {
                MXL::XLEN32 => "RV32",
                MXL::XLEN64 => "RV64",
                MXL::XLEN128 => "RV128",
            };
            print!("[rustsbi] misa: {}", mxl_str);
            for ext in 'A'..='Z' {
                if isa.has_extension(ext) {
                    print!("{}", ext);
                }
            }
            println!("");
        }
        println!("[rustsbi] mideleg: {:#x}", mideleg::read().bits());
        println!("[rustsbi] medeleg: {:#x}", medeleg::read().bits());
        println!("[rustsbi] Kernel entry: 0x80020000");
    }

    extern "C" {
        fn _s_mode_start();
    }

    // 设置 mret 跳转到 mepc 地址 _s_mode_start
    // 设置 MPP 为 S 模式
    // mret 进入 S 模式
    unsafe {
        mepc::write(_s_mode_start as usize);
        mstatus::set_mpp(MPP::Supervisor);
        enter_privileged(mhartid::read(), 0x2333333366666666);
    }
}

// 跳转到 0x80020000
global_asm!(
    "
    .section .text
    .globl _s_mode_start
_s_mode_start:
1:  auipc ra, %pcrel_hi(1f)
    ld ra, %pcrel_lo(1b)(ra)
    jr ra
.align  3
1:  .dword 0x80020000
"
);

// mscratch 存放之前中断保存的栈指针
global_asm!(
    "
    // 每个寄存器占 8 个字节
    .equ REGBYTES, 8

    // 保存 reg 到 sp, 偏移 8*offset 的寄存器中
    .macro STORE reg, offset
        sd  \\reg, \\offset*REGBYTES(sp)
    .endm

    // 从 sp, 偏移 8*offset 的寄存器中加载数据到 reg
    .macro LOAD reg, offset
        ld  \\reg, \\offset*REGBYTES(sp)
    .endm


    .section .text        // 开始定义 .text段
    .global _start_trap   // 对外暴露 trap 入口
    .p2align 2            // 4 字节对齐

_start_trap:
    // 交换 sp 和 mscratch
    csrrw   sp, mscratch, sp

    // 如果 mscratch 保存非0, 表示从 S/U 模式跳入，则跳转到标签 1
    bnez    sp, 1f
    // 否则从 M 模式跳入，清零 mscratch, 恢复 sp
    csrrw   sp, mscratch, zero

1:
    // 保存 16 个通用寄存器
    addi    sp, sp, -16 * REGBYTES
    STORE   ra, 0
    STORE   t0, 1
    STORE   t1, 2
    STORE   t2, 3
    STORE   t3, 4
    STORE   t4, 5
    STORE   t5, 6
    STORE   t6, 7
    STORE   a0, 8
    STORE   a1, 9
    STORE   a2, 10
    STORE   a3, 11
    STORE   a4, 12
    STORE   a5, 13
    STORE   a6, 14
    STORE   a7, 15

    // 把 sp 作为参数传给 Rust trap 处理函数
    mv      a0, sp
    call    _start_trap_rust

    // 恢复寄存器
    LOAD    ra, 0
    LOAD    t0, 1
    LOAD    t1, 2
    LOAD    t2, 3
    LOAD    t3, 4
    LOAD    t4, 5
    LOAD    t5, 6
    LOAD    t6, 7
    LOAD    a0, 8
    LOAD    a1, 9
    LOAD    a2, 10
    LOAD    a3, 11
    LOAD    a4, 12
    LOAD    a5, 13
    LOAD    a6, 14
    LOAD    a7, 15
    addi    sp, sp, 16 * REGBYTES

    // 恢复 sp 到 mscratch
    csrrw   sp, mscratch, sp
    mret
"
);

// #[allow(unused)] 表示不要对未使用的项发出警告
#[allow(unused)]
struct TrapFrame {
    ra: usize,
    t0: usize,
    t1: usize,
    t2: usize,
    t3: usize,
    t4: usize,
    t5: usize,
    t6: usize,
    a0: usize,
    a1: usize,
    a2: usize,
    a3: usize,
    a4: usize,
    a5: usize,
    a6: usize,
    a7: usize,
}

#[export_name = "_start_trap_rust"]
extern "C" fn start_trap_rust(trap_frame: &mut TrapFrame) {
    let cause = mcause::read().cause();
    match cause {
        // S 模式下执行 ecall
        Trap::Exception(Exception::SupervisorEnvCall) => {
            // 如果系统调用号为 (0x0A000004, 0x210)
            // 将函数地址保存到 DEVINTRENTRY
            // 使能 M 外部中断
            // 返回 (SBI_SUCCESS, 0)
            if trap_frame.a7 == 0x0A000004 && trap_frame.a6 == 0x210 {
                // 将函数地址保存到 DEVINTRENTRY
                unsafe { DEVINTRENTRY = trap_frame.a0; }
                // enable mext
                unsafe { mie::set_mext(); }
                // return values
                trap_frame.a0 = 0;
                trap_frame.a1 = 0;
            } 
            // 如果系统调用号为 (0x0A000005)
            // 使能 M 外部中断、M 定时器中断
            // 返回 (SBI_SUCCESS, 0)            
            else if trap_frame.a7 == 0x0A000005 {
                unsafe {
                    mie::set_mext();
                    mie::set_mtimer();
                }
                trap_frame.a0 = 0;
                trap_frame.a1 = 0;
            } 
            // 如果系统调用号为 (0x01)
            // 串口输出 a0 存放的字符
			else if 0x01 == trap_frame.a7 {
				serial::putchar(trap_frame.a0 as u8);
			}
            // 如果系统调用号为 (0x02)
            // 将串口输入的字符存放到 a0  
			else if 0x02 == trap_frame.a7 {
				trap_frame.a0 = match serial::getchar() {
					Some(c) => c as usize, 
					None => (-1i64 as usize), 
				};
			}
			else {
                // 若 a7 为 0 且触发了机器模式定时器中断、就使能 M 外部中断
                if trap_frame.a7 == 0x0 {
                    unsafe {
                        let mtip = mip::read().mtimer();
                        if mtip {
                            mie::set_mext();
                        }
                    }
                }

                // 调用真正的 SBI 处理函数
                let params = [trap_frame.a0, trap_frame.a1, trap_frame.a2, trap_frame.a3];
                let ans = rustsbi::ecall(trap_frame.a7, trap_frame.a6, params);
                trap_frame.a0 = ans.error;
                trap_frame.a1 = ans.value;
            }

            // mepc 保存发生异常时的 PC 地址，跳过 ECALL 指令
            mepc::write(mepc::read().wrapping_add(4));
        }
        // 将 M 软件中断转发到 S 软件中断
        Trap::Interrupt(Interrupt::MachineSoft) => {
            unsafe {
                mip::set_ssoft();
                mie::clear_msoft();
            }
        }
        // 将 M 定时器中断转发到 S 定时器中断
        // 禁止 M 外部中断、M 定时器中断
        Trap::Interrupt(Interrupt::MachineTimer) => {
            unsafe {
                mip::set_stimer();
                mie::clear_mext();
                mie::clear_mtimer();
            }
        }
        // 收到 M 模式外部中断
        // 将 M 模式外部中断转发为 S 模式软中断，stval 设置为 9
        // 禁止 M 外部中断、 M 定时器中断
        Trap::Interrupt(Interrupt::MachineExternal) => {
            unsafe {
                llvm_asm!("csrw stval, $0" :: "r"(9) :: "volatile");
                mip::set_ssoft(); // set S-soft interrupt flag
                mie::clear_mext();
                mie::clear_mtimer();
            }
        }
        Trap::Exception(Exception::IllegalInstruction) => {
            // 取出一条可能非法的指令
            let vaddr = mepc::read();
            let ins = unsafe { get_vaddr_u32(vaddr) };

            // 处理本身不支持的 rdtime 指令
            // 将机器时间写入目标寄存器
            if ins & 0xFFFFF07F == 0xC0102073 {
                let rd = ((ins >> 7) & 0b1_1111) as u8;
                let mtime = k210_hal::clint::mtime::read();
                let time_usize = mtime as usize;
                set_rd(trap_frame, rd, time_usize);
                mepc::write(mepc::read().wrapping_add(4));

            } 
            // 处理器本身不支持的 sfence.vma
            else if ins & 0xFE007FFF == 0x12000073 {
                // 读取页表基地址
                let satp_bits = satp::read().bits();

                // 将页表基地址写入旧版本的 SPTBR 寄存器
                let ppn = satp_bits & 0xFFF_FFFF_FFFF;
                let sptbr_bits = ppn & 0x3F_FFFF_FFFF;
                unsafe { llvm_asm!("csrw 0x180, $0"::"r"(sptbr_bits)) };


                // 手动设置 mstatus 为开启 Sv39 分页
                let mut mstatus_bits: usize; 
                unsafe { llvm_asm!("csrr $0, mstatus":"=r"(mstatus_bits)) };
                mstatus_bits &= !0x1F00_0000;
                mstatus_bits |= 9 << 24; 
                unsafe { llvm_asm!("csrw mstatus, $0"::"r"(mstatus_bits)) };

                // 旧版 TLB 刷新
                unsafe { llvm_asm!(".word 0x10400073") };
                mepc::write(mepc::read().wrapping_add(4));
            } else {
                panic!("invalid instruction! mepc: {:016x?}, instruction: {:08x?}", mepc::read(), ins);
            }
        }
        cause => panic!(
            "unhandled trap! mcause: {:?}, mepc: {:016x?}, mtval: {:016x?}",
            cause,
            mepc::read(),
            mtval::read(),
        ),
    }
}


// 以 mpp 的权限读取 vaddr 虚拟地址上的 32 位数据
#[inline]
unsafe fn get_vaddr_u32(vaddr: usize) -> u32 {
    // todo: comment
    get_vaddr_u16(vaddr) as u32 | 
    ((get_vaddr_u16(vaddr.wrapping_add(2)) as u32) << 16)
}


// 以 mpp 的权限读取 vaddr 虚拟地址上的 16 位数据
#[inline]
unsafe fn get_vaddr_u16(vaddr: usize) -> u16 {
    let mut ans: u16;
    llvm_asm!("
        li      t0, (1 << 17)
        csrrs   t0, mstatus, t0
        lhu     $0, 0($1)
        csrw    mstatus, t0
    "
        :"=r"(ans) 
        :"r"(vaddr)
        :"t0", "t1");
    ans
}


// 根据 rd 将值存入到 trap_frame 对应的字段
#[inline]
fn set_rd(trap_frame: &mut TrapFrame, rd: u8, value: usize) {
    match rd {
        10 => trap_frame.a0 = value,
        11 => trap_frame.a1 = value,
        12 => trap_frame.a2 = value,
        13 => trap_frame.a3 = value,
        14 => trap_frame.a4 = value,
        15 => trap_frame.a5 = value,
        16 => trap_frame.a6 = value,
        17 => trap_frame.a7 = value,
        5  => trap_frame.t0 = value,
        6  => trap_frame.t1 = value,
        7  => trap_frame.t2 = value,
        28 => trap_frame.t3 = value,
        29 => trap_frame.t4 = value,
        30 => trap_frame.t5 = value,
        31 => trap_frame.t6 = value,
        _ => panic!("invalid target `rd`"),
    }
}

