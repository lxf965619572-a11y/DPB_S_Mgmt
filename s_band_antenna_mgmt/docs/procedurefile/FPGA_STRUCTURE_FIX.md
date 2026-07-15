# FPGA遥测结构修正说明

## 修正日期
2026-03-12

## 问题描述

FPGA遥测数据结构 `fpga_status_data_t` 与协议定义文档 `FPGA_Telemetry_Definition.md` 不匹配，导致字段偏移和类型错误。

## 修正内容

### 1. 结构体字段重新排列

根据 `FPGA_Telemetry_Definition.md` 的字节偏移定义，重新排列了 `fpga_status_data_t` 结构体（127字节）：

**主要变更**:

| 旧字段名 | 新字段名 | 类型变更 | 说明 |
|---------|---------|---------|------|
| `beam_count` | `nr_beam_count` | `uint32_t` | 支持的NR波束数量 |
| `beam_status[16]` | `beam_status` | `uint16_t[16]` → `uint32_t` | 改为位图表示（低16位对应16个波束） |
| `hw_version[4]` | `hw_version` | `char[4]` → `uint32_t` | 硬件版本号 |
| `current_work_mode` | `phased_array_work_mode` | `uint32_t` | 相控阵工作模式 |
| - | `cpri_work_mode` | `uint32_t` | CPRI工作模式（新增） |

**新增字段**:
- `channel_setup_reason` (Byte 3): 通道建立原因
- `cmd_beam_num` (Byte 52): 信令波束个数 n1
- `service_beam_num` (Byte 53): 业务波束个数 n2
- `hw_type_ext[6]` (Bytes 66-71): 硬件类型补充
- `calib_result` (Byte 85): 校准结果
- `loopback_result` (Byte 99): 回环回路结果
- `channel_fault_count` (Byte 103): 通道故障数量
- `tx_freq_start`, `tx_freq_end` (Bytes 113-116): 发送频段
- `rx_freq_start`, `rx_freq_end` (Bytes 117-120): 接收频段
- `beam_bandwidth` (Bytes 121-122): 支持的波束带宽

**保留字段**:
- `reserved1` (Bytes 50-51): 2字节
- `reserved2` (Byte 72): 1字节
- `reserved3` (Byte 86): 1字节
- `reserved4[3]` (Bytes 100-102): 3字节
- `reserved5` (Bytes 123-126): 4字节

### 2. 修改的文件

#### 头文件
- **include/fpga_protocol.h**: 重新定义 `fpga_status_data_t` 结构体

#### 源文件
- **src/fpga_protocol.c**: 更新字节序转换代码
  - 更新 `fpga_decode_status_frame()` 函数
  - 修改字段名: `beam_count` → `nr_beam_count`
  - 修改字段名: `current_work_mode` → `phased_array_work_mode` 和 `cpri_work_mode`
  - 修改波束状态处理: 从数组改为位图
  - 新增字段的字节序转换

- **src/fpga_handler.c**: 更新日志输出
  - 修改字段名: `beam_count` → `nr_beam_count`

- **src/channel_setup.c**: 更新通道建立请求
  - 修改字段名: `beam_count` → `nr_beam_count`

- **src/status_query.c**: 更新状态查询响应
  - 修改字段名: `current_work_mode` → `phased_array_work_mode` (运行状态)
  - 修改字段名: `current_work_mode` → `cpri_work_mode` (CPRI模式)
  - 修改波束状态访问: 从 `beam_status[beam_number]` 改为位图提取 `(beam_status >> beam_number) & 0x01`

- **src/fpga_to_cpri.c**: 更新FPGA到CPRI转换
  - 修改字段名: `current_work_mode` → `phased_array_work_mode` (运行状态)
  - 修改字段名: `current_work_mode` → `cpri_work_mode` (CPRI模式)

### 3. 结构体布局验证

完整的 `fpga_status_data_t` 结构体（127字节）:

