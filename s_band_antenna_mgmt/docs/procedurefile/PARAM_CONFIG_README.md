# 参数配置模块 (Parameter Configuration)

## 概述

参数配置模块实现了BBU向PAAU下发配置参数的功能，包括系统时间配置、CPU统计周期配置和CPRI工作模式配置。

## 功能特性

### 1. 系统时间配置 (IE 11)
- **功能**: 配置Linux系统时间
- **实现**: 使用`settimeofday()`系统调用设置系统时间
- **参数验证**:
  - 秒: 0-59
  - 分: 0-59
  - 时: 0-23
  - 日: 1-31
  - 月: 1-12
  - 年: 如2024
- **响应**: IE 551 (配置结果: 0成功, 1失败)

### 2. CPU占用率统计周期配置 (IE 502)
- **功能**: 配置CPU使用率统计的采样周期
- **实现思路**:
  - 周期值保存到全局变量`g_cpu_stat_period`
  - `param_query.c`中的`read_cpu_usage()`通过`param_config_get_cpu_period()`获取周期
  - 该周期决定两次`/proc/stat`读取之间的时间间隔
- **合理范围**:
  - 最小值: 1秒 (太小会增加系统开销)
  - 最大值: 3600秒 (太大会导致统计不及时)
  - 推荐值: 60秒 (平衡精度和开销)
- **响应**: IE 553 (配置结果: 0成功, 1失败)

### 3. CPRI口工作模式配置 (IE 504)
- **功能**: 配置CPRI接口的工作模式
- **实现**: 通过`fpga_handler_set_work_mode()`发送给FPGA
- **工作模式**:
  - 0: 普通模式
  - 1: 级联模式
  - 2: 主备模式
  - 3: 负荷分担模式
- **响应**: IE 555 (主/辅光口号 + 配置结果)
  - 普通模式: 辅光口填无效值(0xFF)
  - 级联模式: 辅光口填无效值(0xFF)
  - 主备模式: 辅光口填备光口号(如可用)
  - 负荷分担模式: 辅光口填辅光口号(如可用)

## 消息流程

```
BBU                                    PAAU
 |                                      |
 |  参数配置请求 (MsgID: 61)             |
 |  - IE 11: 系统时间                    |
 |  - IE 502: CPU统计周期                |
 |  - IE 504: CPRI工作模式               |
 |------------------------------------->|
 |                                      | 1. 设置系统时间
 |                                      | 2. 保存CPU周期
 |                                      | 3. 发送模式给FPGA
 |                                      |
 |  参数配置响应 (MsgID: 62)             |
 |  - IE 551: 时间配置结果               |
 |  - IE 553: CPU周期配置结果            |
 |  - IE 555: CPRI模式配置结果           |
 |<-------------------------------------|
 |                                      |
```

## API 接口

### 初始化
```c
int param_config_init(void);
```

### 消息处理
```c
int param_config_handle_request(const cpri_message_t *request,
                                cpri_message_t *response);
```

### 配置函数
```c
// 配置系统时间
int param_config_set_system_time(const system_time_config_ie_t *config_ie);

// 配置CPU统计周期
int param_config_set_cpu_period(const cpu_period_config_ie_t *config_ie);

// 配置CPRI工作模式
int param_config_set_cpri_mode(const cpri_work_mode_config_ie_t *config_ie);

// 获取当前CPU统计周期
uint32_t param_config_get_cpu_period(void);
```

## 数据结构

### 配置请求IE

```c
/* IE 11: 系统时间配置 */
typedef struct __attribute__((packed)) {
    uint16_t validity;
    uint16_t ie_id;
    uint16_t ie_length;
    uint8_t  second;    /* 0-59 */
    uint8_t  minute;    /* 0-59 */
    uint8_t  hour;      /* 0-23 */
    uint8_t  day;       /* 1-31 */
    uint8_t  month;     /* 1-12 */
    uint16_t year;      /* 如2024 */
} system_time_config_ie_t;

/* IE 502: CPU占用率统计周期配置 */
typedef struct __attribute__((packed)) {
    uint16_t validity;
    uint16_t ie_id;
    uint16_t ie_length;
    uint32_t period;    /* 统计周期(秒) */
} cpu_period_config_ie_t;

/* IE 504: CPRI口工作模式配置 */
typedef struct __attribute__((packed)) {
    uint16_t validity;
    uint16_t ie_id;
    uint16_t ie_length;
    uint32_t work_mode; /* 0:普通 1:级联 2:主备 3:负荷分担 */
} cpri_work_mode_config_ie_t;
```

