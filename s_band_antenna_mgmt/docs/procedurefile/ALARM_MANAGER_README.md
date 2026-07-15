# 告警上报功能实现文档

## 概述

本文档描述了S频段天线管理软件中告警上报机制的实现，包括告警检测、记录、上报和清除功能。

## 流程概览

告警上报由PAAU主动发起，当检测到硬件或软件故障时触发。

| 步骤 | 消息名称 | 消息ID | 发送方 | 接收方 | 作用 |
|------|----------|--------|--------|--------|------|
| 1 | 告警上报请求 | 111 | PAAU | BBU | PAAU主动上报当前发生的告警信息 |

## 消息定义

### 告警上报请求 (MsgID: 111)

**方向**: PAAU -> BBU

**包含IE**: IE 1001 (0x3E9) - 告警上报请求

#### IE 1001: 告警上报请求 (134字节payload)

```c
typedef struct {
    uint16_t validity;          /* 告警有效性 (0:有效, 1:告警不存在) */
    uint32_t alarm_code;        /* 告警码 */
    uint32_t sub_code;          /* 告警子码 */
    uint32_t clear_flag;        /* 告警清除标志 (0:产生, 1:清除) */
    char     timestamp[20];     /* 告警发生时间 (yyyy-mm-dd hh:mm:ss) */
    char     additional_info[100]; /* 附加信息 */
} alarm_report_ie_t;
```

**字段说明**:
- **Validity** (2字节): 告警有效性 (0:有效, 1:告警不存在)
- **AlarmCode** (4字节): 告警码（见告警码定义表）
- **SubCode** (4字节): 告警子码（辅助定位，如光口号、探针ID）
- **ClearFlag** (4字节): 告警清除标志 (0:告警产生/未清除, 1:告警已清除)
- **Timestamp** (20字节): 告警发生时间（字符串: yyyy-mm-dd hh:mm:ss）
- **AdditionalInfo** (100字节): 附加信息（厂家自定义调试信息）

## 告警码定义表

| 序号 | 告警码 | 告警子码 | 告警名称 | 告警属性 | 处理建议 |
|------|--------|----------|----------|----------|----------|
| 1 | 50001 | (无) | 本振失锁告警 | 故障类 | 相控阵自行关闭所有通道 |
| 2 | 1097 | 0..7 (光口号) | 光链路同步码流丢失 | 故障类 | 检查光纤连接 |
| 3 | 1156 | 0..9 (温度检测点) | 过温告警 | 故障类 | 监测点温度超过过温门限 |
| 4 | 1158 | (无) | 版本激活失败 | 故障类 | BBU指定版本激活失败 |
| 5 | 1159 | (无) | 通道故障过多 | 故障类 | 通道故障数超过门限 |

### 告警属性说明

- **故障类**: 反映设备的某种功能当前处于故障状态（需清除）
- **事件类**: 反映偶发异常，发生即上报，无清除状态
- **状态类**: 状态翻转时上报

## 实现说明

### 文件结构

- **include/alarm_manager.h**: 告警管理模块头文件
- **src/alarm_manager.c**: 告警管理模块实现
- **src/fpga_handler.c**: 添加FPGA状态告警检查
- **src/main.c**: 初始化和销毁告警管理器

### 核心功能

#### 1. 告警管理器

```c
typedef struct {
    alarm_record_t records[MAX_ALARM_RECORDS];  /* 最多32条告警记录 */
    pthread_mutex_t mutex;                      /* 线程安全保护 */
    uint32_t alarm_count;                       /* 当前激活的告警数量 */
} alarm_manager_t;
```

#### 2. 告警记录

```c
typedef struct {
    bool     active;            /* 告警是否激活 */
    uint32_t alarm_code;        /* 告警码 */
    uint32_t sub_code;          /* 告警子码 */
    time_t   start_time;        /* 告警产生时间 */
    time_t   clear_time;        /* 告警清除时间 */
    bool     reported;          /* 是否已上报 */
    char     additional_info[100]; /* 附加信息 */
} alarm_record_t;
```

#### 3. 告警检测

系统在收到FPGA状态更新时自动检测以下告警：

**本振失锁告警** (告警码: 50001):
```c
if (fpga_status->data.lo_lock_status != 0) {
    alarm_raise(ALARM_CODE_LO_UNLOCK, 0, "LO unlock detected");
} else {
    alarm_clear(ALARM_CODE_LO_UNLOCK, 0);
}
```

**过温告警** (告警码: 1156, 子码: 0-2):
```c
/* 温度检测点1 */
if (fpga_status->data.temp1 > fpga_status->data.temp1_high ||
    fpga_status->data.temp1 < fpga_status->data.temp1_low) {
    alarm_raise(ALARM_CODE_OVER_TEMP, 0, "Temp1 out of range");
} else {
    alarm_clear(ALARM_CODE_OVER_TEMP, 0);
}
```

**通道故障过多告警** (告警码: 1159):
```c
if (fpga_status->data.channel_fault_count > 10) {
    alarm_raise(ALARM_CODE_CHANNEL_FAULT, 0, "Too many channel faults");
} else {
    alarm_clear(ALARM_CODE_CHANNEL_FAULT, 0);
}
```

