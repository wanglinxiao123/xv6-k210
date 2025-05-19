#include "include/printf.h"
#include "include/types.h"
#include "include/riscv.h"
#include "include/gpiohs.h"
#include "include/buf.h"
#include "include/spinlock.h"

#include "include/dmac.h"
#include "include/spi.h"
#include "include/sdcard.h"

// 通过 CPU 发送数据
static void sd_write_data(uint8 const *data_buff, uint32 length)
{
    spi_init(SPI_DEVICE_0, SPI_WORK_MODE_0, SPI_FF_STANDARD, 8, 0);
    spi_send_data_standard(SPI_DEVICE_0, SPI_CHIP_SELECT_3, NULL, 0, data_buff, length);
}

// 通过 CPU 接收数据
static void sd_read_data(uint8 *data_buff, uint32 length)
{
    spi_init(SPI_DEVICE_0, SPI_WORK_MODE_0, SPI_FF_STANDARD, 8, 0);
    spi_receive_data_standard(SPI_DEVICE_0, SPI_CHIP_SELECT_3, NULL, 0, data_buff, length);
}

// 通过 DMA 发送数据
static void sd_write_data_dma(uint8 const *data_buff, uint32 length)
{
    spi_init(SPI_DEVICE_0, SPI_WORK_MODE_0, SPI_FF_STANDARD, 8, 0);
    spi_send_data_standard_dma(DMAC_CHANNEL0, SPI_DEVICE_0, SPI_CHIP_SELECT_3, NULL, 0, data_buff, length);
}

// 通过 DMA 接收数据
static void sd_read_data_dma(uint8 *data_buff, uint32 length)
{
    spi_init(SPI_DEVICE_0, SPI_WORK_MODE_0, SPI_FF_STANDARD, 8, 0);
    spi_receive_data_standard_dma(-1, DMAC_CHANNEL0, SPI_DEVICE_0, SPI_CHIP_SELECT_3, NULL, 0, data_buff, length);
}

// 向 SD 卡发送命令帧，(命令、 4字节参数、 CRC)
static void sd_send_cmd(uint8 cmd, uint32 arg, uint8 crc)
{
    uint8 frame[6];
    frame[0] = (cmd | 0x40);
    frame[1] = (uint8)(arg >> 24);
    frame[2] = (uint8)(arg >> 16);
    frame[3] = (uint8)(arg >> 8);
    frame[4] = (uint8)(arg);
    frame[5] = (crc);
    sd_write_data(frame, 6);
}

// 发送结束脉冲
static void sd_end_cmd(void)
{
    uint8 frame[1] = {0xFF};
    sd_write_data(frame, 1);
}

#define SD_CMD0 0    // 复位命令
#define SD_CMD8 8    // 检测 SD 卡版本
#define SD_CMD58 58  // 读取 OCR 寄存器
#define SD_CMD55 55  // 前导命令，告诉接下来发送的是应用专用命令
#define SD_ACMD41 41 // SD_SEND_OP_COND
#define SD_CMD16 16  // 设置块区容量
#define SD_CMD17 17  // 单块读命令
#define SD_CMD24 24  // 单块写命令
#define SD_CMD13 13  // 读取卡状态

// 读取 R1 类型响应
static uint8 sd_get_response_R1(void)
{
    uint8 result;
    uint16 timeout = 0xff;

    while (timeout--)
    {
        sd_read_data(&result, 1);
        if (result != 0xff)
        {
            return result;
        }
    }

    return 0xff;
}

// 读取 R3 类型剩下的响应
// 用于 CMD58, 判断卡的电压范围和是否支持 SDHC/SDXC
static void sd_get_response_R3_rest(uint8 *frame)
{
    sd_read_data(frame, 4);
}

// 读取 R7 类型剩下的响应
// 用于 CMD8, 检测 SD 卡电压范围、是否符合 2.0 标准
static void sd_get_response_R7_rest(uint8 *frame)
{
    sd_read_data(frame, 4);
}

// 将 SD 卡重置为 idle 状态
static int switch_to_SPI_mode(void)
{
    int timeout = 0xff;

    while (--timeout)
    {
        // 发送复位命令
        sd_send_cmd(SD_CMD0, 0, 0x95);
        // 读取一字节响应
        uint64 result = sd_get_response_R1();
        // 产生 SPI 时钟
        sd_end_cmd();
        // 读取到成功
        if (0x01 == result)
        {
            break;
        }
    }

    if (0 == timeout)
    {
        printf("SD_CMD0 failed\n");
        return 0xff;
    }

    return 0;
}

