# 参数查询模块说明

## 概述

参数查询模块实现了BBU对PAAU各类运行参数的查询功能,支持系统时间、CPU占用率、温度、功率、延时等参数的查询。

## 功能特性

### 1. 支持的查询类型

根据CPRI V1.6协议,实现以下参数查询:

| 查询IE | IE ID | 响应IE | IE ID | 数据来源 | 说明 |
|--------|-------|--------|-------|----------|------|
| 系统时间查询 | 401 | 系统时间响应 | 551 | Linux系统 | 查询结果 |
| CPU占用率查询 | 402 | CPU占用率响应 | 451 | /proc/stat | D2000 CPU占用率 |
| CPU统计周期查询 | 403 | CPU统计周期响应 | 452 | 配置 | 统计周期(秒) |
| 相控阵温度查询 | 404 | 相控阵温度响应 | 453 | FPGA遥测 | 指定测温点温度 |
| 过温门限查询 | 407 | 过温门限响应 | 456 | FPGA遥测 | 指定测温点门限 |
| 输出功率查询 | 408 | 输出功率响应 | 457 | FPGA遥测 | 当前输出功率 |
| Toffset查询 | 409 | Toffset响应 | 458 | FPGA遥测 | CPRI Toffset值 |
| T2a查询 | 410 | T2a响应 | 459 | FPGA遥测 | CPRI T2a值 |
| Ta3查询 | 411 | Ta3响应 | 460 | FPGA遥测 | CPRI Ta3值 |

### 2. 消息格式

#### 查询请求 (MsgID: 51)
- 方向: BBU -> PAAU
- 可包含多个查询IE
- 无payload的IE只有4字节头部

#### 查询响应 (MsgID: 52)
- 方向: PAAU -> BBU
- 返回相同的流水号
- 包含对应的响应IE

### 3. CPU占用率获取

从Linux系统的`/proc/stat`文件读取CPU时间统计:

```c
static uint32_t read_cpu_usage(void)
{
    // 读取 /proc/stat
    // 解析: cpu user nice system idle iowait irq softirq
    // 计算: usage = 100 * (total_diff - idle_diff) / total_diff
}
```

**特点**:
- 实时读取D2000处理器的CPU占用率
- 使用差值计算,避免瞬时波动
- 默认5秒统计周期
- 返回0-100的百分比值

### 4. FPGA参数获取

所有FPGA相关参数从FPGA遥测数据获取:

```c
const fpga_status_frame_t *fpga_status = fpga_handler_get_status();
```

**温度查询**:
- 测温点0: `fpga_status->data.temp1`
- 测温点1: `fpga_status->data.temp2`
- 测温点2: `fpga_status->data.temp3`
- 温度值为有符号整数(int8_t), 单位:摄氏度

**过温门限查询**:
- 测温点0: `temp1_high`, `temp1_low`
- 测温点1: `temp2_high`, `temp2_low`
- 测温点2: `temp3_high`, `temp3_low`
- 门限值为有符号整数(int8_t), 单位:摄氏度

**输出功率查询**:
- 字段: `current_output_power`
- 单位: 1/256 dBm
- 例如: 值为256表示1 dBm

**延时参数查询**:
- Toffset: `fpga_status->data.toffset`
- T2a: `fpga_status->data.t2a`
- Ta3: `fpga_status->data.ta3`

## API接口

### 初始化和清理

```c
// 初始化参数查询模块
int param_query_init(void);

// 清理参数查询模块
void param_query_cleanup(void);
```

### 主处理函数

```c
// 处理参数查询请求
int param_query_handle_request(const cpri_message_t *request, 
                                cpri_message_t *response);
```

**功能**:
- 解析请求中的所有查询IE
- 调用对应的处理函数
- 构造响应消息
- 返回相同的流水号

### CPU相关接口

```c
// 获取CPU占用率 (0-100)
uint32_t param_query_get_cpu_usage(void);

// 获取CPU统计周期 (秒)
uint32_t param_query_get_cpu_period(void);

// 更新CPU统计信息
void param_query_update_cpu_stats(uint32_t usage);
```

### 各参数查询处理函数

```c
// 系统时间查询
int param_query_handle_system_time(cpri_message_t *response);

// CPU占用率查询
int param_query_handle_cpu_usage(cpri_message_t *response);

// CPU统计周期查询
int param_query_handle_cpu_period(cpri_message_t *response);

// 相控阵温度查询
int param_query_handle_temperature(uint8_t temp_point, cpri_message_t *response);

// 过温门限查询
int param_query_handle_temp_threshold(uint8_t temp_point, cpri_message_t *response);

// 输出功率查询
int param_query_handle_output_power(uint16_t rf_channel, cpri_message_t *response);

// Toffset查询
int param_query_handle_toffset(cpri_message_t *response);

// T2a查询
int param_query_handle_t2a(cpri_message_t *response);

// Ta3查询
int param_query_handle_ta3(cpri_message_t *response);
```

## 使用示例

### 示例1: 处理查询请求