### 配置响应IE

```c
/* IE 551: 系统时间配置响应 */
typedef struct __attribute__((packed)) {
    uint16_t validity;
    uint16_t ie_id;
    uint16_t ie_length;
    uint32_t result;    /* 0:成功 1:失败 */
} system_time_config_resp_ie_t;

/* IE 553: CPU占用率统计周期配置响应 */
typedef struct __attribute__((packed)) {
    uint16_t validity;
    uint16_t ie_id;
    uint16_t ie_length;
    uint32_t result;    /* 0:成功 1:失败 */
} cpu_period_config_resp_ie_t;

/* IE 555: CPRI口工作模式配置响应 */
typedef struct __attribute__((packed)) {
    uint16_t validity;
    uint16_t ie_id;
    uint16_t ie_length;
    uint8_t  main_fiber_port;  /* 主光纤端口号 */
    uint8_t  sub_fiber_port;   /* 辅光纤端口号 */
    uint32_t result;           /* 0:成功 1:失败 */
} cpri_work_mode_config_resp_ie_t;
```

## 实现细节

### 系统时间配置
1. 验证时间参数有效性
2. 构造`struct tm`结构
3. 使用`mktime()`转换为`time_t`
4. 使用`settimeofday()`设置系统时间
5. 返回配置结果

### CPU统计周期配置
1. 验证周期范围(1-3600秒)
2. 使用互斥锁保护全局变量
3. 更新`g_cpu_stat_period`
4. `param_query`模块通过`param_config_get_cpu_period()`获取周期
5. 返回配置结果

### CPRI工作模式配置
1. 验证工作模式(0-3)
2. 调用`fpga_handler_set_work_mode()`发送给FPGA
3. FPGA内部处理模式切换
4. 返回配置结果和光口信息

## 线程安全

- CPU统计周期使用互斥锁`g_period_mutex`保护
- 系统时间配置使用系统调用，由内核保证原子性
- FPGA工作模式配置通过UART串行发送，天然串行化

## 错误处理

- 参数验证失败返回`ERROR_INVALID_PARAM`
- 系统调用失败返回`ERROR_OPERATION_FAILED`
- FPGA通信失败返回相应错误码
- 所有错误都会记录日志

## 使用示例

### 在msg_handler中处理配置请求

```c
int handle_param_config(const cpri_message_t *msg)
{
    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    /* 处理参数配置请求 */
    int ret = param_config_handle_request(msg, &response);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to handle param config request: %d", ret);
        return ret;
    }

    /* 编码并发送响应 */
    uint8_t buffer[MAX_CPRI_MSG_SIZE];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
    }

    cpri_free_message(&response);
    return SUCCESS;
}
```

## 测试建议

1. **系统时间配置测试**:
   - 配置有效时间，验证系统时间是否正确设置
   - 配置无效时间(如月份13)，验证是否返回失败
   - 使用`date`命令验证时间

2. **CPU统计周期测试**:
   - 配置不同周期值(1, 60, 3600秒)
   - 验证`param_config_get_cpu_period()`返回正确值
   - 配置超出范围的值，验证是否拒绝

3. **CPRI工作模式测试**:
   - 配置各种工作模式(0-3)
   - 验证FPGA是否收到正确的模式配置
   - 检查响应中的光口信息是否正确

## 注意事项

1. 系统时间配置需要root权限
2. CPU统计周期影响CPU使用率查询的精度
3. CPRI工作模式切换可能影响通信链路
4. 光纤端口信息需要从FPGA实际状态读取(当前使用默认值)