// 验证 SD 卡支持的电压范围
static int verify_operation_condition(void)
{
    uint64 result;

    uint8 frame[4];
    sd_send_cmd(SD_CMD8, 0x01aa, 0x87); // 检查电压范围
    result = sd_get_response_R1();      // 获取一字节响应
    sd_get_response_R7_rest(frame);     // 读取剩余的四字节
    sd_end_cmd();

    // CRC 错误
    if (0x09 == result)
    {
        printf("invalid CRC for CMD8\n");
        return 0xff;
    }
    // 检查电压范围
    else if (0x01 == result && 0x01 == (frame[2] & 0x0f) && 0xaa == frame[3])
    {
        return 0x00;
    }

    printf("verify_operation_condition() fail!\n");
    return 0xff;
}

// 读取操作条件寄存器，确认当前的工作状态和电压范围
static int read_OCR(void)
{
    uint64 result;
    uint8 ocr[4];

    int timeout = 0xff;

    while (--timeout)
    {
        sd_send_cmd(SD_CMD58, 0, 0);
        result = sd_get_response_R1();
        sd_get_response_R3_rest(ocr);
        sd_end_cmd();

        if (0x01 == result && (ocr[1] & 0x1f) && (ocr[2] & 0x80))
        {
            return 0;
        }
    }

    printf("read_OCR() timeout!\n");
    printf("result = %d\n", result);
    return 0xff;
}

// 将卡设置为就绪状态
static int set_SDXC_capacity(void)
{
    uint8 result = 0xff;

    int timeout = 0xfff;
    while (--timeout)
    {
        // 发送前导命令，检查响应
        sd_send_cmd(SD_CMD55, 0, 0);
        result = sd_get_response_R1();
        sd_end_cmd();
        if (0x01 != result)
        {
            printf("SD_CMD55 fail! result = %d\n", result);
            return 0xff;
        }

        // 主机支持高容量，询问卡是否准备好进入就绪状态
        sd_send_cmd(SD_ACMD41, 0x40000000, 0);
        result = sd_get_response_R1();
        sd_end_cmd();
        if (0 == result)
        {
            return 0;
        }
    }

    printf("set_SDXC_capacity() timeout!\n");
    printf("result = %d\n", result);
    return 0xff;
}

// 0 表示高容量卡，1 表示标准卡
static int is_standard_sd = 0;

// 查看 OCR 寄存器，并设置块大小为 512
static int check_block_size(void)
{
    uint8 result = 0xff;
    uint8 ocr[4];

    int timeout = 0xff;
    while (timeout--)
    {
        // 读取 OCR 寄存器
        sd_send_cmd(SD_CMD58, 0, 0);
        result = sd_get_response_R1();
        sd_get_response_R3_rest(ocr);
        sd_end_cmd();

        if (0 == result)
        {
            // 如果是 SDHC/SDXC 卡
            if (ocr[0] & 0x40)
            {
                printf("SDHC/SDXC detected\n");
                if (512 != BSIZE)
                {
                    printf("BSIZE != 512\n");
                    return 0xff;
                }

                is_standard_sd = 0;
            }
            // 如果是 SDSC 卡，则设置块区容量
            else
            {
                printf("SDSC detected, setting block size\n");
                int timeout = 0xff;
                int result = 0xff;
                while (--timeout)
                {
                    sd_send_cmd(SD_CMD16, BSIZE, 0);
                    result = sd_get_response_R1();
                    sd_end_cmd();

                    if (0 == result)
                    {
                        break;
                    }
                }
                if (0 == timeout)
                {
                    printf("check_OCR(): fail to set block size");
                    return 0xff;
                }

                is_standard_sd = 1;
            }

            return 0;
        }
    }

    printf("check_OCR() timeout!\n");
    printf("result = %d\n", result);
    return 0xff;
}

