# FPGA看门狗功能说明

## 一、功能概述

通过GPIO引脚定时翻转电平给FPGA喂狗，防止FPGA看门狗超时复位。

- FPGA看门狗超时时间: 8秒
- 喂狗间隔: 3秒 (小于超时时间的一半，确保安全裕度)
- 使用独立线程自动喂狗

## 二、配置说明

在配置文件 `config/antenna_mgmt.conf` 中配置GPIO参数：

```ini
# FPGA看门狗GPIO配置
WATCHDOG_GPIO_BASE=0x28004000        # GPIO控制器基地址
WATCHDOG_PIN_MUX_REG=0x28180100      # PIN复用寄存器地址
WATCHDOG_MUX_BIT_OFFSET=0            # 复用配置位偏移
WATCHDOG_GPIO_PORT=10                # GPIO端口号 (0-31)
```

**说明：**
- `GPIO0_BASE = 0x28004000`
- `GPIO1_BASE = 0x28005000`
- `PIN_MUX_BASE = 0x28180000`
- 根据实际硬件连接配置对应的GPIO端口和复用寄存器

## 三、工作原理

### 1. 初始化阶段
- 配置GPIO引脚复用为GPIO功能
- 设置GPIO为输出模式
- 初始化GPIO输出为低电平

### 2. 运行阶段
- 创建独立线程，每3秒执行一次喂狗操作
- 喂狗操作：翻转GPIO电平 (高->低 或 低->高)
- FPGA检测到GPIO电平变化，重置看门狗计数器

### 3. 退出阶段
- 停止喂狗线程
- 清理资源

## 四、代码集成

### 1. 头文件
- `include/fpga_watchdog.h` - 看门狗接口定义
- `include/d2000_gpio.h` - D2000 GPIO寄存器定义

### 2. 源文件
- `src/fpga_watchdog.c` - 看门狗实现
- `src/d2000_gpio.c` - GPIO底层操作

### 3. 主程序集成 (main.c)
- 初始化: `fpga_watchdog_init()`
- 启动: `fpga_watchdog_start()`
- 停止: `fpga_watchdog_stop()`
- 销毁: `fpga_watchdog_destroy()`

## 五、API接口

### fpga_watchdog_init()
```c
int fpga_watchdog_init(uint32_t gpio_base, uint32_t pin_mux_reg,
                       uint32_t mux_bit_offset, uint32_t port);
```
- **功能**: 初始化FPGA看门狗
- **返回**: `SUCCESS(0)` 成功, `ERROR_GENERAL(-1)` 失败

### fpga_watchdog_start()
```c
int fpga_watchdog_start(void);
```
- **功能**: 启动看门狗喂狗线程
- **返回**: `SUCCESS(0)` 成功, `ERROR_GENERAL(-1)` 失败

### fpga_watchdog_stop()
```c
int fpga_watchdog_stop(void);
```
- **功能**: 停止看门狗喂狗线程
- **返回**: `SUCCESS(0)` 成功, `ERROR_GENERAL(-1)` 失败

### fpga_watchdog_is_running()
```c
bool fpga_watchdog_is_running(void);
```
- **功能**: 检查看门狗是否运行
- **返回**: `true` 运行中, `false` 未运行

### fpga_watchdog_destroy()
```c
void fpga_watchdog_destroy(void);
```
- **功能**: 销毁看门狗管理器

## 六、日志输出

### 初始化成功
```
[INFO] Initializing FPGA watchdog...
[INFO] FPGA watchdog initialized successfully
[INFO] FPGA watchdog thread started
```

### 喂狗操作
```
[DEBUG] Watchdog fed (count=1)
[DEBUG] Watchdog fed (count=2)
...
```

### 停止
```
[INFO] FPGA watchdog thread stopped
[INFO] FPGA watchdog manager destroyed
```

## 七、注意事项

1. 需要root权限访问 `/dev/mem`
2. GPIO配置必须与硬件实际连接一致
3. 看门狗初始化失败不会导致程序退出，仅记录警告日志
4. 喂狗线程独立运行，不影响主程序逻辑
5. 程序退出时会自动停止喂狗线程

## 八、故障排查

### 1. 初始化失败
- 检查是否有root权限
- 检查GPIO配置参数是否正确
- 查看日志中的错误信息

### 2. 喂狗失败
- 检查GPIO硬件连接
- 检查FPGA是否正常工作
- 查看日志中的警告信息

### 3. FPGA仍然复位
- 确认喂狗间隔小于FPGA看门狗超时时间
- 检查GPIO信号是否正常翻转
- 使用示波器测量GPIO引脚波形