```c
typedef struct __attribute__((packed)) {
    /* Byte 0: 通道建立标志 */
    uint8_t  link_success_flag;         /* Bit0-3: 主路0-3, Bit4-7: 备路0-3 */

    /* Bytes 1-3: 光纤和建立原因 */
    uint8_t  main_fiber_num;            /* 主光纤号 */
    uint8_t  backup_fiber_num;          /* 备光纤号 */
    uint8_t  channel_setup_reason;      /* 通道建立原因 */

    /* Bytes 4-7: NR波束数量 */
    uint32_t nr_beam_count;             /* 支持的NR波束数量 */

    /* Bytes 8-11: 功率和模式 */
    uint16_t max_tx_power;              /* 最大发射功率（1/256 dBm） */
    uint16_t supported_modes;           /* 支持的模式 */

    /* Bytes 12-43: 硬件类型 */
    uint8_t  hw_type[32];               /* 相控阵硬件类型（ASCII字符串） */

    /* Bytes 44-47: 硬件版本 */
    uint32_t hw_version;                /* 相控阵硬件版本号 */

    /* Bytes 48-49: 当前功率 */
    uint16_t current_output_power;      /* 当前功率值（1/256 dBm） */

    /* Bytes 50-51: 保留 */
    uint16_t reserved1;

    /* Bytes 52-53: 波束个数 */
    uint8_t  cmd_beam_num;              /* 信令波束个数 n1 */
    uint8_t  service_beam_num;          /* 业务波束个数 n2 */

    /* Bytes 54-57: 波束状态 */
    uint32_t beam_status;               /* 波束状态位图（低16位对应16个物理波束） */

    /* Bytes 58-61: 本振频率 */
    uint32_t lo_frequency;              /* 本振频率（编码值） */

    /* Bytes 62-65: 本振状态 */
    uint32_t lo_lock_status;            /* 本振状态（0=锁定，1=失锁） */

    /* Bytes 66-71: 硬件类型补充 */
    uint8_t  hw_type_ext[6];            /* 相控阵硬件类型(回件) */

    /* Byte 72: 保留 */
    uint8_t  reserved2;

    /* Bytes 73-76: 时钟状态 */
    uint32_t clock_sync_status;         /* 时钟状态（0=同步，1=失锁） */

    /* Bytes 77-80: 相控阵工作模式 */
    uint32_t phased_array_work_mode;    /* 相控阵工作模式（0=业务，1=频谱，2=待机，3=自校准） */

    /* Bytes 81-84: CPRI工作模式 */
    uint32_t cpri_work_mode;            /* CPRI工作模式（1=普通，2=级联，3=主备，4=反向分担） */

    /* Byte 85: 校准结果 */
    uint8_t  calib_result;              /* 校准结果（0=成功，其他=失败） */

    /* Byte 86: 保留 */
    uint8_t  reserved3;

    /* Bytes 87-98: CPRI时延参数 */
    uint32_t toffset;                   /* CPRI Toffset数值 */
    uint32_t t2a;                       /* CPRI T2a数值 */
    uint32_t ta3;                       /* CPRI Ta3数值 */

    /* Byte 99: 回环回路结果 */
    uint8_t  loopback_result;           /* 回环回路结果（低4位有效） */

    /* Bytes 100-102: 保留 */
    uint8_t  reserved4[3];

    /* Byte 103: 通道故障数量 */
    uint8_t  channel_fault_count;       /* 通道故障数量 */

    /* Bytes 104-112: 温度检测 */
    int8_t   temp1;                     /* 温度检测点1的温度（℃，有符号） */
    int8_t   temp2;                     /* 温度检测点2的温度（℃，有符号） */
    int8_t   temp3;                     /* 温度检测点3的温度（℃，有符号） */
    int8_t   temp1_high;                /* 温度检测点1的上门限 */
    int8_t   temp1_low;                 /* 温度检测点1的下门限 */
    int8_t   temp2_high;                /* 温度检测点2的上门限 */
    int8_t   temp2_low;                 /* 温度检测点2的下门限 */
    int8_t   temp3_high;                /* 温度检测点3的上门限 */
    int8_t   temp3_low;                 /* 温度检测点3的下门限 */

    /* Bytes 113-122: 频段信息（预留，CPU写死） */
    uint16_t tx_freq_start;             /* 发送频段起始频点 */
    uint16_t tx_freq_end;               /* 发送频段截止频点 */
    uint16_t rx_freq_start;             /* 接收频段起始频点 */
    uint16_t rx_freq_end;               /* 接收频段截止频点 */
    uint16_t beam_bandwidth;            /* 支持的波束带宽 */

    /* Bytes 123-126: 保留 */
    uint32_t reserved5;
} fpga_status_data_t;
```