```c
// 在消息处理器中
int handle_param_query(const cpri_message_t *msg)
{
    cpri_message_t response;
    
    // 处理查询请求
    int ret = param_query_handle_request(msg, &response);
    if (ret != SUCCESS) {
        return ret;
    }
    
    // 编码并发送响应
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
    }
    
    cpri_free_message(&response);
    return SUCCESS;
}
```

### 示例2: 查询CPU占用率

```c
// 获取当前CPU占用率
uint32_t cpu_usage = param_query_get_cpu_usage();
printf("CPU Usage: %u%%\n", cpu_usage);
```

### 示例3: 查询温度

```c
// 获取FPGA状态
const fpga_status_frame_t *fpga_status = fpga_handler_get_status();

// 读取温度
int8_t temp1 = fpga_status->data.temp1;
int8_t temp2 = fpga_status->data.temp2;
int8_t temp3 = fpga_status->data.temp3;

printf("Temperature: %d°C, %d°C, %d°C\n", temp1, temp2, temp3);
```

## 数据结构

### 查询请求IE

```c
/* IE 404: 相控阵温度查询 */
typedef struct {
    uint8_t temp_point;  // 测温点索引 (0~N)
} temp_query_ie_t;

/* IE 407: 过温门限查询 */
typedef struct {
    uint8_t temp_point;  // 测温点索引 (0~N)
} temp_threshold_query_ie_t;

/* IE 408: 输出功率查询 */
typedef struct {
    uint16_t rf_channel;  // 射频通道号
} output_power_query_ie_t;
```

### 查询响应IE

```c
/* IE 451: CPU占用率响应 */
typedef struct {
    uint32_t cpu_usage;  // CPU占用率 (0-100)
} cpu_usage_resp_ie_t;

/* IE 453: 相控阵温度响应 */
typedef struct {
    uint8_t temp_point;   // 测温点索引
    int8_t temperature;   // 温度值 (摄氏度, 有符号)
} temp_resp_ie_t;

/* IE 456: 过温门限响应 */
typedef struct {
    uint8_t temp_point;    // 测温点索引
    int8_t up_threshold;   // 温度上门限
    int8_t low_threshold;  // 温度下门限
} temp_threshold_resp_ie_t;

/* IE 457: 输出功率响应 */
typedef struct {
    uint8_t reserved;   // 保留
    uint16_t power;     // 输出功率 (1/256 dBm)
} output_power_resp_ie_t;
```

## 集成说明

### 1. 消息处理器集成

在`msg_handler.c`中:

```c
#include "param_query.h"

int handle_param_query(const cpri_message_t *msg)
{
    cpri_message_t response;
    param_query_handle_request(msg, &response);
    
    // 发送响应
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    tcp_client_send(&g_tcp_client, buffer, len);
    
    cpri_free_message(&response);
    return SUCCESS;
}
```

### 2. 主程序集成

在`main.c`中:

```c
#include "param_query.h"

int main()
{
    // 初始化参数查询模块
    param_query_init();
    
    // ... 其他初始化 ...
    
    // 清理
    param_query_cleanup();
}
```

## 日志输出

参数查询模块会输出以下日志:

```
[INFO] Parameter query module initialized
[INFO] Handle param query (serial=123)
[DEBUG] Added CPU usage response IE: 25%
[DEBUG] Added temperature response IE: point=0, temp=45°C
[DEBUG] Added temp threshold response IE: point=0, high=85°C, low=-40°C
[INFO] Processed parameter query request: 3 IEs
[INFO] Sent param query response (serial=123, 256 bytes)
```

## 性能考虑

- CPU占用率读取: 约1ms (读取/proc/stat)
- FPGA参数获取: 约10μs (内存访问)
- 响应消息构造: 约100μs
- 总处理时间: < 2ms

## 测试建议

### 测试1: CPU占用率查询

1. 发送查询请求(IE 402)
2. 验证响应中CPU占用率在0-100范围内
3. 对比`top`命令的CPU占用率

### 测试2: 温度查询

1. 发送温度查询请求(IE 404, temp_point=0)
2. 验证响应中温度值为有符号整数
3. 对比FPGA遥测数据

### 测试3: 批量查询

1. 发送包含多个查询IE的请求
2. 验证响应中包含所有对应的响应IE
3. 验证流水号正确返回

### 测试4: 无效参数

1. 发送无效测温点查询(temp_point=99)
2. 验证响应中返回默认值或错误

## 注意事项

1. CPU占用率是D2000处理器的占用率,不是FPGA的
2. 温度值为有符号整数,支持负温度
3. 输出功率单位为1/256 dBm,需要转换
4. 所有FPGA参数依赖FPGA遥测数据的有效性
5. 查询响应必须返回相同的流水号
6. 支持一次查询多个参数

## 扩展建议

1. 添加更多测温点支持(当前支持0-2)
2. 添加CPU温度查询
3. 添加内存使用率查询
4. 添加网络流量统计查询
5. 添加参数缓存机制,减少重复读取