**光链路同步码流丢失** (告警码: 1097, 子码: 0-7):
```c
/* 检查主路和备路 */
for (int i = 0; i < 4; i++) {
    /* 主路 */
    if ((fpga_status->data.link_success_flag & (1 << i)) == 0) {
        alarm_raise(ALARM_CODE_OPTICAL_LOSS, i, "Main optical link lost");
    } else {
        alarm_clear(ALARM_CODE_OPTICAL_LOSS, i);
    }

    /* 备路 */
    if ((fpga_status->data.link_success_flag & (1 << (i + 4))) == 0) {
        alarm_raise(ALARM_CODE_OPTICAL_LOSS, i + 4, "Backup optical link lost");
    } else {
        alarm_clear(ALARM_CODE_OPTICAL_LOSS, i + 4);
    }
}
```

## 工作流程

```
FPGA状态更新
    ↓
fpga_handler_on_status_update()
    ↓
alarm_check_fpga_status()
    ↓
检测各类告警条件
    ↓
告警产生？
    ├─ 是 → alarm_raise()
    │         ├─ 记录告警
    │         └─ alarm_send_report() (clear_flag=0)
    │               ├─ 生成告警上报消息 (MsgID 111)
    │               └─ 发送到BBU
    │
    └─ 否 → alarm_clear()
              ├─ 清除告警记录
              └─ alarm_send_report() (clear_flag=1)
                    ├─ 生成告警清除消息 (MsgID 111)
                    └─ 发送到BBU
```

## API接口

### 初始化和销毁

```c
/* 初始化告警管理器 */
int alarm_manager_init(void);

/* 销毁告警管理器 */
void alarm_manager_destroy(void);
```

### 告警操作

```c
/* 产生告警 */
int alarm_raise(uint32_t alarm_code, uint32_t sub_code, const char *additional_info);

/* 清除告警 */
int alarm_clear(uint32_t alarm_code, uint32_t sub_code);

/* 发送告警上报 */
int alarm_send_report(uint32_t alarm_code, uint32_t sub_code, uint32_t clear_flag,
                      const char *additional_info);
```

### 自动检测

```c
/* 检查FPGA状态并产生告警 */
int alarm_check_fpga_status(const fpga_status_frame_t *fpga_status);
```

## 告警处理规则

1. **去重**: 相同告警码和子码的告警不会重复上报
2. **自动清除**: 当告警条件消失时，自动清除告警并上报清除消息
3. **立即上报**: 告警产生或清除时立即发送上报消息
4. **时间戳**: 使用系统时间作为告警发生时间
5. **附加信息**: 包含详细的故障信息，便于调试

## 日志输出

正常运行时的日志：

```
[WARN] Alarm raised: 本振失锁告警 (code=50001, sub=0)
[INFO] Sent alarm report: 本振失锁告警 (code=50001, sub=0, clear=0, serial=X)

[INFO] Alarm cleared: 本振失锁告警 (code=50001, sub=0)
[INFO] Sent alarm report: 本振失锁告警 (code=50001, sub=0, clear=1, serial=Y)

[WARN] Alarm raised: 过温告警 (code=1156, sub=0)
[INFO] Sent alarm report: 过温告警 (code=1156, sub=0, clear=0, serial=Z)
```

## 数据结构

### 告警上报消息结构

```
CPRI消息头 (15字节)
    ├─ msg_id = 111
    ├─ msg_length = 15 + 138
    ├─ paau_id
    ├─ bbu_id
    ├─ port_num
    └─ serial_num (递增)

IE 1001 (138字节)
    ├─ IE头 (4字节)
    │   ├─ ie_type = 0x3E9 (1001)
    │   └─ ie_length = 138
    └─ IE数据 (134字节)
        ├─ validity (2字节)
        ├─ alarm_code (4字节)
        ├─ sub_code (4字节)
        ├─ clear_flag (4字节)
        ├─ timestamp (20字节)
        └─ additional_info (100字节)
```

## 注意事项

1. **线程安全**: 使用互斥锁保护告警记录
2. **自动检测**: FPGA状态更新时自动检查告警
3. **立即上报**: 告警产生或清除时立即发送
4. **流水号**: 每条告警消息使用递增的流水号
5. **小端序**: 所有数据使用小端序编码
6. **容量限制**: 最多支持32条告警记录

## 测试建议

1. **本振失锁测试**:
   - 模拟FPGA返回本振失锁状态
   - 验证告警上报
   - 恢复正常状态，验证告警清除

2. **过温告警测试**:
   - 模拟温度超过门限
   - 验证告警上报（包含温度信息）
   - 温度恢复正常，验证告警清除

3. **光链路丢失测试**:
   - 模拟光链路建链失败
   - 验证告警上报（包含光口号）
   - 链路恢复，验证告警清除

4. **通道故障测试**:
   - 模拟通道故障数超过门限
   - 验证告警上报
   - 故障数降低，验证告警清除

5. **并发测试**:
   - 同时产生多个告警
   - 验证所有告警都能正确上报
   - 验证告警记录不冲突

## 扩展功能

### 添加新的告警类型

1. 在`alarm_manager.h`中添加告警码定义：
```c
#define ALARM_CODE_NEW_ALARM 1160
```

2. 在`alarm_manager.c`的`g_alarm_info`表中添加告警信息：
```c
{ALARM_CODE_NEW_ALARM, "新告警名称", "处理建议"},
```

3. 在`alarm_check_fpga_status()`中添加检测逻辑：
```c
if (/* 告警条件 */) {
    alarm_raise(ALARM_CODE_NEW_ALARM, sub_code, "详细信息");
} else {
    alarm_clear(ALARM_CODE_NEW_ALARM, sub_code);
}
```

## 相关文档

- `alarm_report_flow.md`: 告警上报流程需求文档
- `FPGA_Telemetry_Definition.md`: FPGA遥测数据定义
- `IMPLEMENTATION_SUMMARY.md`: 实现总结文档