### 4. 字节序转换更新

在 `fpga_decode_status_frame()` 中，所有多字节字段都需要进行大端到主机序的转换：

```c
/* 2字节字段 */
status->data.max_tx_power = NTOHS(status->data.max_tx_power);
status->data.supported_modes = NTOHS(status->data.supported_modes);
status->data.current_output_power = NTOHS(status->data.current_output_power);
status->data.reserved1 = NTOHS(status->data.reserved1);
status->data.tx_freq_start = NTOHS(status->data.tx_freq_start);
status->data.tx_freq_end = NTOHS(status->data.tx_freq_end);
status->data.rx_freq_start = NTOHS(status->data.rx_freq_start);
status->data.rx_freq_end = NTOHS(status->data.rx_freq_end);
status->data.beam_bandwidth = NTOHS(status->data.beam_bandwidth);

/* 4字节字段 */
status->data.nr_beam_count = NTOHL(status->data.nr_beam_count);
status->data.hw_version = NTOHL(status->data.hw_version);
status->data.beam_status = NTOHL(status->data.beam_status);
status->data.lo_frequency = NTOHL(status->data.lo_frequency);
status->data.lo_lock_status = NTOHL(status->data.lo_lock_status);
status->data.clock_sync_status = NTOHL(status->data.clock_sync_status);
status->data.phased_array_work_mode = NTOHL(status->data.phased_array_work_mode);
status->data.cpri_work_mode = NTOHL(status->data.cpri_work_mode);
status->data.toffset = NTOHL(status->data.toffset);
status->data.t2a = NTOHL(status->data.t2a);
status->data.ta3 = NTOHL(status->data.ta3);
status->data.reserved5 = NTOHL(status->data.reserved5);
```

### 5. 波束状态访问方式变更

**旧方式** (数组):
```c
uint16_t beam_status = fpga_status.data.beam_status[beam_number];
```

**新方式** (位图):
```c
uint32_t beam_status = (fpga_status.data.beam_status >> beam_number) & 0x01;
```

位图中，低16位对应16个物理波束：
- Bit 0: 波束0
- Bit 1: 波束1
- ...
- Bit 15: 波束15

### 6. 工作模式字段分离

**旧方式** (单一字段):
```c
uint32_t mode = fpga_status.data.current_work_mode;  // 混用
```

**新方式** (分离字段):
```c
/* 相控阵运行状态 */
uint32_t run_status = fpga_status.data.phased_array_work_mode;
// 0=业务, 1=频谱, 2=待机, 3=自校准

/* CPRI工作模式 */
uint32_t cpri_mode = fpga_status.data.cpri_work_mode;
// 1=普通, 2=级联, 3=主备, 4=反向分担
```

## 验证要点

1. **结构体大小**: `sizeof(fpga_status_data_t)` 必须等于 127 字节
2. **字节对齐**: 使用 `__attribute__((packed))` 确保1字节对齐
3. **字节序**: 所有多字节字段从FPGA接收时需要大端转主机序
4. **字段访问**: 所有代码中访问FPGA状态字段的地方都已更新

## 兼容性

- 此修正是破坏性变更，需要重新编译整个项目
- 旧的二进制文件无法与新的结构体兼容
- 所有访问FPGA状态的代码都已同步更新

## 测试建议

1. 验证结构体大小为127字节
2. 测试FPGA状态帧解析
3. 验证波束状态位图提取
4. 测试状态查询响应
5. 验证通道建立请求
6. 检查日志输出是否正确

## 相关文档

- `FPGA_Telemetry_Definition.md`: FPGA遥测协议定义
- `STATUS_QUERY_README.md`: 状态查询功能文档
- `UART_FPGA_README.md`: UART与FPGA通信文档
