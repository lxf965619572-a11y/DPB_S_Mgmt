# 告警管理模块说明

## 概述

告警管理模块实现了完整的告警检测、存储和上报功能,支持所有告警类型的自动检测和上报到BBU。

## 功能特性

### 1. 支持的告警类型

根据CPRI V1.6协议表C.1,实现以下告警:

| 告警码 | 告警名称 | 告警子码 | 说明 |
|--------|----------|----------|------|
| 50001 | 本振失锁告警 | 无 | 相控阵自行关闭所有通道 |
| 1097 | 光链路同步码流丢失 | 0-7 (光口号) | 检查光纤连接 |
| 1156 | 过温告警 | 0-9 (温度检测点) | 监测点温度超过门限 |
| 1158 | 版本激活失败 | 无 | BBU指定版本激活失败 |
| 1159 | 通道故障过多 | 无 | 通道故障数超过门限 |
| 1160 | TCP连接断开 | 无 | 与BBU的TCP连接断开(自定义) |

### 2. 告警检测机制

#### 周期性检测
- 从FPGA遥测数据中周期性检测告警状态
- 检测频率: 与FPGA状态轮询频率一致(默认1秒)
- 状态跟踪: 记录每个告警的前一状态,只在状态变化时触发/清除告警

#### 特定告警检测

**本振失锁告警 (50001)**
```c
void alarm_check_lo_unlock(uint32_t lo_lock_status);
```
- 检测条件: `lo_lock_status == 0` 表示失锁
- 数据来源: FPGA遥测字段 `lo_lock_status`

**光链路同步丢失告警 (1097)**
```c
void alarm_check_optical_sync_loss(uint8_t optical_port, bool sync_status);
```
- 检测条件: `sync_status == false` 表示丢失
- 数据来源: FPGA遥测字段 `link_success_flag`
- 子码: 0-3为主路, 4-7为备路

**过温告警 (1156)**
```c
void alarm_check_over_temperature(uint8_t probe_id, int8_t temperature);
```
- 检测条件: `temperature > threshold_high` 或 `temperature < threshold_low`
- 数据来源: FPGA遥测字段 `temp1/temp2/temp3`
- 子码: 0-9表示不同温度探测点
- 默认门限: 高温85°C, 低温-40°C

**通道故障过多告警 (1159)**
```c
void alarm_check_channel_fault_exceed(uint8_t fault_count);
```
- 检测条件: `fault_count > threshold`
- 数据来源: FPGA遥测字段 `channel_fault_count`
- 默认门限: 10个通道

**TCP断开告警 (1160)**
```c
void alarm_trigger_tcp_disconnect(const char *reason);
void alarm_clear_tcp_disconnect(void);
```
- 触发时机: TCP连接断开时
- 清除时机: TCP连接建立时
- 特殊处理: 只有在TCP连接建立后才能上报

### 3. 告警上报机制

#### 上报时机
1. **立即上报**: 告警触发时,如果TCP已连接,立即上报
2. **延迟上报**: 告警触发时,如果TCP未连接,记录为待上报
3. **批量上报**: TCP连接建立后,上报所有待上报的告警

#### 上报消息格式
- 消息ID: 111 (MSG_ALARM_REPORT_REQ)
- IE类型: 1001 (0x3E9)
- IE长度: 138字节 (4字节头 + 134字节数据)

**IE 1001结构**:
```c
typedef struct {
    uint16_t validity;          // 告警有效性 (0:有效, 1:不存在)
    uint32_t alarm_code;        // 告警码
    uint32_t sub_code;          // 告警子码
    uint32_t clear_flag;        // 清除标志 (0:产生, 1:清除)
    char     timestamp[20];     // 时间戳 "yyyy-mm-dd hh:mm:ss"
    char     additional_info[100]; // 附加信息
} alarm_report_ie_t;
```

#### 告警清除上报
- 故障类告警清除时,自动上报清除消息
- `clear_flag = 1` 表示告警已清除

### 4. TCP断开告警特殊处理

TCP断开告警有特殊的处理逻辑:

