# 通道建立功能实现总结

## 实现内容

已完成通道建立请求流程的完整实现，包括周期发送、流水号管理、系统时间同步等功能。

### 1. 配置文件扩展

**文件**: `config/antenna_mgmt.conf`

新增字段：
- 相控阵产品标识（厂商、产品名、序列号、日期等）
- 软件版本信息（软件版本、固件版本）

### 2. 通道建立模块

**文件**:
- `include/channel_setup.h`
- `src/channel_setup.c`

**功能**:
- `channel_setup_create_request()`: 生成通道建立请求消息（MSG_CHANNEL_SETUP_REQ）
  - 包含6个IE：产品标识、建立原因、相控阵能力、硬件信息、软件版本、频段能力
  - 优先使用FPGA真实值（波束数、最大功率、硬件类型等）
  - 从配置文件读取产品标识和版本信息

- `channel_setup_handle_config()`: 处理通道建立配置消息（MSG_CHANNEL_SETUP_CFG）
  - 解析IE 501（系统时间），设置Linux系统时间并同步到FPGA
  - 解析IE 502（CPU统计周期）
  - 解析IE 503（相控阵操作模式），并发送到FPGA
  - 解析IE 504（CPRI工作模式），并发送到FPGA

- `channel_setup_create_response()`: 生成通道建立响应消息（MSG_CHANNEL_SETUP_CFG_ACK）
  - 包含IE 21（建立结果）

### 3. 消息处理器更新

**文件**: `src/msg_handler.c`

- 引入channel_setup模块
- 实现`handle_channel_setup_req()`函数
  - 调用`channel_setup_handle_config()`处理配置
  - 调用`channel_setup_create_response()`生成响应
  - **复制请求消息的流水号到响应消息**
  - 通过TCP发送响应消息

### 4. 主程序集成

**文件**: `src/main.c`

- 引入channel_setup模块
- 添加全局变量：
  - `g_channel_established`: 通道建立标志
  - `g_serial_num`: PAAU侧流水号计数器
- 在`on_tcp_connected()`回调中立即发送第一次通道建立请求
- 在`on_tcp_disconnected()`回调中重置通道建立标志
- 在`on_tcp_data_received()`中检测通道建立配置消息，设置标志
- **主循环中实现周期发送机制**：
  - 如果通道未建立且TCP已连接，每5秒发送一次请求
  - 收到配置后停止周期发送
  - 每次发送使用递增的流水号

### 5. 文档

**文件**: `CHANNEL_SETUP_README.md`

详细文档包括：
- 流程说明
- IE列表和数据来源
- 配置文件字段说明
- FPGA状态使用说明
- 代码结构和关键函数
- 使用示例和故障排查

## 关键特性

### 1. 智能数据填充

- **FPGA真实值优先**: IE 3（相控阵能力）和IE 5（硬件信息）优先使用FPGA状态中的真实值
- **配置文件读取**: 产品标识和版本信息从配置文件读取
- **默认值兜底**: 如果FPGA状态未就绪，使用合理的默认值

### 2. 周期发送机制

- **5秒周期**: 通道未建立时，每5秒发送一次通道建立请求
- **自动停止**: 收到BBU的通道建立配置后，自动停止周期发送
- **重连恢复**: TCP断开重连后，重新开始周期发送
- **立即发送**: TCP连接建立后立即发送第一次请求，不等待5秒

### 3. 流水号管理

- **独立计数**: PAAU维护自己的流水号，从1开始递增
- **请求递增**: 每次发送请求使用新的流水号
- **响应回传**: 响应消息带回请求消息中的流水号
- **符合协议**: BBU和PAAU分别统计流水号

### 4. 系统时间同步

- **IE 501处理**: 收到系统时间IE后，设置Linux系统时间
- **FPGA同步**: 设置成功后自动同步到FPGA
- **权限要求**: 需要root权限才能设置系统时间
- **错误处理**: 设置失败时记录错误日志

### 5. FPGA命令转发

- **IE 503**: 收到相控阵操作模式配置时，调用`fpga_send_cpri_config()`
- **IE 504**: 收到CPRI工作模式配置时，调用`fpga_send_cpri_mode()`
- **自动转发**: 无需手动干预，自动转发到FPGA

