# NR小区配置功能实现文档

## 概述

本文档描述了S频段天线管理软件中NR小区配置流程的实现，包括小区建立、重配、删除以及频点配置功能。

## 流程概览

根据CPRI V1.6协议，小区配置流程不再分阶段，BBU在一条配置消息中同时下发小区参数和一个或多个频点参数。

| 步骤 | 消息名称 | 消息ID | 发送方 | 接收方 | 作用 |
|------|----------|--------|--------|--------|------|
| 1 | NR小区配置消息 | 195 | BBU | PAAU | 同时下发小区配置及所有频点配置 |
| 2 | NR小区配置响应 | 196 | PAAU | BBU | 同时确认小区及频点配置结果 |

## 消息定义

### 1. NR小区配置消息 (MsgID: 195)

**发送方**: BBU -> PAAU

**包含的IE列表**:
- **IE 1506 (0x5E2)**: 小区配置 (1个)
- **IE 1507 (0x5E3)**: 频点配置 (N个, N>=1)

#### IE 1506: 小区配置 (10字节payload)

```c
typedef struct {
    uint8_t  cell_cfg_flag;     /* 小区配置标识 (0:建立, 1:重配, 2:删除) */
    uint32_t local_cell_id;     /* 本地小区标识 */
    uint16_t cell_power;        /* 小区发射功率 (1/256 dBm) */
    uint8_t  reserved;          /* 保留 */
    uint8_t  freq_count;        /* 频点数量 */
    uint8_t  cell_type;         /* 小区制式 (3: 5G NR) */
} cell_config_ie_t;
```

#### IE 1507: 频点配置 (32字节payload)

```c
typedef struct {
    uint8_t  freq_cfg_flag;     /* 频点配置标识 (0:建立, 1:删除) */
    uint32_t local_cell_id;     /* 关联的本地小区标识 */
    uint8_t  beam_id;           /* 波束号 */
    uint32_t dl_center_freq;    /* 下行中心频率 (1kHz) */
    uint32_t reserved1;         /* 保留 */
    uint8_t  special_subframe;  /* 特殊子帧配置 (bit0-3: 0-8) */
    uint32_t sys_subframe_num;  /* 配置生效的系统子帧号 (0-4095) */
    uint32_t beam_bandwidth;    /* 波束带宽 */
    uint32_t ul_dl_config;      /* 上下行子帧配置 */
    uint8_t  reserved2;         /* 保留 */
    uint32_t ul_center_freq;    /* 上行中心频率 (1kHz) */
} freq_config_ie_t;
```

### 2. NR小区配置响应 (MsgID: 196)

**发送方**: PAAU -> BBU

**包含的IE列表**:
- **IE 1516 (0x5EC)**: 小区配置响应 (1个)
- **IE 1517 (0x5ED)**: 频点配置响应 (N个)

#### IE 1516: 小区配置响应 (8字节payload)

```c
typedef struct {
    uint32_t local_cell_id;     /* 对应的本地小区标识 */
    uint32_t result;            /* 小区配置结果 (0:成功, 1:失败) */
} cell_config_resp_ie_t;
```

#### IE 1517: 频点配置响应 (9字节payload)

```c
typedef struct {
    uint32_t local_cell_id;     /* 对应的本地小区标识 */
    uint8_t  beam_id;           /* 对应的波束号 */
    uint32_t result;            /* 频点配置结果 (0:成功, 1:失败) */
} freq_config_resp_ie_t;
```

## 实现说明

### 文件结构

- **include/cell_config.h**: 小区配置模块头文件
- **src/cell_config.c**: 小区配置模块实现
- **src/msg_handler.c**: 消息处理器（添加小区配置消息处理）

### 核心功能

#### 1. 小区配置管理器

```c
typedef struct {
    cell_info_t cells[MAX_CELLS];                      /* 最多16个小区 */
    freq_info_t freqs[MAX_CELLS][MAX_FREQS_PER_CELL]; /* 每个小区最多8个频点 */
    pthread_mutex_t mutex;                             /* 线程安全保护 */
} cell_config_manager_t;
```

#### 2. 小区配置处理

**小区建立** (cell_cfg_flag = 0):
- 检查小区是否已存在
- 如果已存在且配置一致，返回成功
- 如果已存在但配置不一致，返回失败
- 如果不存在，创建新小区

**小区重配** (cell_cfg_flag = 1):
- 检查小区是否存在
- 如果不存在，返回失败
- 如果存在，更新小区配置（如功率）

**小区删除** (cell_cfg_flag = 2):
- 删除小区及其所有频点
- 删除不存在的小区也返回成功

#### 3. 频点配置处理

**频点建立** (freq_cfg_flag = 0):
- 检查关联的小区是否存在
- 如果频点已存在，用新参数覆盖
- 如果频点不存在，创建新频点
- **关键**: 发送频率与带宽配置到FPGA

**频点删除** (freq_cfg_flag = 1):
- 删除指定的频点
- 删除不存在的频点也返回成功

### FPGA交互

当收到频点配置后，系统会自动将频率和带宽信息发送给FPGA：

```c
/* 发送频率与带宽配置到FPGA */
int ret = fpga_send_freq_band(freq_cfg->dl_center_freq,
                              freq_cfg->ul_center_freq,
                              freq_cfg->beam_bandwidth);
```

