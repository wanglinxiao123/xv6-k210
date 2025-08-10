K=kernel
U=xv6-user
T=target

OBJS = \
  $K/entry_k210.o \
  $K/printf.o \
  $K/kalloc.o \
  $K/intr.o \
  $K/spinlock.o \
  $K/string.o \
  $K/main.o \
  $K/vm.o \
  $K/proc.o \
  $K/swtch.o \
  $K/trampoline.o \
  $K/trap.o \
  $K/syscall.o \
  $K/sysproc.o \
  $K/bio.o \
  $K/sleeplock.o \
  $K/file.o \
  $K/pipe.o \
  $K/exec.o \
  $K/sysfile.o \
  $K/kernelvec.o \
  $K/timer.o \
  $K/disk.o \
  $K/fat32.o \
  $K/plic.o \
  $K/console.o \
  $K/spi.o \
  $K/gpiohs.o \
  $K/fpioa.o \
  $K/utils.o \
  $K/sdcard.o \
  $K/dmac.o \
  $K/sysctl.o \
  $K/i2c.o \

UPROGS = \
	$U/_init\
	$U/_sh\
	$U/_cat\
	$U/_echo\
	$U/_grep\
	$U/_ls\
	$U/_kill\
	$U/_mkdir\
	$U/_xargs\
	$U/_sleep\
	$U/_find\
	$U/_rm\
	$U/_wc\
	$U/_test\
	$U/_usertests\
	$U/_strace\
	$U/_mv\
	$U/_i2c_read\



RUSTSBI = ./bootloader/SBI/sbi-k210

TOOLPREFIX := riscv64-unknown-elf-
CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

# 启动所有警告，将警告视为错误（除了 无限递归 和 未使用的变量）
CFLAGS = -Wall -Werror -Wno-error=unused-variable -Wno-error=infinite-recursion
# 添加头文件路径
CFLAGS += -I.
# 代码和数据可以位于任何位置
CFLAGS += -mcmodel=medany
# 设置开启调试
CFLAGS += -O0 -fno-omit-frame-pointer -ggdb -g 
# 禁止编译器优化
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
# 警用编译器的栈保护机制
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# 目标文件的内存页大小为 4K
LDFLAGS = -z max-page-size=4096

image = $T/kernel.bin
k210 = $T/k210.bin
k210-serialport := /dev/ttyUSB0


# 提取内核和 RUSTSBI 的纯二进制镜像
# 拼接两个镜像
# 将反汇编结果保存到 k210.asm
# 调用 kflash.py 进行烧录
run: build
	@$(OBJCOPY) $T/kernel --strip-all -O binary $(image)
	@$(OBJCOPY) $(RUSTSBI) --strip-all -O binary $(k210)
	@dd if=$(image) of=$(k210) bs=128k seek=1
	@$(OBJDUMP) -D -b binary -m riscv $(k210) > $T/k210.asm
	@sudo chmod 777 $(k210-serialport)
	@python3 ./tools/kflash.py -p $(k210-serialport) -b 1500000 -t $(k210)

build: $T/kernel userprogs

SD_DST = /media/wlx/9669-BAE1
# 将可执行程序拷贝到 SD 卡
sdcard: userprogs
	@if [ ! -d "$(SD_DST)/bin" ]; then sudo mkdir $(SD_DST)/bin; fi
	@for file in $$( ls $U/_* ); do \
		sudo cp $$file $(SD_DST)/bin/$${file#$U/_}; done
	@sudo cp $U/_init $(SD_DST)/init
	@sudo cp $U/_sh $(SD_DST)/sh

# 链接所有的 .o 文件，生成可执行文件 kernel
# 将可执行文件 kernel 生成反汇编代码 kernel.asm
# 将可执行文件 kernel 生成符号表 kernel.sym
linker = ./linker/k210.ld
$T/kernel: $(OBJS) $(linker) $U/initcode
	@if [ ! -d "./target" ]; then mkdir target; fi
	@$(LD) $(LDFLAGS) -T $(linker) -o $T/kernel $(OBJS)
	@$(OBJDUMP) -S $T/kernel > $T/kernel.asm
	@$(OBJDUMP) -t $T/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $T/kernel.sym
  
userprogs: $(UPROGS)

# 构建用户态程序，并生成反汇编文件和符号表
ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o
_%: %.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym


# 编译汇编代码 .s -> .o
# 链接为可执行文件 .o -> .out
# 从 ELF 提取二进制代码
# 生成反汇编代码 initcode.o -> initcode.asm
$U/initcode: $U/initcode.S
	$(CC) $(CFLAGS) -march=rv64g -nostdinc -I. -Ikernel -c $U/initcode.S -o $U/initcode.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $U/initcode.out $U/initcode.o
	$(OBJCOPY) -S -O binary $U/initcode.out $U/initcode
	$(OBJDUMP) -S $U/initcode.o > $U/initcode.asm

# 生成用户态系统调用的汇编代码
$U/usys.S : $U/usys.pl
	@perl $U/usys.pl > $U/usys.S
$U/usys.o : $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S

clean: 
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym \
	$T/* \
	$U/initcode $U/initcode.out \
	$K/kernel \
	.gdbinit \
	$U/usys.S \
	$(UPROGS)


# 使用 cargo 编译 rustsbi
RUSTSBI:
	@cd ./bootloader/SBI/rustsbi-k210 && cargo build && cp ./target/riscv64gc-unknown-none-elf/debug/rustsbi-k210 ../sbi-k210
	@$(OBJDUMP) -S ./bootloader/SBI/sbi-k210 > $T/rustsbi-k210.asm

rustsbi-clean:
	@cd ./bootloader/SBI/rustsbi-k210 && cargo clean
