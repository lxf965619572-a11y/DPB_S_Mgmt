# 状态查询和心跳流水号更新说明

## 更新日期
2026-03-12

## 更新内容

### 1. 心跳流水号修正

**问题**: 心跳消息的流水号固定为0，不符合协议要求

**修正**: 心跳消息使用递增的流水号，与其他消息保持一致

**代码位置**: `src/heartbeat.c`

**修改内容**:
```c
/* 引入外部流水号 */
extern uint32_t g_serial_num;

/* 发送心跳时使用递增的流水号 */
msg.header.serial_num = ++g_serial_num;
```

**日志输出**:
```
[DEBUG] Sent PAAU heartbeat (count=1, serial_num=5)
[DEBUG] Sent PAAU heartbeat (count=2, serial_num=8)
```

### 2. 状态查询功能实现

**需求**: 实现PAAU状态查询和响应功能，响应中的IE字段根据FPGA的遥测信息进行真实填写

**实现**:

#### 新增文件

1. `include/status_query.h`: 状态查询模块头文件
2. `src/status_query.c`: 状态查询模块实现

#### 修改文件

1. `src/msg_handler.c`:
   - 引入status_query模块
   - 实现`handle_status_query()`函数
   - 处理状态查询请求并发送响应

#### 支持的查询类型

| 查询IE | 响应IE | 数据来源 |
|--------|--------|----------|
| 303 (波束状态查询) | 353 (波束状态响应) | `fpga_status.data.beam_status[]` |
| 304 (本振状态查询) | 354 (本振状态响应) | `fpga_status.data.lo_frequency`, `lo_lock_status` |
| 305 (时钟状态查询) | 355 (时钟状态响应) | `fpga_status.data.clock_sync_status` |
| 306 (运行状态查询) | 356 (运行状态响应) | `fpga_status.data.current_work_mode` |
| 307 (CPRI模式查询) | 357 (CPRI模式响应) | `fpga_status.data.current_work_mode` |
| 308 (校准结果查询) | 358 (校准结果响应) | 默认值0（成功） |

#### 核心功能

**状态查询处理**:
```c
int handle_status_query(const cpri_message_t *msg)
{
    /* 生成状态查询响应（使用FPGA真实值） */
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

**FPGA数据提取**:
```c
/* 获取FPGA状态 */
fpga_status_frame_t fpga_status;
if (fpga_handler_get_status(&fpga_status) == SUCCESS) {
    /* 使用FPGA真实值 */
    uint32_t lo_freq_khz = fpga_decode_frequency(fpga_status.data.lo_frequency);
    uint32_t lo_freq_100khz = lo_freq_khz / 100;  /* 转换为100kHz单位 */
    uint32_t lo_status = fpga_status.data.lo_lock_status;
    uint32_t clock_status = fpga_status.data.clock_sync_status;
    uint32_t run_status = fpga_status.data.current_work_mode;
} else {
    /* FPGA状态未就绪，使用默认值 */
}
```

## 工作流程

### 心跳流水号

```
主循环（每3秒）
    ↓
heartbeat_send_paau()
    ↓
msg.header.serial_num = ++g_serial_num
    ↓
发送心跳 (MsgID: 171, serial_num递增)
```

### 状态查询

```
收到状态查询请求 (MsgID: 41)
    ↓
handle_status_query()
    ↓
status_query_create_response()
    ↓
获取FPGA状态 (fpga_handler_get_status())
    ↓
解析请求IE，确定查询类型
    ↓
从FPGA状态提取真实值：
    - 波束状态 ← beam_status[]
    - 本振频率/状态 ← lo_frequency, lo_lock_status
    - 时钟状态 ← clock_sync_status
    - 运行状态 ← current_work_mode
    - CPRI模式 ← current_work_mode
    - 校准结果 ← 默认值
    ↓
构造响应IE
    ↓
复制请求的流水号
    ↓
发送响应 (MsgID: 42, serial_num=请求的流水号)
```

## 数据映射

### FPGA状态到响应IE

| 响应IE | FPGA字段 | 转换 |
|--------|----------|------|
| IE 353 (波束状态) | `beam_status[n]` | 直接使用 |
| IE 354 (本振频率) | `lo_frequency` | 解码后除以100 |
| IE 354 (本振状态) | `lo_lock_status` | 直接使用 |
| IE 355 (时钟状态) | `clock_sync_status` | 直接使用 |
| IE 356 (运行状态) | `current_work_mode` | 直接使用 |
| IE 357 (CPRI模式) | `current_work_mode` | 直接使用 |
| IE 358 (校准结果) | - | 默认值0 |

## 日志示例

### 心跳日志

```
[DEBUG] Sent PAAU heartbeat (count=1, serial_num=5)
[DEBUG] Received BBU heartbeat
[DEBUG] Sent PAAU heartbeat (count=2, serial_num=8)
[DEBUG] Received BBU heartbeat
```

### 状态查询日志

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

## 测试要点

### 心跳流水号测试

1. 启动程序，完成通道建立
2. 观察心跳日志，验证流水号递增
3. 验证流水号与其他消息连续

### 状态查询测试

1. **FPGA状态就绪**:
   - 发送状态查询请求
   - 验证响应数据来自FPGA真实值
   - 检查本振频率、时钟状态等是否正确

2. **FPGA状态未就绪**:
   - 在FPGA状态轮询前发送查询
   - 验证响应使用默认值
   - 确保不影响查询功能

3. **多IE查询**:
   - 发送包含多个IE的查询请求
   - 验证响应包含所有对应的IE
   - 检查IE顺序是否正确

4. **流水号测试**:
   - 验证响应的流水号与请求相同

## 注意事项

1. **心跳流水号**: 心跳消息现在使用全局流水号，与其他消息保持一致
2. **FPGA数据**: 状态查询响应优先使用FPGA真实值，确保数据准确
3. **默认值**: FPGA状态未就绪时使用合理的默认值
4. **流水号**: 所有响应消息都带回请求的流水号
5. **小端序**: 所有IE数据使用小端序编码
6. **线程安全**: FPGA状态管理器使用互斥锁保护

## 文档

详细文档请参考：
- `STATUS_QUERY_README.md`: 状态查询功能详细文档
- `HEARTBEAT_README.md`: 心跳功能文档（已更新）
- `IMPLEMENTATION_SUMMARY.md`: 实现总结（需更新）

## 编译和运行

```bash
# 编译
make clean
make

# 运行
sudo ./antenna_mgmt

# 查看日志
tail -f logs/antenna_mgmt.log
```

## 兼容性

- 向后兼容之前的实现
- 心跳流水号修正不影响现有功能
- 状态查询功能为新增功能
- 所有修改符合CPRI V1.6协议要求
