# 状态查询功能实现文档

## 概述

本文档描述了S频段天线管理软件中状态查询流程的实现，包括BBU查询PAAU状态和PAAU响应的完整流程。

## 流程概览

| 步骤 | 消息名称 | 消息ID | 发送方 | 接收方 | 作用 |
|------|----------|--------|--------|--------|------|
| 1 | PAAU状态查询 | 41 | BBU | PAAU | 查询设备各类运行状态 |
| 2 | PAAU状态查询响应 | 42 | PAAU | BBU | 返回状态数据 |

## 消息定义

### 1. PAAU状态查询 (MSG_PAAU_STATUS_QUERY, ID=41)

**方向**: BBU -> PAAU
**描述**: BBU主动查询PAAU状态

#### 包含的IE列表

| IE ID | IE名称 | 长度 | 关键内容 |
|-------|--------|------|----------|
| 303 | 波束状态查询 | 6字节 | 波束号(2B) |
| 304 | 本振状态查询 | 4字节 | 无Payload |
| 305 | 时钟状态查询 | 4字节 | 无Payload |
| 306 | 运行状态查询 | 4字节 | 无Payload |
| 307 | CPRI口工作模式查询 | 4字节 | 无Payload |
| 308 | 校准结果查询 | 4字节 | 无Payload |

#### IE结构详解

**IE 303**: 波束状态查询
- `BeamNumber` (2字节): 待查询的波束号
  - 0~127: 特定波束
  - 0xFFFF: 所有波束

**IE 304~308**: 无Payload的查询指令，仅包含4字节IE Header

### 2. PAAU状态查询响应 (MSG_PAAU_STATUS_QUERY_RSP, ID=42)

**方向**: PAAU -> BBU
**描述**: PAAU回复状态数据，**使用FPGA真实遥测信息填充**

#### 包含的IE列表

| IE ID | IE名称 | 长度 | 关键内容 |
|-------|--------|------|----------|
| 353 | 波束状态响应 | 10字节 | 保留(1B), 波束号(1B), 状态(4B) |
| 354 | 本振状态响应 | 12字节 | 本振频率(4B), 状态(4B) |
| 355 | 时钟状态响应 | 8字节 | 状态(4B) |
| 356 | 相控阵运行状态响应 | 8字节 | 状态(4B) |
| 357 | CPRI口工作模式响应 | 8字节 | 模式(4B) |
| 358 | 校准结果响应 | 5字节 | 结果(1B) |

#### IE结构详解

**IE 353**: 波束状态响应
- `Reserved` (1字节): 保留
- `BeamNumber` (1字节): 波束号
- `Status` (4字节): 波束状态
  - 0: 使能
  - 1: 去使能
- **数据来源**: `fpga_status.data.beam_status[beam_number]`

**IE 354**: 本振状态响应
- `LO_Frequency` (4字节): 本振频率（单位: 100kHz）
- `Status` (4字节): 锁定状态
  - 0: 锁定
  - 1: 失锁
- **数据来源**:
  - 频率: `fpga_decode_frequency(fpga_status.data.lo_frequency) / 100`
  - 状态: `fpga_status.data.lo_lock_status`

**IE 355**: 时钟状态响应
- `Status` (4字节): 时钟同步状态
  - 0: 同步
  - 1: 失步
- **数据来源**: `fpga_status.data.clock_sync_status`

**IE 356**: 相控阵运行状态响应
- `Status` (4字节): 运行状态
  - 0: 业务模式
  - 1: 频谱监测模式
  - 2: 待机模式
  - 3: 自校准模式
- **数据来源**: `fpga_status.data.current_work_mode`

**IE 357**: CPRI口工作模式响应
- `Mode` (4字节): 当前工作模式
  - 0: 普通模式
  - 1: 级联模式
  - 2: 主备模式
  - 3: 负荷分担模式
- **数据来源**: `fpga_status.data.current_work_mode`

**IE 358**: 校准结果响应
- `Result` (1字节): 校准结果
  - 0: 成功
  - 其他: 失败
- **数据来源**: 默认值（TODO: 从FPGA获取）

## 实现说明

### 文件结构

- `include/status_query.h`: 状态查询模块头文件
- `src/status_query.c`: 状态查询模块实现
- `src/msg_handler.c`: 处理状态查询请求并发送响应

### 关键函数

```c
/* 处理状态查询请求 */
int status_query_handle_request(const cpri_message_t *msg);

/* 生成状态查询响应（使用FPGA真实值） */
int status_query_create_response(cpri_message_t *response, const cpri_message_t *request);
```

## 工作流程

```
收到状态查询请求 (MSG_PAAU_STATUS_QUERY, MsgID=41)
    ↓
msg_handler_dispatch()
    ↓
handle_status_query()
    ↓
status_query_handle_request()
    ↓
status_query_create_response()
    ↓
获取FPGA状态 (fpga_handler_get_status())
    ↓
解析请求中的IE，确定查询类型
    ↓
根据查询类型，从FPGA状态提取真实值
    ↓
构造响应IE：
    - IE 303: 波束状态 ← fpga_status.data.beam_status[]
    - IE 354: 本振状态 ← fpga_status.data.lo_frequency, lo_lock_status
    - IE 355: 时钟状态 ← fpga_status.data.clock_sync_status
    - IE 356: 运行状态 ← fpga_status.data.current_work_mode
    - IE 357: CPRI模式 ← fpga_status.data.current_work_mode
    - IE 358: 校准结果 ← 默认值
    ↓
复制请求的流水号到响应
    ↓
编码并发送响应 (MSG_PAAU_STATUS_QUERY_RSP, MsgID=42)
    ↓
完成
```

