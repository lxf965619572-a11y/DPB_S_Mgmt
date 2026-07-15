# 在位检测（心跳）功能实现文档

## 概述

本文档描述了S频段天线管理软件中在位检测（心跳保活）机制的实现，用于确认通信链路的有效性及对端的存活状态。

## 触发条件

**重要**: 在位检测在通信通道建立完成后启动。

- 具体标志：BBU收到PAAU发送的`通道建立配置应答 (MsgID: 3)`消息后，视为通道建立完成
- PAAU在发送通道建立响应后，立即启动心跳流程

## 交互流程

在位检测采用**双向独立发送**模式，非请求-应答模式。

1. **PAAU -> BBU**: PAAU周期性发送`PAAU在位心跳消息 (MsgID: 171)`
2. **BBU -> PAAU**: BBU周期性发送`BBU在位心跳消息 (MsgID: 181)`

## 参数要求

| 参数项 | 规格值 | 说明 |
|--------|--------|------|
| 发送时间间隔 | 3秒 | 双方均以此周期发送心跳包 |
| 超时判定阈值 | 20次 | 连续20次（约60秒）未收到对端心跳，判定对端不存在/链路断开 |

## 消息定义

心跳消息仅包含消息头（Message Header），**消息内容（Payload/IE）为空**。

### PAAU在位心跳消息

- **消息ID**: 171 (MSG_PAAU_HEARTBEAT)
- **发送方**: PAAU
- **长度**: 15 Bytes（仅Header）
- **Payload**: 空

### BBU在位心跳消息

- **消息ID**: 181 (MSG_BBU_HEARTBEAT)
- **发送方**: BBU
- **长度**: 15 Bytes（仅Header）
- **Payload**: 空

## 实现说明

### 文件结构

- `include/heartbeat.h`: 心跳模块头文件
- `src/heartbeat.c`: 心跳模块实现
- `src/msg_handler.c`: 处理BBU心跳消息
- `src/main.c`: 主循环中发送PAAU心跳和检查超时

### 心跳管理器

```c
typedef struct {
    uint32_t paau_heartbeat_count;      /* PAAU发送心跳计数 */
    uint32_t bbu_heartbeat_miss_count;  /* BBU心跳丢失计数 */
    time_t last_bbu_heartbeat_time;     /* 上次收到BBU心跳的时间 */
    bool heartbeat_enabled;             /* 心跳是否启用 */
    pthread_mutex_t mutex;              /* 互斥锁 */
} heartbeat_manager_t;
```

### 关键函数

```c
/* 初始化心跳管理器 */
int heartbeat_init(void);

/* 启动心跳（通道建立完成后调用） */
int heartbeat_start(void);

/* 停止心跳 */
int heartbeat_stop(void);

/* 发送PAAU心跳消息 */
int heartbeat_send_paau(void);

/* 处理收到的BBU心跳消息 */
int heartbeat_handle_bbu(const cpri_message_t *msg);

/* 检查BBU心跳超时 */
bool heartbeat_check_timeout(void);

/* 销毁心跳管理器 */
void heartbeat_destroy(void);
```

## 工作流程

### 1. 初始化阶段

```
程序启动
    ↓
heartbeat_init()
    ↓
初始化心跳管理器
    ↓
heartbeat_enabled = false
```

### 2. 通道建立完成

```
收到通道建立配置 (MSG_CHANNEL_SETUP_CFG)
    ↓
处理配置并发送响应
    ↓
heartbeat_start()
    ↓
heartbeat_enabled = true
    ↓
记录当前时间为last_bbu_heartbeat_time
    ↓
开始心跳流程
```

### 3. 心跳发送（PAAU侧）

```
主循环（每秒执行一次）
    ↓
heartbeat_timer++
    ↓
if (heartbeat_timer >= 3秒)
    ↓
heartbeat_send_paau()
    ↓
构造MSG_PAAU_HEARTBEAT消息（无payload）
    ↓
TCP发送到BBU
    ↓
paau_heartbeat_count++
    ↓
重置heartbeat_timer = 0
```

### 4. 心跳接收（BBU侧）

```
收到BBU心跳消息 (MSG_BBU_HEARTBEAT)
    ↓
msg_handler_dispatch()
    ↓
handle_heartbeat()
    ↓
heartbeat_handle_bbu()
    ↓
更新last_bbu_heartbeat_time = now
    ↓
重置bbu_heartbeat_miss_count = 0
```

### 5. 超时检测

```
主循环（每秒执行一次）
    ↓
heartbeat_check_timeout()
    ↓
计算elapsed = now - last_bbu_heartbeat_time
    ↓
if (elapsed >= 3秒)
    ↓
bbu_heartbeat_miss_count++
    ↓
if (bbu_heartbeat_miss_count >= 20)
    ↓
LOG_ERROR("BBU heartbeat timeout")
    ↓
返回true（超时）
    ↓
主循环处理超时：
    ↓
g_channel_established = false
    ↓
heartbeat_stop()
    ↓
TCP自动重连
    ↓
重新进入通道建立流程
```