1. **触发时机**: TCP连接断开时触发
2. **无法立即上报**: 因为TCP已断开,无法发送
3. **延迟上报**: 记录为待上报告警
4. **连接后上报**: TCP重新连接后,立即上报所有待上报告警
5. **自动清除**: TCP连接建立后,自动清除TCP断开告警

## API接口

### 初始化和清理

```c
// 初始化告警管理器
alarm_config_t config = {
    .channel_fault_threshold = 10,
    .over_temp_threshold_high = 85,
    .over_temp_threshold_low = -40
};
int ret = alarm_manager_init(&config);

// 清理告警管理器
void alarm_manager_destroy(void);
```

### 告警操作

```c
// 触发告警
int alarm_trigger(uint32_t alarm_code, uint32_t sub_code, const char *additional_info);

// 清除告警
int alarm_clear(uint32_t alarm_code, uint32_t sub_code);

// 检查告警是否激活
bool alarm_is_active(uint32_t alarm_code, uint32_t sub_code);
```

### TCP状态管理

```c
// 设置TCP连接状态
void alarm_set_tcp_status(bool connected);

// 上报所有待上报的告警
int alarm_report_all_pending(void);
```

### 周期性检测

```c
// 从FPGA遥测数据检测所有告警
void alarm_periodic_check(const fpga_status_frame_t *fpga_status);
```

## 集成说明

### 1. TCP客户端集成

在 `tcp_client.c` 中:

```c
// TCP连接成功时
alarm_set_tcp_status(true);
alarm_clear_tcp_disconnect();

// TCP断开时
alarm_set_tcp_status(false);
alarm_trigger_tcp_disconnect("Connection lost");
```

### 2. 主程序集成

在 `main.c` 中:

```c
// FPGA状态接收回调
static void on_fpga_status_received(const void *status)
{
    const fpga_status_frame_t *fpga_status = (const fpga_status_frame_t*)status;
    
    // 更新FPGA状态
    fpga_handler_on_status_update(fpga_status);
    
    // 周期性告警检测
    alarm_periodic_check(fpga_status);
}
```

### 3. 版本激活失败告警

在版本管理模块中:

```c
// 版本激活失败时
alarm_trigger_version_activate_fail("Version 1.2.3 activation failed");
```

## 配置参数

告警管理器支持以下配置参数:

```c
typedef struct {
    uint8_t channel_fault_threshold;    // 通道故障门限 (默认: 10)
    int8_t over_temp_threshold_high;    // 过温门限高 (默认: 85°C)
    int8_t over_temp_threshold_low;     // 过温门限低 (默认: -40°C)
} alarm_config_t;
```

## 日志输出

告警管理器会输出以下日志:

```
[WARN] Alarm triggered: 本振失锁告警 (code=50001, sub=0) - LO unlock detected
[INFO] Sent alarm report: 本振失锁告警 (code=50001, sub=0, clear=0, serial=123)
[INFO] Alarm cleared: 本振失锁告警 (code=50001, sub=0)
[INFO] TCP connected, reporting pending alarms
[INFO] Reported 3 pending alarms
```

## 线程安全

- 所有告警操作都使用互斥锁保护
- 支持多线程并发访问
- 状态跟踪器独立保护

## 性能考虑

- 告警记录表大小: 64条 (MAX_ALARM_RECORDS)
- 内存占用: 约10KB
- 检测开销: 每秒约100微秒 (取决于FPGA状态轮询频率)

## 测试建议

1. **本振失锁测试**: 模拟FPGA返回 `lo_lock_status = 0`
2. **光链路丢失测试**: 模拟FPGA返回 `link_success_flag` 某位为0
3. **过温测试**: 模拟FPGA返回温度超过门限
4. **TCP断开测试**: 断开TCP连接,验证告警触发和重连后上报
5. **批量告警测试**: 同时触发多个告警,验证上报顺序

## 注意事项

1. TCP断开告警只有在TCP重新连接后才能上报
2. 告警清除消息会自动发送,无需手动调用
3. 状态类告警在状态翻转时才会触发/清除
4. 告警记录表满时,新告警会被拒绝
5. 所有告警操作都是异步的,不会阻塞主线程
