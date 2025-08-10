# 项目介绍
## 概要
- [xv6-k210](https://github.com/HUST-OS/xv6-k210 ) 项目是几位大佬完成的，此项目将 xv6 操作系统移植到基于 RISC-V 架构的 K210 芯片上，感谢该仓库的各位大佬完成的大量工作
- 本项目基于[xv6-k210](https://github.com/HUST-OS/xv6-k210 ) ，对该仓库的代码进行阅读和源码注释，并尝试进行一些修改
## 动机
- xv6 操作系统是在 qemu 模拟器上运行的，系统运行在电脑终端上。在学习完成后，想要尝试自己移植到实际的芯片上运行，通过网络搜索很快找到了[xv6-k210](https://github.com/HUST-OS/xv6-k210 ) 仓库，恰巧以前竞赛的时候使用过 K210 ，当时是通过 MicroPython 在 K210 上运行 YOLOv2，对该芯片也有一定的了解，所以就决定行动起来
## 硬件准备
- Sipeed Maix Bit 开发板
- 一张 SD 卡 + 读卡器
- Sipeed RV debugger plus 调试器（可选）

# 程序启动
1. 格式化 SD 卡
- 在原作者移植的 xv6-k210 中，将文件系统修改为 FAT32，并将数据存储在 SD 卡中，但是文件系统不支持分区，因此需要格式化 SD 卡，并且不进行分区
- 首先将 SD 卡通过读卡器查到 Ubuntu 的电脑上，通过 df -h 查看设备名称，下图为`/dev/sda`
![](./img/1.jpg)
- 对 SD 卡进行格式化
```bash
sudo umount /dev/sda
sudo mkfs.fat -F32 /dev/sda
```


2. 安装一些工具包
```bash
sudo apt update
sudo apt install build-essential pkg-config  autoconf libtool libssl-dev flex bison ninja-build libglib2.0-dev libpixman-1-dev libslirp-dev libncurses5-dev libncursesw5-dev git build-essential gdb-multiarch qemu-system-misc gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu gcc-riscv64-unknown-elf
```

3. 下载代码，编译并将用户态程序拷贝到 SD 卡中
```bash
git clone https://github.com/wanglinxiao123/xv6-k210.git
cd xv6-k210
# 更改为实际的挂载路径
export SD_DST=/media/wlx/0F62-5AA5
# 该命令编译用户态程序，并拷贝到 SD 卡中
make sdcard
```

4. 将 SD 卡插入开发板中，并将 K210 开发板插入电脑中，查看系统为该串口生成的字符文件，修改`makefie`中的`k210-serialport`变量，通过`make run`烧录程序
```bash
ls /dev | grep ttyUSB
# 修改 makefile 中的 k210-serialport 为实际端口，如/dev/ttyUSB0
make run
```

5. 大功告成，我们的程序跑起来了！
![](./img/2.jpg)

# 个人笔记
- 存放在根目录的`doc`文件夹下，分模块记录

# 与原生xv6的区别
## 模块移植
- 通过`RustSBI`提供为操作系统提供底层服务，内核不直接执行机器模式的指令
- 修改文件系统为`FAT32`格式，去除日志层、索引节点层，改为`FAT32`逻辑，通过`SPI`读写访问`SD`卡扇区，进行文件存储
## 功能扩展
- 为每个进程维护一个内核页表，在陷入内核时切换进程的内核页表，实现进程用户态空间读写
- 利用缺页中断，实现惰分配和写时复制
- 添加系统调用，通过芯片SDK实现`I2C`和`SPI`设备读写
- 修改内存页和磁盘缓冲块的分配机制，通过按哈希桶分配减少自旋等待次数

# 遗留问题
- 使用`OpenOCD`调试芯片时，无法同时调试两个核心，且系统重启时不能停在断点处，只有完全运行后才能进行调试（猜测和调试工具版本与调试器硬件有关）
- 在打开交叉编译的`O1-O3`优化时，多次执行用户态程序，可能会执行到非法指令，导致内核运行异常