#ifndef FPGA_WATCHDOG_H
#define FPGA_WATCHDOG_H

#include "common.h"

/**
 * FPGA看门狗模块
 *
 * 功能说明:
 * - 通过GPIO引脚定时翻转电平给FPGA喂狗
 * - FPGA看门狗超时时间: 8秒
 * - 喂狗间隔: 3秒 (小于超时时间的一半，确保安全裕度)
 * - 使用独立线程定时喂狗
 */

/* D2000 GPIO基地址定义 */
#define GPIO0_BASE              0x28004000
#define GPIO1_BASE              0x28005000
#define PIN_MUX_BASE            0x28180000

/* GPIO寄存器偏移 - PORTA组 */
#define SWPORTA_DR_OFFSET       0x00    /* PORTA数据寄存器 */
#define SWPORTA_DDR_OFFSET      0x04    /* PORTA方向寄存器 */
#define EXT_PORTA_OFFSET        0x08    /* PORTA外部端口寄存器 */

/* GPIO寄存器偏移 - PORTB组 */
#define SWPORTB_DR_OFFSET       0x0C    /* PORTB数据寄存器 */
#define SWPORTB_DDR_OFFSET      0x10    /* PORTB方向寄存器 */
#define EXT_PORTB_OFFSET        0x14    /* PORTB外部端口寄存器 */

/* IOMUX配置 - GPIO0_B6 */
#define GPIO0_B6_MUX_OFFSET     0x0214  /* qspi_csn2_pad寄存器偏移 */
#define GPIO0_B6_FUNC_BIT       8       /* 功能选择位偏移 bit[9:8] */
#define GPIO0_B6_PULL_BIT       10      /* 上下拉位偏移 bit[11:10] */
#define GPIO_FUNCTION_VALUE     0x2     /* GPIO功能值 func2 (bit[9:8]=2'b10) */
#define PULL_DISABLE_VALUE      0x0     /* 上下拉禁用 (bit[11:10]=2'b00) */

/* GPIO方向定义 */
#define GPIO_DIR_INPUT          0
#define GPIO_DIR_OUTPUT         1

/* GPIO电平定义 */
#define GPIO_LEVEL_LOW          0
#define GPIO_LEVEL_HIGH         1

/* 看门狗参数 */
#define WATCHDOG_TIMEOUT_MS         8000    /* FPGA看门狗超时时间: 8秒 */
#define WATCHDOG_FEED_INTERVAL_MS   3000    /* 喂狗间隔: 3秒 */

/* GPIO配置结构体 */
typedef struct {
    uint32_t gpio_base;         /* GPIO控制器基地址 (GPIO0_BASE/GPIO1_BASE) */
    uint32_t pin_mux_offset;    /* PIN复用寄存器偏移（相对于PIN_MUX_BASE） */
    uint32_t mux_func_bit;      /* 功能选择位偏移 */
    uint32_t mux_pull_bit;      /* 上下拉位偏移 */
    uint32_t port;              /* GPIO端口号 (0-7 for PORTB) */
    uint32_t use_portb;         /* 是否使用PORTB组 (1=PORTB, 0=PORTA) */
} gpio_config_t;

/* 看门狗管理器 */
typedef struct {
    uint32_t gpio_base;             /* GPIO控制器基地址 */
    uint32_t pin_mux_offset;        /* PIN复用寄存器偏移 */
    uint32_t mux_func_bit;          /* 功能选择位偏移 */
    uint32_t mux_pull_bit;          /* 上下拉位偏移 */
    uint32_t port;                  /* GPIO端口号 */
    uint32_t use_portb;             /* 是否使用PORTB组 */
    uint32_t feed_count;            /* 喂狗计数 */
    bool enabled;                   /* 看门狗是否启用 */
    pthread_t thread;               /* 喂狗线程 */
    pthread_mutex_t mutex;          /* 互斥锁 */
} fpga_watchdog_manager_t;

/* 初始化FPGA看门狗 */
int fpga_watchdog_init(uint32_t gpio_base, uint32_t pin_mux_offset,
                       uint32_t mux_func_bit, uint32_t mux_pull_bit,
                       uint32_t port, uint32_t use_portb);

/* 启动FPGA看门狗喂狗线程 */
int fpga_watchdog_start(void);

/* 停止FPGA看门狗喂狗线程 */
int fpga_watchdog_stop(void);

/* 检查看门狗是否运行 */
bool fpga_watchdog_is_running(void);

/* 销毁FPGA看门狗 */
void fpga_watchdog_destroy(void);

#endif /* FPGA_WATCHDOG_H */
