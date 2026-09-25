#include "fpga_watchdog.h"
#include "logger.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

/* 全局看门狗管理器 */
static fpga_watchdog_manager_t g_watchdog_mgr;

/* g_watchdog_mgr.mutex 是否处于"已初始化且尚未销毁"的状态。
 * 必要性：init 在 GPIO 初始化失败时会把 mutex 销毁再返回失败（见 init 内），
 * 而 main.c 把它当非致命错误继续运行，清理阶段又无条件调用 stop/destroy ——
 * 对已销毁（或从未初始化）的 mutex 加锁/再销毁都是 UB。
 * 所有触及该 mutex 的入口都先查这个标志。 */
static bool g_watchdog_mutex_ready = false;

/* GPIO配置 */
static gpio_config_t g_watchdog_gpio;

/* 内存映射文件描述符 */
static int mem_fd = -1;

/* GPIO寄存器映射地址（保持映射，避免频繁mmap/munmap） */
static void *gpio_mapped_base = NULL;

/* IOMUX寄存器映射地址 */
static void *iomux_mapped_base = NULL;

/* 寄存器读写宏 */
#define REG_READ(addr)          (*((volatile uint32_t *)(addr)))
#define REG_WRITE(addr, val)    (*((volatile uint32_t *)(addr)) = (val))

/**
 * 内存映射初始化
 */
static void* mmap_init(uint32_t base_addr, uint32_t size)
{
    void *mapped_base;

    if (mem_fd == -1) {
        mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mem_fd == -1) {
            LOG_ERROR("Failed to open /dev/mem");
            return NULL;
        }
    }

    mapped_base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       mem_fd, base_addr);
    if (mapped_base == MAP_FAILED) {
        LOG_ERROR("Failed to mmap");
        return NULL;
    }

    return mapped_base;
}

/**
 * 配置IOMUX，将GPIO引脚设置为GPIO功能
 */
static int configure_iomux(gpio_config_t *config)
{
    uint32_t mux_val;
    volatile uint32_t *mux_reg;

    if (!config) {
        LOG_ERROR("Invalid GPIO config");
        return ERROR_GENERAL;
    }

    LOG_INFO("Configuring IOMUX for GPIO");
    LOG_INFO("  IOMUX register address: 0x%08X", PIN_MUX_BASE + config->pin_mux_offset);

    /* 映射IOMUX寄存器 */
    if (!iomux_mapped_base) {
        iomux_mapped_base = mmap_init(PIN_MUX_BASE, 0x1000);
        if (!iomux_mapped_base) {
            LOG_ERROR("Failed to map IOMUX registers");
            return ERROR_GENERAL;
        }
    }

    /* 读取当前IOMUX配置 */
    mux_reg = (volatile uint32_t *)((uint8_t *)iomux_mapped_base + config->pin_mux_offset);
    mux_val = REG_READ(mux_reg);
    LOG_DEBUG("  Current IOMUX value: 0x%08X", mux_val);

    /* 清除功能选择位和上下拉位 [11:8] */
    mux_val &= ~(0xF << config->mux_func_bit);

    /* 设置bit[9:8] = 0b10 (2) 选择GPIO功能 */
    mux_val |= (GPIO_FUNCTION_VALUE << config->mux_func_bit);

    /* bit[11:10] = 0b00 (0) 禁用上下拉 (已经被清除了) */

    /* 写入新配置 */
    REG_WRITE(mux_reg, mux_val);

    /* 延迟确保配置生效 */
    usleep(10000);  // 10ms

    /* 读回验证 */
    uint32_t verify_val = REG_READ(mux_reg);
    LOG_DEBUG("  New IOMUX value: 0x%08X", verify_val);
    LOG_DEBUG("  Function bits [9:8]: 0x%X (should be 0x2)",
              (verify_val >> config->mux_func_bit) & 0x3);
    LOG_DEBUG("  Pull bits [11:10]: 0x%X (should be 0x0)",
              (verify_val >> config->mux_pull_bit) & 0x3);

    if (verify_val != mux_val) {
        LOG_WARN("IOMUX verification mismatch");
        return ERROR_GENERAL;
    }

    LOG_INFO("  IOMUX configured successfully");
    return SUCCESS;
}