## 异常处理

### PAAU未收到BBU心跳（超时）

当PAAU判定BBU不在位（超时）时，执行以下流程：

1. **生成告警**: 记录"导致相控阵复位"的告警信息
2. **进入复位状态**: 停止心跳，重置通道建立标志
3. **重连**: TCP自动重连，重新进入通道建立过程
4. **告警上报**: 在重新发起的`通道建立请求消息 (MsgID: 1)`的`IE 2 (通道建立原因)`中：
   - 设置**建立原因**为`1`（复位/重连）
   - 在**关联告警码**字段填入保存的告警码

### BBU未收到PAAU心跳（超时）

BBU侧处理（由BBU实现）：
1. 上报网管（OMC）
2. 删除该相控阵上配置的所有波束资源

## 代码示例

### 发送PAAU心跳

```c
/* 在主循环中每3秒调用一次 */
if (g_channel_established && heartbeat_timer >= 3) {
    heartbeat_timer = 0;
    heartbeat_send_paau();
}
```

### 处理BBU心跳

```c
/* 在消息处理器中 */
int handle_heartbeat(const cpri_message_t *msg)
{
    return heartbeat_handle_bbu(msg);
}
```

### 检查超时

```c
/* 在主循环中每秒调用一次 */
if (g_channel_established && heartbeat_check_timeout()) {
    LOG_ERROR("BBU heartbeat timeout, resetting connection");
    g_channel_established = false;
    heartbeat_stop();
    /* TCP会自动重连 */
}
```

## 日志输出

正常运行时的日志：

```
[INFO] Heartbeat manager initialized
[INFO] Channel established, heartbeat started
[DEBUG] Sent PAAU heartbeat (count=1)
[DEBUG] Received BBU heartbeat
[DEBUG] Sent PAAU heartbeat (count=2)
[DEBUG] Received BBU heartbeat
...
```

超时时的日志：

```
[ERROR] BBU heartbeat timeout (missed 20 times, 60 seconds)
[ERROR] BBU heartbeat timeout, resetting connection
[INFO] Heartbeat stopped
[WARN] TCP disconnected from BBU
[INFO] TCP connected to BBU
[INFO] Sent channel setup request (serial_num=X, XXX bytes)
...
```

## 注意事项

1. **启动时机**: 心跳必须在通道建立完成后才启动，否则会误判超时
2. **停止时机**: TCP断开连接时必须停止心跳，避免误判
3. **线程安全**: 心跳管理器使用互斥锁保护，可以在多线程环境中安全使用
4. **超时计算**: 使用时间差计算，避免计数器溢出问题
5. **自动重连**: 超时后不需要手动重连，TCP客户端会自动重连
6. **告警上报**: 超时后应在通道建立请求中上报告警码（TODO: 实现告警码管理）

## 测试建议

1. **正常心跳测试**:
   - 启动程序，完成通道建立
   - 观察日志，验证每3秒发送一次PAAU心跳
   - 验证收到BBU心跳时日志正确

2. **超时测试**:
   - 模拟BBU停止发送心跳
   - 观察60秒后是否检测到超时
   - 验证是否自动重连并重新建立通道

3. **重连测试**:
   - 断开TCP连接
   - 验证心跳是否停止
   - 重连后验证心跳是否重新启动

4. **并发测试**:
   - 多线程环境下测试心跳功能
   - 验证互斥锁是否正常工作

## 扩展功能

### 告警码管理（TODO）

需要实现告警码的生成和保存机制，在超时时记录告警码，在重连时上报。

```c
/* 超时时生成告警码 */
uint32_t alarm_code = generate_alarm_code(ALARM_BBU_HEARTBEAT_TIMEOUT);
save_alarm_code(alarm_code);

/* 重连时上报告警码 */
uint32_t alarm_code = get_saved_alarm_code();
channel_setup_create_request(&request, CHANNEL_SETUP_REASON_RESET, alarm_code);
```

### 统计信息

可以添加心跳统计信息，用于监控和调试：

- PAAU发送心跳总数
- BBU心跳接收总数
- 超时次数
- 平均心跳间隔
- 最大心跳间隔

## 故障排查

| 问题 | 可能原因 | 解决方案 |
|------|----------|----------|
| 心跳未发送 | 通道未建立 | 检查通道建立流程是否完成 |
| 频繁超时 | 网络不稳定 | 检查网络连接和BBU状态 |
| 心跳发送失败 | TCP连接断开 | 检查TCP连接状态 |
| 误判超时 | 时间计算错误 | 检查系统时间是否正确 |
| 心跳未停止 | 未调用heartbeat_stop() | 检查TCP断开回调 |