## 数据映射

### FPGA状态到响应IE的映射

| 响应IE | FPGA状态字段 | 转换说明 |
|--------|--------------|----------|
| IE 353 (波束状态) | `beam_status[n]` | 直接使用 |
| IE 354 (本振频率) | `lo_frequency` | 解码后除以100转换为100kHz单位 |
| IE 354 (本振状态) | `lo_lock_status` | 直接使用 (0=锁定) |
| IE 355 (时钟状态) | `clock_sync_status` | 直接使用 (0=同步) |
| IE 356 (运行状态) | `current_work_mode` | 直接使用 |
| IE 357 (CPRI模式) | `current_work_mode` | 直接使用 |
| IE 358 (校准结果) | - | 默认值0（成功） |

### 默认值处理

如果FPGA状态未就绪（`fpga_handler_get_status()`返回失败），使用以下默认值：

- 本振频率: 21000 (2.1 GHz, 100kHz单位)
- 本振状态: 0 (锁定)
- 时钟状态: 0 (同步)
- 运行状态: 0 (业务模式)
- CPRI模式: 0 (普通模式)
- 校准结果: 0 (成功)

## 代码示例

### 处理状态查询

```c
int handle_status_query(const cpri_message_t *msg)
{
    /* 生成状态查询响应 */
    cpri_message_t response;
    status_query_create_response(&response, msg);

    /* 编码并发送响应 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
    }

    cpri_free_message(&response);
    return SUCCESS;
}
```

### 从FPGA状态提取数据

```c
/* 获取FPGA状态 */
fpga_status_frame_t fpga_status;
if (fpga_handler_get_status(&fpga_status) == SUCCESS) {
    /* 使用真实值 */
    uint32_t lo_freq_khz = fpga_decode_frequency(fpga_status.data.lo_frequency);
    uint32_t lo_freq_100khz = lo_freq_khz / 100;
    uint32_t lo_status = fpga_status.data.lo_lock_status;
} else {
    /* 使用默认值 */
    uint32_t lo_freq_100khz = 21000;
    uint32_t lo_status = 0;
}
```

## 日志输出

正常运行时的日志：

```
[INFO] Handle status query
[DEBUG] Added beam status response: beam=0, status=0
[DEBUG] Added LO status response: freq=21000 (100kHz), status=0
[DEBUG] Added clock status response: status=0
[DEBUG] Added run status response: status=0
[DEBUG] Added CPRI mode response: mode=0
[DEBUG] Added calibration result response: result=0
[INFO] Created status query response: 6 IEs, payload_len=60
[INFO] Sent status query response (serial_num=X)
```

## 注意事项

1. **FPGA状态依赖**: 响应数据优先使用FPGA真实值，确保数据准确性
2. **流水号处理**: 响应消息必须带回请求消息中的流水号
3. **IE顺序**: 响应IE的顺序应与请求IE的顺序一致
4. **默认值**: FPGA状态未就绪时使用合理的默认值，不影响查询功能
5. **线程安全**: FPGA状态管理器使用互斥锁保护，可以安全访问
6. **小端序**: 所有IE数据使用小端序编码

## 测试建议

1. **单个IE查询**:
   - 发送只包含一个IE的查询请求
   - 验证响应中包含对应的响应IE
   - 验证数据是否来自FPGA真实值

2. **多个IE查询**:
   - 发送包含多个IE的查询请求
   - 验证响应中包含所有对应的响应IE
   - 验证IE顺序是否正确

3. **波束查询**:
   - 查询特定波束（beam_number < 16）
   - 查询所有波束（beam_number = 0xFFFF）
   - 验证波束状态是否正确

4. **FPGA状态测试**:
   - FPGA状态就绪时，验证使用真实值
   - FPGA状态未就绪时，验证使用默认值

5. **流水号测试**:
   - 验证响应消息的流水号与请求消息相同

## 扩展功能

### 添加新的查询类型

在`status_query_create_response()`中添加新的case分支：

```c
case IE_TYPE_YOUR_NEW_QUERY: {
    /* 从FPGA状态获取数据 */
    uint8_t resp_data[N];
    /* 填充响应数据 */
    add_ie(&payload, &offset, IE_TYPE_YOUR_NEW_RESP, resp_data, sizeof(resp_data));
    break;
}
```

### 校准结果查询

TODO: 从FPGA获取真实的校准结果

```c
case IE_TYPE_CALIB_RESULT_QUERY: {
    uint8_t calib_resp[1];
    if (has_fpga_status) {
        /* 从FPGA状态获取校准结果 */
        calib_resp[0] = fpga_status.data.calib_result;
    } else {
        calib_resp[0] = 0;  /* 默认成功 */
    }
    add_ie(&payload, &offset, IE_TYPE_CALIB_RESULT_RESP, calib_resp, sizeof(calib_resp));
    break;
}
```

## 故障排查

| 问题 | 可能原因 | 解决方案 |
|------|----------|----------|
| 响应数据不正确 | FPGA状态未就绪 | 检查UART连接和FPGA状态轮询 |
| 响应未发送 | TCP连接断开 | 检查TCP连接状态 |
| IE数据错误 | 字节序错误 | 检查小端序编码 |
| 流水号不匹配 | 未复制请求流水号 | 检查响应消息头设置 |
| 波束状态错误 | 波束号超出范围 | 检查波束号有效性（0~15） |