/**
 * GPIO初始化
 */
static int gpio_init(gpio_config_t *config)
{
    int ret;

    if (!config) {
        LOG_ERROR("Invalid GPIO config");
        return ERROR_GENERAL;
    }

    /* 配置IOMUX */
    ret = configure_iomux(config);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to configure IOMUX");
        return ERROR_GENERAL;
    }

    LOG_DEBUG("GPIO initialized: base=0x%x, port=%d, use_portb=%d",
              config->gpio_base, config->port, config->use_portb);

    return SUCCESS;
}

/**
 * 设置GPIO方向
 */
static int gpio_set_direction(gpio_config_t *config, uint32_t direction)
{
    uint32_t temp;
    uint32_t ddr_offset;

    if (!config) {
        return ERROR_GENERAL;
    }

    /* 验证GPIO基地址是否合法 */
    if (config->gpio_base != GPIO0_BASE && config->gpio_base != GPIO1_BASE) {
        LOG_ERROR("Invalid GPIO base address: 0x%x", config->gpio_base);
        return ERROR_GENERAL;
    }

    /* 使用持久化的GPIO映射 */
    if (!gpio_mapped_base) {
        LOG_ERROR("GPIO base not mapped");
        return ERROR_GENERAL;
    }

    /* 根据使用的PORT组选择对应的DDR寄存器 */
    ddr_offset = config->use_portb ? SWPORTB_DDR_OFFSET : SWPORTA_DDR_OFFSET;
    volatile uint32_t *ddr_reg = (volatile uint32_t *)((uint8_t *)gpio_mapped_base + ddr_offset);

    temp = REG_READ(ddr_reg);

    if (direction == GPIO_DIR_OUTPUT) {
        temp |= (1 << config->port);
    } else {
        temp &= ~(1 << config->port);
    }

    REG_WRITE(ddr_reg, temp);

    return SUCCESS;
}

/**
 * 设置GPIO输出值
 */
static int gpio_set_value(gpio_config_t *config, uint32_t value)
{
    uint32_t temp;
    uint32_t dr_offset;

    if (!config) {
        return ERROR_GENERAL;
    }

    /* 验证GPIO基地址是否合法 */
    if (config->gpio_base != GPIO0_BASE && config->gpio_base != GPIO1_BASE) {
        LOG_ERROR("Invalid GPIO base address: 0x%x", config->gpio_base);
        return ERROR_GENERAL;
    }

    /* 使用持久化的GPIO映射 */
    if (!gpio_mapped_base) {
        LOG_ERROR("GPIO base not mapped");
        return ERROR_GENERAL;
    }

    /* 根据使用的PORT组选择对应的DR寄存器 */
    dr_offset = config->use_portb ? SWPORTB_DR_OFFSET : SWPORTA_DR_OFFSET;
    volatile uint32_t *dr_reg = (volatile uint32_t *)((uint8_t *)gpio_mapped_base + dr_offset);

    temp = REG_READ(dr_reg);

    if (value == GPIO_LEVEL_HIGH) {
        temp |= (1 << config->port);
    } else {
        temp &= ~(1 << config->port);
    }

    REG_WRITE(dr_reg, temp);

    return SUCCESS;
}

/**
 * 看门狗GPIO初始化
 */
static int watchdog_gpio_init(gpio_config_t *config)
{
    int ret;

    LOG_DEBUG("Initializing watchdog GPIO");

    /* 映射GPIO寄存器基地址（保持映射，用于后续所有GPIO操作） */
    gpio_mapped_base = mmap_init(config->gpio_base, 0x1000);
    if (!gpio_mapped_base) {
        LOG_ERROR("Failed to map GPIO base for watchdog");
        return ERROR_GENERAL;
    }

    /* 初始化GPIO */
    ret = gpio_init(config);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to init GPIO");
        return ERROR_GENERAL;
    }

    /* 设置为输出模式 */
    ret = gpio_set_direction(config, GPIO_DIR_OUTPUT);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to set GPIO direction");
        return ERROR_GENERAL;
    }

    /* 初始化为低电平 */
    ret = gpio_set_value(config, GPIO_LEVEL_LOW);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to set GPIO value");
        return ERROR_GENERAL;
    }

    LOG_DEBUG("Watchdog GPIO initialized successfully");

    return SUCCESS;
}

