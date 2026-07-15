# 心跳功能实现更新说明

## 更新日期
2026-03-12

## 更新内容

### 1. 系统时间同步修正

**修改**: 系统时间不再发送到FPGA

**原因**: 根据需求，收到系统时间IE时只需要设置Linux系统时间，不需要同步到FPGA

**代码位置**: `src/channel_setup.c`
- 移除了`fpga_send_system_time()`调用

### 2. 在位检测（心跳）功能实现

**需求**: 实现PAAU与BBU之间的心跳保活机制

**参数**:
- 发送间隔: 3秒
- 超时阈值: 20次（约60秒）
- 触发条件: 通道建立完成后启动

**实现**:

#### 新增文件

1. `include/heartbeat.h`: 心跳模块头文件
2. `src/heartbeat.c`: 心跳模块实现

#### 修改文件

1. `src/msg_handler.c`:
   - 引入heartbeat模块
   - 在通道建立完成后启动心跳
   - 实现BBU心跳消息处理

2. `src/main.c`:
   - 引入heartbeat模块
   - 初始化心跳管理器
   - 主循环中每3秒发送PAAU心跳
   - 主循环中检查BBU心跳超时
   - TCP断开时停止心跳
   - 清理时销毁心跳管理器

#### 核心功能

**心跳发送**:
```c
/* 每3秒发送一次PAAU心跳 */
if (g_channel_established && heartbeat_timer >= 3) {
    heartbeat_timer = 0;
    heartbeat_send_paau();
}
```

**心跳接收**:
```c
/* 收到BBU心跳时更新时间戳 */
int handle_heartbeat(const cpri_message_t *msg)
{
    return heartbeat_handle_bbu(msg);
}
```

**超时检测**:
```c
/* 检查BBU心跳超时（20次，约60秒） */
if (heartbeat_check_timeout()) {
    LOG_ERROR("BBU heartbeat timeout, resetting connection");
    g_channel_established = false;
    heartbeat_stop();
    /* TCP自动重连，重新进入通道建立流程 */
}
```

## 工作流程

```
通道建立完成
    ↓
heartbeat_start()
    ↓
主循环（每秒执行）
    ↓
┌─────────────────────────────────┐
│ 每3秒发送PAAU心跳 (MsgID: 171)  │
│ 接收BBU心跳 (MsgID: 181)        │
│ 检查BBU心跳超时                 │
└─────────────────────────────────┘
    ↓
如果超时（20次，约60秒）
    ↓
停止心跳
    ↓
重置通道建立标志
    ↓
TCP自动重连
    ↓
重新进入通道建立流程
```

## 消息格式

### PAAU在位心跳消息

- **消息ID**: 171 (MSG_PAAU_HEARTBEAT)
- **长度**: 15字节（仅消息头）
- **Payload**: 空

### BBU在位心跳消息

- **消息ID**: 181 (MSG_BBU_HEARTBEAT)
- **长度**: 15字节（仅消息头）
- **Payload**: 空

## 异常处理

### PAAU未收到BBU心跳（超时）

1. 记录错误日志
2. 停止心跳
3. 重置通道建立标志
4. TCP自动重连
5. 重新进入通道建立流程
6. 在通道建立请求中上报告警码（TODO）

### BBU未收到PAAU心跳（超时）

由BBU侧处理：
1. 上报网管
2. 删除该相控阵上配置的所有波束资源

## 日志示例

### 正常运行

```
[INFO] Heartbeat manager initialized
[INFO] Channel established, heartbeat started
[DEBUG] Sent PAAU heartbeat (count=1)
[DEBUG] Received BBU heartbeat
[DEBUG] Sent PAAU heartbeat (count=2)
[DEBUG] Received BBU heartbeat
```

### 超时重连

```
[ERROR] BBU heartbeat timeout (missed 20 times, 60 seconds)
[ERROR] BBU heartbeat timeout, resetting connection
[INFO] Heartbeat stopped
[WARN] TCP disconnected from BBU
[INFO] TCP connected to BBU
[INFO] Sent channel setup request (serial_num=X, XXX bytes)
```

## 测试要点

1. **正常心跳**:
   - 通道建立后，验证每3秒发送一次PAAU心跳
   - 验证收到BBU心跳时日志正确

2. **超时检测**:
   - 模拟BBU停止发送心跳
   - 验证60秒后检测到超时
   - 验证自动重连

3. **重连恢复**:
   - TCP断开后验证心跳停止
   - 重连后验证心跳重新启动

## 注意事项

1. **启动时机**: 心跳在通道建立完成后启动，不是TCP连接后
2. **停止时机**: TCP断开时必须停止心跳
3. **线程安全**: 心跳管理器使用互斥锁保护
4. **自动重连**: 超时后TCP自动重连，无需手动处理
5. **系统时间**: 不再发送到FPGA，只设置Linux系统时间

## 文档

详细文档请参考：
- `HEARTBEAT_README.md`: 心跳功能详细文档
- `CHANNEL_SETUP_README.md`: 通道建立流程文档（已更新）
- `IMPLEMENTATION_SUMMARY.md`: 实现总结（已更新）

## 编译和运行

```bash
# 编译
make clean
make

# 运行（需要root权限以设置系统时间）
sudo ./antenna_mgmt

# 查看日志
tail -f logs/antenna_mgmt.log
```

## 兼容性

- 向后兼容之前的实现
- 新增心跳功能不影响现有功能
- 系统时间同步修正不影响其他功能
