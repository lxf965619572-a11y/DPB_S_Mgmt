# 通道建立功能更新说明

## 更新日期
2026-03-12

## 更新内容

### 1. 周期发送机制

**需求**: PAAU周期发送通道建立请求，直到收到BBU发送的通道建立配置为止，发送周期为5s。

**实现**:
- TCP连接建立后，立即发送第一次通道建立请求
- 如果5秒内未收到BBU的通道建立配置，继续发送请求
- 收到通道建立配置后，停止周期发送
- TCP断开重连后，重新开始周期发送

**代码位置**: `src/main.c`
- 添加全局变量 `g_channel_established` 标记通道建立状态
- 主循环中实现5秒定时器和周期发送逻辑
- `on_tcp_data_received()` 中检测通道建立配置消息

### 2. 流水号管理

**需求**: BBU和相控阵侧分别统计流水号。BBU向相控阵发一条请求消息，相控阵也只向BBU回一条响应消息，并且相控阵响应消息中带回相同的流水号。

**实现**:
- PAAU维护自己的流水号计数器 `g_serial_num`（从1开始递增）
- 每次发送通道建立请求时，使用新的流水号
- 收到BBU的通道建立配置时，响应消息带回BBU请求中的流水号

**代码位置**:
- `src/main.c`: 添加 `g_serial_num` 全局变量，发送请求时递增
- `src/msg_handler.c`: 响应消息复制请求消息的流水号

### 3. 系统时间同步

**需求**: 收到系统时间IE时，要将Linux系统的时间设置成配置的时间。

**实现**:
- 解析IE 501（系统时间配置）
- 使用 `clock_settime()` 设置Linux系统时间
- 设置成功后调用 `fpga_send_system_time()` 同步到FPGA
- 需要root权限才能设置系统时间

**代码位置**: `src/channel_setup.c`
- 添加 `IE_TYPE_SYSTEM_TIME` 定义
- 在 `channel_setup_handle_config()` 中处理IE 501

### 4. 相控阵操作模式配置

**需求**: 收到相控阵操作模式IE时，需要将模式发送给FPGA。

**实现**:
- 解析IE 503（相控阵操作模式配置）
- 调用 `fpga_send_cpri_config(mode)` 发送到FPGA

**代码位置**: `src/channel_setup.c`
- 添加 `IE_TYPE_OPERATION_MODE` 定义
- 在 `channel_setup_handle_config()` 中处理IE 503

### 5. CPRI口工作模式配置

**需求**: 收到CPRI口工作模式配置IE时，需要发送指令给FPGA。

**实现**:
- 解析IE 504（CPRI口工作模式配置）
- 调用 `fpga_send_cpri_mode(work_mode)` 发送到FPGA

**代码位置**: `src/channel_setup.c`
- 在 `channel_setup_handle_config()` 中处理IE 504（已实现，本次更新完善）

## 修改的文件

1. `src/channel_setup.c`
   - 添加系统时间同步处理（IE 501）
   - 添加相控阵操作模式配置处理（IE 503）
   - 完善CPRI工作模式配置处理（IE 504）
   - 添加必要的头文件（errno.h, string.h）

2. `src/msg_handler.c`
   - 响应消息复制请求消息的流水号
   - 复制BBU ID和端口号到响应消息

3. `src/main.c`
   - 添加 `g_channel_established` 全局变量
   - 添加 `g_serial_num` 全局变量
   - 实现周期发送机制（5秒定时器）
   - 在 `on_tcp_connected()` 中设置流水号
   - 在 `on_tcp_disconnected()` 中重置通道建立标志
   - 在 `on_tcp_data_received()` 中检测通道建立配置消息

4. `CHANNEL_SETUP_README.md`
   - 更新文档，添加周期发送机制说明
   - 添加流水号处理说明
   - 添加系统时间同步说明
   - 更新IE处理列表
   - 更新消息处理流程图
   - 更新注意事项和故障排查

5. `IMPLEMENTATION_SUMMARY.md`
   - 更新实现总结，反映新功能
   - 更新关键特性说明
   - 更新数据流向图
   - 更新测试建议

## 使用说明

### 运行程序

```bash
# 需要root权限以设置系统时间
sudo ./antenna_mgmt
```

### 观察日志

```bash
tail -f logs/antenna_mgmt.log
```

日志中会显示：
- `Sent channel setup request (serial_num=1, XXX bytes)` - 第一次发送请求
- `Periodic channel setup request sent (serial_num=2)` - 周期发送请求
- `Received channel setup config, stopping periodic requests` - 收到配置，停止周期发送
- `System time IE received: YYYY-MM-DD HH:MM:SS` - 收到系统时间IE
- `System time set successfully` - 系统时间设置成功
- `Operation mode configured: X` - 收到操作模式配置
- `CPRI work mode configured: main=X, sub=Y, mode=Z` - 收到CPRI工作模式配置
- `Sent channel setup response (serial_num=X)` - 发送响应，带回相同流水号

## 测试要点

1. **周期发送测试**:
   - 启动程序后，观察是否每5秒发送一次请求
   - 流水号是否递增（1, 2, 3, ...）
   - 收到配置后是否停止发送

2. **流水号测试**:
   - 响应消息的流水号是否与请求消息相同
   - 不同请求的流水号是否不同

3. **系统时间同步测试**:
   - 发送IE 501后，系统时间是否更新
   - 检查日志是否显示 "System time set successfully"
   - 如果权限不足，是否显示错误日志

4. **FPGA命令测试**:
   - 发送IE 503后，FPGA是否收到操作模式命令
   - 发送IE 504后，FPGA是否收到CPRI工作模式命令

## 注意事项

1. **Root权限**: 设置系统时间需要root权限，否则会失败并记录错误日志
2. **周期发送**: 通道未建立时会持续发送请求，这是正常行为
3. **流水号**: PAAU和BBU各自维护流水号，不要混淆
4. **TCP重连**: 断开重连后会重新开始周期发送，流水号继续递增
5. **FPGA状态**: 如果FPGA状态未就绪，会使用默认值，不影响通道建立

## 兼容性

- 向后兼容之前的实现
- 新增功能不影响现有功能
- 可以独立测试每个新功能