FPGA接收的数据格式：
- 下行中心频率: 单位1kHz，使用频率编码算法压缩
- 上行中心频率: 单位1kHz，使用频率编码算法压缩
- 波束带宽: 原始值

频率编码算法：
```
encoded = (freq_khz / 50000) << 16 | ((freq_khz % 50000) * 2)
```

## 工作流程

```
收到NR小区配置消息 (MsgID 195)
    ↓
msg_handler_dispatch()
    ↓
handle_cell_config()
    ↓
cell_config_create_response()
    ↓
解析请求中的IE
    ↓
处理小区配置IE (IE 1506)
    ├─ 建立/重配/删除小区
    └─ 生成小区配置响应IE (IE 1516)
    ↓
处理频点配置IE (IE 1507, 可能有多个)
    ├─ 建立/删除频点
    ├─ 发送频率与带宽到FPGA ← 关键步骤
    └─ 生成频点配置响应IE (IE 1517)
    ↓
编码并发送响应 (MsgID 196)
    ↓
完成
```

## 配置规则

根据协议要求，实现了以下规则：

1. **小区建立/重配/删除**：都应包含小区配置IE
2. **功率重配**：不需要带频点配置IE
3. **频点重配**：只需要带重配的频点配置IE
4. **小区删除**：不包含频点配置IE
5. **小区建立**：跟随的频点配置IE必为"建立"；只要有1个频点建立成功，则表示小区建立成功
6. **删除不存在的小区**：返回"成功"
7. **删除未建立的载频**：返回"成功"
8. **建立已存在的小区**：如配置一致返回"成功"，否则返回"失败"
9. **重配未建立的小区**：返回"失败"
10. **小区重配+频点建立**：用新参数覆盖此波束的原有参数

## 数据结构

### 小区信息存储

```c
typedef struct {
    bool     active;            /* 小区是否激活 */
    uint32_t local_cell_id;     /* 本地小区标识 */
    uint16_t cell_power;        /* 小区发射功率 */
    uint8_t  cell_type;         /* 小区制式 */
    uint8_t  freq_count;        /* 频点数量 */
} cell_info_t;
```

### 频点信息存储

```c
typedef struct {
    bool     active;            /* 频点是否激活 */
    uint32_t local_cell_id;     /* 关联的小区ID */
    uint8_t  beam_id;           /* 波束号 */
    uint32_t dl_center_freq;    /* 下行中心频率 (1kHz) */
    uint32_t ul_center_freq;    /* 上行中心频率 (1kHz) */
    uint32_t beam_bandwidth;    /* 波束带宽 */
    uint32_t ul_dl_config;      /* 上下行子帧配置 */
    uint8_t  special_subframe;  /* 特殊子帧配置 */
} freq_info_t;
```

## 日志输出

正常运行时的日志：

```
[INFO] Handling NR cell config request
[DEBUG] Parsed cell config IE: flag=0, cell_id=1, power=11520, freq_count=2, type=3
[INFO] Cell 1 established: power=11520, type=3
[INFO] Cell config response: cell_id=1, result=0
[DEBUG] Parsed freq config IE: flag=0, cell_id=1, beam=0, dl_freq=2100000 kHz, ul_freq=1900000 kHz, bw=20000000
[INFO] Freq configured: cell=1, beam=0, dl=2100000 kHz, ul=1900000 kHz, bw=20000000
[INFO] Sent freq/band to FPGA: DL=2100000 kHz, UL=1900000 kHz, BW=20000000
[INFO] Freq config response: cell_id=1, beam=0, result=0
[INFO] Created cell config response: payload_len=34
[INFO] Sent cell config response (serial_num=X)
```

## 注意事项

1. **原子性**: PAAU必须成功应用所有配置（小区+所有频点）才返回成功
2. **线程安全**: 使用互斥锁保护小区配置数据
3. **FPGA同步**: 频点配置成功后立即发送到FPGA
4. **流水号**: 响应消息带回请求消息的流水号
5. **小端序**: 所有IE数据使用小端序编码
6. **容量限制**: 最多支持16个小区，每个小区最多8个频点

## 测试建议

1. **小区建立测试**:
   - 发送小区建立请求（带1个频点）
   - 验证响应成功
   - 验证FPGA收到频率配置

2. **多频点测试**:
   - 发送小区建立请求（带多个频点）
   - 验证所有频点配置成功
   - 验证FPGA收到所有频率配置

3. **小区重配测试**:
   - 建立小区后发送重配请求
   - 验证功率更新成功

4. **频点重配测试**:
   - 建立频点后发送重配请求
   - 验证FPGA收到新的频率配置

5. **小区删除测试**:
   - 删除已建立的小区
   - 验证所有频点被删除

6. **异常场景测试**:
   - 重配不存在的小区（应返回失败）
   - 删除不存在的小区（应返回成功）
   - 建立已存在的小区（配置一致返回成功，不一致返回失败）

## 相关文档

- `cell_config_flow.md`: 小区配置流程需求文档
- `FPGA_PROTOCOL.md`: FPGA通信协议文档
- `IMPLEMENTATION_SUMMARY.md`: 实现总结文档