/**
 * 喂狗操作（优化版：使用持久化的内存映射）
 */
static int watchdog_feed(gpio_config_t *config)
{
    uint32_t current_value, new_value;
    uint32_t dr_offset;

    if (!config || !gpio_mapped_base) {
        return ERROR_GENERAL;
    }

    /* 根据使用的PORT组选择对应的DR寄存器 */
    dr_offset = config->use_portb ? SWPORTB_DR_OFFSET : SWPORTA_DR_OFFSET;
    volatile uint32_t *dr_reg = (volatile uint32_t *)((uint8_t *)gpio_mapped_base + dr_offset);

    /* 读取当前GPIO输出值 */
    current_value = REG_READ(dr_reg);

    /* 翻转GPIO电平 */
    if (current_value & (1 << config->port)) {
        /* 当前为高电平，翻转为低电平 */
        new_value = current_value & ~(1 << config->port);
    } else {
        /* 当前为低电平，翻转为高电平 */
        new_value = current_value | (1 << config->port);
    }

    REG_WRITE(dr_reg, new_value);

    return SUCCESS;
}

/**
 * 看门狗线程函数
 */
static void* watchdog_thread_func(void *arg)
{
    (void)arg;  /* 未使用参数 */

    LOG_INFO("FPGA watchdog thread started, feed interval: %d ms", WATCHDOG_FEED_INTERVAL_MS);

    while (1) {
        pthread_mutex_lock(&g_watchdog_mgr.mutex);

        if (!g_watchdog_mgr.enabled) {
            pthread_mutex_unlock(&g_watchdog_mgr.mutex);
            break;
        }

        pthread_mutex_unlock(&g_watchdog_mgr.mutex);

        /* 喂狗操作 */
        if (watchdog_feed(&g_watchdog_gpio) != 0) {
            LOG_WARN("Failed to feed watchdog");
        } else {
            pthread_mutex_lock(&g_watchdog_mgr.mutex);
            g_watchdog_mgr.feed_count++;
            pthread_mutex_unlock(&g_watchdog_mgr.mutex);

           // LOG_DEBUG("Watchdog fed (count=%u)", g_watchdog_mgr.feed_count);
        }

        /* 等待下一次喂狗 */
        usleep(WATCHDOG_FEED_INTERVAL_MS * 1000);
    }

    LOG_INFO("FPGA watchdog thread stopped");
    return NULL;
}

int fpga_watchdog_init(uint32_t gpio_base, uint32_t pin_mux_offset,
                       uint32_t mux_func_bit, uint32_t mux_pull_bit,
                       uint32_t port, uint32_t use_portb)
{
    memset(&g_watchdog_mgr, 0, sizeof(g_watchdog_mgr));

    if (pthread_mutex_init(&g_watchdog_mgr.mutex, NULL) != 0) {
        LOG_ERROR("Failed to init watchdog mutex");
        return ERROR_GENERAL;
    }
    g_watchdog_mutex_ready = true;

    LOG_INFO("Initializing FPGA watchdog...");
    LOG_INFO("  GPIO base: 0x%08x", gpio_base);
    LOG_INFO("  PIN mux offset: 0x%03x", pin_mux_offset);
    LOG_INFO("  Mux func bit: %d", mux_func_bit);
    LOG_INFO("  Mux pull bit: %d", mux_pull_bit);
    LOG_INFO("  Port: %d", port);
    LOG_INFO("  Use PORTB: %d", use_portb);

    /* 保存配置参数 */
    g_watchdog_mgr.gpio_base = gpio_base;
    g_watchdog_mgr.pin_mux_offset = pin_mux_offset;
    g_watchdog_mgr.mux_func_bit = mux_func_bit;
    g_watchdog_mgr.mux_pull_bit = mux_pull_bit;
    g_watchdog_mgr.port = port;
    g_watchdog_mgr.use_portb = use_portb;
    g_watchdog_mgr.enabled = false;
    g_watchdog_mgr.feed_count = 0;

    /* 配置GPIO参数 */
    g_watchdog_gpio.gpio_base = gpio_base;
    g_watchdog_gpio.pin_mux_offset = pin_mux_offset;
    g_watchdog_gpio.mux_func_bit = mux_func_bit;
    g_watchdog_gpio.mux_pull_bit = mux_pull_bit;
    g_watchdog_gpio.port = port;
    g_watchdog_gpio.use_portb = use_portb;

    /* 初始化GPIO */
    int ret = watchdog_gpio_init(&g_watchdog_gpio);
    if (ret != 0) {
        LOG_ERROR("Failed to initialize watchdog GPIO");
        pthread_mutex_destroy(&g_watchdog_mgr.mutex);
        g_watchdog_mutex_ready = false;   /* 已销毁，后续 stop/destroy 不得再碰它 */
        return ERROR_GENERAL;
    }

    LOG_INFO("FPGA watchdog initialized successfully");
    return SUCCESS;
}

int fpga_watchdog_start(void)
{
    if (!g_watchdog_mutex_ready) {
        LOG_ERROR("Watchdog not initialized, cannot start");
        return ERROR_GENERAL;
    }

    pthread_mutex_lock(&g_watchdog_mgr.mutex);

    if (g_watchdog_mgr.enabled) {
        pthread_mutex_unlock(&g_watchdog_mgr.mutex);
        LOG_WARN("Watchdog thread already running");
        return SUCCESS;
    }

    g_watchdog_mgr.enabled = true;
    g_watchdog_mgr.feed_count = 0;

    pthread_mutex_unlock(&g_watchdog_mgr.mutex);

    int ret = pthread_create(&g_watchdog_mgr.thread, NULL, watchdog_thread_func, NULL);
    if (ret != 0) {
        LOG_ERROR("Failed to create watchdog thread");
        g_watchdog_mgr.enabled = false;
        return ERROR_GENERAL;
    }

    LOG_INFO("FPGA watchdog thread started");
    return SUCCESS;
}

int fpga_watchdog_stop(void)
{
    if (!g_watchdog_mutex_ready) {
        /* 未初始化或已销毁：没有线程可停，安全返回 */
        return SUCCESS;
    }

    pthread_mutex_lock(&g_watchdog_mgr.mutex);

    if (!g_watchdog_mgr.enabled) {
        pthread_mutex_unlock(&g_watchdog_mgr.mutex);
        LOG_WARN("Watchdog thread not running");
        return SUCCESS;
    }

    g_watchdog_mgr.enabled = false;

    pthread_mutex_unlock(&g_watchdog_mgr.mutex);

    /* 等待线程退出 */
    pthread_join(g_watchdog_mgr.thread, NULL);

    LOG_INFO("FPGA watchdog thread stopped");
    return SUCCESS;
}

bool fpga_watchdog_is_running(void)
{
    if (!g_watchdog_mutex_ready) {
        return false;
    }

    pthread_mutex_lock(&g_watchdog_mgr.mutex);
    bool running = g_watchdog_mgr.enabled;
    pthread_mutex_unlock(&g_watchdog_mgr.mutex);
    return running;
}

void fpga_watchdog_destroy(void)
{
    /* 先停喂狗线程并 join，再解除映射。
     * 否则线程可能仍在 watchdog_feed() 里访问已 munmap 的地址（use-after-unmap）。 */
    fpga_watchdog_stop();

    /* 释放GPIO内存映射 */
    if (gpio_mapped_base) {
        munmap(gpio_mapped_base, 0x1000);
        gpio_mapped_base = NULL;
    }

    /* 释放IOMUX内存映射 */
    if (iomux_mapped_base) {
        munmap(iomux_mapped_base, 0x1000);
        iomux_mapped_base = NULL;
    }

    /* 关闭内存设备 */
    if (mem_fd != -1) {
        close(mem_fd);
        mem_fd = -1;
    }

    /* 只在 mutex 确实处于已初始化状态时才销毁，并置标志使本函数可安全重复调用 */
    if (g_watchdog_mutex_ready) {
        pthread_mutex_destroy(&g_watchdog_mgr.mutex);
        g_watchdog_mutex_ready = false;
    }
    LOG_INFO("FPGA watchdog manager destroyed");
}