// 初始化 SD 卡，设置为 SPI 模式
static int sd_init(void)
{
    uint8 frame[10];

    // 发送时钟，促使 SD 卡工作
    for (int i = 0; i < 10; i++)
    {
        frame[i] = 0xff;
    }
    sd_write_data(frame, 10);

    // 将 SD 卡重置为 idle 状态
    if (0 != switch_to_SPI_mode())
    {
        return 0xff;
    }
    // 验证 SD 卡支持的电压范围
    if (0 != verify_operation_condition())
    {
        return 0xff;
    }
    // 读取操作条件寄存器，确认当前的工作状态和电压范围
    if (0 != read_OCR())
    {
        return 0xff;
    }
    //  将卡设置为就绪状态
    if (0 != set_SDXC_capacity())
    {
        return 0xff;
    }
    // 查看 OCR 寄存器，并设置块大小为 512
    if (0 != check_block_size())
    {
        return 0xff;
    }

    return 0;
}

static struct sleeplock sdcard_lock;

// 通过 SPI 初始化 SD卡，并初始化 sdcard_lock
void sdcard_init(void)
{
    int result = sd_init();
    initsleeplock(&sdcard_lock, "sdcard");

    if (0 != result)
    {
        panic("sdcard_init failed");
    }
}

// 读取一个扇区
void sdcard_read_sector(uint8 *buf, int sectorno)
{
    uint8 result;
    uint32 address;
    uint8 dummy_crc[2];

    // 统一寻址方式
    if (is_standard_sd)
    {
        address = sectorno << 9;
    }
    else
    {
        address = sectorno;
    }

    acquiresleep(&sdcard_lock);

    // 发送单块读命令
    sd_send_cmd(SD_CMD17, address, 0);
    result = sd_get_response_R1();
    if (0 != result)
    {
        releasesleep(&sdcard_lock);
        panic("sdcard: fail to read");
    }

    // 等待数据块
    int timeout = 0xffffff;
    while (--timeout)
    {
        sd_read_data(&result, 1);
        if (0xfe == result)
        {
            break;
        }
    }

    if (0 == timeout)
    {
        panic("sdcard: timeout waiting for reading");
    }

    // 读取 512 字节的数据
    sd_read_data_dma(buf, BSIZE);
    sd_read_data(dummy_crc, 2);
    sd_end_cmd();

    releasesleep(&sdcard_lock);
}

// 写入一个扇区
void sdcard_write_sector(uint8 *buf, int sectorno)
{
    uint32 address;
    static uint8 const START_BLOCK_TOKEN = 0xfe;
    uint8 dummy_crc[2] = {0xff, 0xff};

    // 统一寻址方式
    if (is_standard_sd)
    {
        address = sectorno << 9;
    }
    else
    {
        address = sectorno;
    }

    acquiresleep(&sdcard_lock);

    // 发送单块写命令
    sd_send_cmd(SD_CMD24, address, 0);
    if (0 != sd_get_response_R1())
    {
        releasesleep(&sdcard_lock);
        panic("sdcard: fail to write");
    }

    // 发送数据块
    sd_write_data(&START_BLOCK_TOKEN, 1);
    sd_write_data_dma(buf, BSIZE);
    sd_write_data(dummy_crc, 2);

    // 等待卡写入完成
    uint8 result;
    int timeout = 0xfff;
    while (--timeout)
    {
        sd_read_data(&result, 1);
        if (0x05 == (result & 0x1f))
        {
            break;
        }
    }

    if (0 == timeout)
    {
        releasesleep(&sdcard_lock);
        panic("sdcard: invalid response token");
    }

    // 等待卡内部写入完成
    timeout = 0xffffff;
    while (--timeout)
    {
        sd_read_data(&result, 1);
        if (0 != result)
        {
            break;
        }
    }
    if (0 == timeout)
    {
        releasesleep(&sdcard_lock);
        panic("sdcard: timeout waiting for response");
    }
    sd_end_cmd();

    // 读取卡状态，确保没有错误
    uint8 error_code = 0xff;
    sd_send_cmd(SD_CMD13, 0, 0);
    result = sd_get_response_R1();
    sd_read_data(&error_code, 1);
    sd_end_cmd();

    if (0 != result || 0 != error_code)
    {
        releasesleep(&sdcard_lock);
        printf("result: %x\n", result);
        printf("error_code: %x\n", error_code);
        panic("sdcard: an error occurs when writing");
    }

    releasesleep(&sdcard_lock);
}