### 6. 小端序处理

- 所有IE编码使用小端序（CPRI协议要求）
- 与FPGA通信的大端序分离处理

## 数据流向

```
配置文件 (antenna_mgmt.conf)
    ↓
产品标识、版本信息
    ↓
channel_setup_create_request()
    ↑
FPGA状态 (fpga_status_frame_t)
    ↓
波束数、功率、硬件信息
    ↓
通道建立请求 (MSG_CHANNEL_SETUP_REQ, serial_num=1)
    ↓
TCP发送到BBU
    ↓
    ... (5秒后未收到响应)
    ↓
通道建立请求 (MSG_CHANNEL_SETUP_REQ, serial_num=2)
    ↓
TCP发送到BBU
    ↓
    ... (BBU处理)
    ↓
通道建立配置 (MSG_CHANNEL_SETUP_CFG, serial_num=X)
    ↓
停止周期发送
    ↓
channel_setup_handle_config()
    ↓
IE 501: 设置系统时间 → fpga_send_system_time()
IE 503: 操作模式 → fpga_send_cpri_config()
IE 504: CPRI模式 → fpga_send_cpri_mode()
    ↓
UART发送到FPGA
    ↓
channel_setup_create_response()
    ↓
复制BBU请求的流水号 (serial_num=X)
    ↓
通道建立响应 (MSG_CHANNEL_SETUP_CFG_ACK, serial_num=X)
    ↓
TCP发送到BBU
    ↓
通道建立完成
```

## 扩展点

系统时间同步（IE 501）、相控阵操作模式配置（IE 503）和CPRI工作模式配置（IE 504）已经实现。

### 添加其他IE处理

在`channel_setup_handle_config()`中添加新的case分支即可。

## 测试建议

1. **配置文件测试**:
   - 修改产品标识字段，验证是否正确发送
   - 修改版本信息，验证是否正确发送

2. **FPGA状态测试**:
   - 在FPGA状态就绪前发送请求，验证使用默认值
   - 在FPGA状态就绪后发送请求，验证使用真实值

3. **周期发送测试**:
   - 启动程序，观察是否每5秒发送一次请求
   - 模拟BBU发送配置，验证是否停止周期发送
   - 断开TCP连接后重连，验证是否重新开始周期发送

4. **流水号测试**:
   - 观察每次请求的流水号是否递增
   - 验证响应消息是否带回请求的流水号

5. **系统时间同步测试**:
   - 发送IE 501，验证系统时间是否更新
   - 验证FPGA是否收到时间同步命令
   - 测试权限不足时的错误处理

6. **IE处理测试**:
   - 发送IE 503，验证FPGA是否收到操作模式命令
   - 发送IE 504，验证FPGA是否收到CPRI工作模式命令
   - 发送IE 502，验证日志是否正确记录

7. **完整流程测试**:
   - 启动程序，观察通道建立请求是否自动发送
   - 模拟BBU发送配置，验证响应是否正确
   - 验证通道建立完成后不再发送请求

## 编译和运行

```bash
# 编译
cd s_band_antenna_mgmt
make clean
make

# 运行（需要root权限以设置系统时间）
sudo ./antenna_mgmt

# 查看日志
tail -f logs/antenna_mgmt.log
```

日志中会显示：
- TCP连接状态
- 通道建立请求发送（包含流水号）
- 周期发送计时
- 通道建立配置接收
- 系统时间设置结果
- FPGA命令发送
- 通道建立响应发送（包含流水号）
- 通道建立完成标志

## 注意事项

1. 确保配置文件中所有产品标识字段都已填写
2. 字符串长度不要超过字段限制（通常16字节）
3. FPGA状态未就绪时会使用默认值，不影响通道建立
4. 收到CPRI工作模式配置后会立即发送到FPGA，确保FPGA已准备好
5. **需要root权限才能设置系统时间**，否则会记录错误日志
6. **通道建立请求每5秒发送一次**，直到收到BBU配置
7. **响应消息必须带回请求的流水号**，确保流水号正确复制
8. TCP断开重连后会重新开始周期发送通道建立请求
