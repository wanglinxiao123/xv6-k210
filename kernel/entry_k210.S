    .section .text.entry
    .globl _start
_start:
	# a0 存储 Hart ID，由启动固件传入
	# t0 = (hart_id + 1) * 16KB
    add t0, a0, 1
    slli t0, t0, 14
	
	# 初始化每个核心的 sp 指针
	la sp, boot_stack
    add sp, sp, t0

	# 调转到 main函数
    call main

	# 死循环兜底
loop:
    j loop

	# .bss段，4KB对齐
	# 为每个核心分配 16KB
    .section .bss.stack
    .align 12

    .globl boot_stack
boot_stack:

    .space 4096 * 4 * 2

    .globl boot_stack_top
boot_stack_top: