# 通道建立流程实现文档

## 概述

本文档描述了S频段天线管理软件中通道建立流程的实现，包括通道建立请求、配置和响应的完整握手过程。

## 流程说明

### 周期发送机制

**重要**: PAAU周期发送通道建立请求，直到收到BBU发送的通道建立配置为止，发送周期为5秒。

- TCP连接建立后，立即发送第一次通道建立请求
- 如果5秒内未收到BBU的通道建立配置，继续发送请求
- 收到通道建立配置后，停止周期发送
- TCP断开重连后，重新开始周期发送

### 流水号处理

**重要**: BBU和相控阵侧分别统计流水号。BBU向相控阵发一条请求消息，相控阵也只向BBU回一条响应消息，并且相控阵响应消息中带回相同的流水号。

- PAAU维护自己的流水号计数器（从1开始递增）
- 发送通道建立请求时，使用PAAU侧的流水号
- 收到BBU的通道建立配置时，响应消息带回BBU请求中的流水号

### 1. 通道建立请求 (MSG_CHANNEL_SETUP_REQ, ID=1)

**方向**: PAAU -> BBU
**触发时机**: TCP连接建立成功后自动发送
**实现位置**: [src/channel_setup.c:channel_setup_create_request()](src/channel_setup.c)

#### 包含的IE列表

| IE ID | IE名称 | 数据来源 | 说明 |
|-------|--------|----------|------|
| 1 | 相控阵产品标识 | 配置文件 | 从antenna_mgmt.conf读取产品信息 |
| 2 | 通道建立原因 | 参数传入 | 0=上电, 1=复位, 2=重连 |
| 3 | 相控阵能力 | FPGA状态 | 优先使用FPGA真实值，否则使用默认值 |
| 5 | 硬件信息 | FPGA状态 | 优先使用FPGA真实值，否则使用默认值 |
| 6 | 软件版本信息 | 配置文件 | 从antenna_mgmt.conf读取版本信息 |
| 7 | 频段能力 | 固定值 | S频段: 2.0-2.3 GHz |

#### 配置文件字段

在 `config/antenna_mgmt.conf` 中配置以下字段：

```ini
# 相控阵产品标识
PRODUCT_MANUFACTURER=StarNet
PRODUCT_NAME=S-Band PAAU
PRODUCT_SERIAL=SN20260311001
PRODUCT_DATE=2026-03-11
PRODUCT_SERVICE_DATE=2026-03-11
PRODUCT_INFO=S-Band Antenna

# 软件版本信息
SOFTWARE_VERSION=v1.0.0
FIRMWARE_VERSION=FPGA_v1.0.0
```

#### FPGA状态使用

- **IE 3 (相控阵能力)**:
  - `beam_count`: 从FPGA状态的 `beam_count` 字段获取
  - `max_tx_power`: 从FPGA状态的 `max_tx_power` 字段获取
  - `supported_modes`: 从FPGA状态的 `supported_modes` 字段获取
  - 如果FPGA状态未就绪，使用默认值: 16波束, 46dBm, 支持TDD-LTE和TDD-NR

- **IE 5 (硬件信息)**:
  - `hw_type`: 从FPGA状态的 `hw_type` 字段获取 (32字节)
  - `hw_version`: 从FPGA状态的 `hw_version` 字段获取 (4字节)
  - 如果FPGA状态未就绪，使用默认值

### 2. 通道建立配置 (MSG_CHANNEL_SETUP_CFG, ID=2)

**方向**: BBU -> PAAU
**触发时机**: BBU收到通道建立请求后下发
**实现位置**: [src/channel_setup.c:channel_setup_handle_config()](src/channel_setup.c)

#### 处理的IE列表

| IE ID | IE名称 | 处理动作 |
|-------|--------|----------|
| 501 | 系统时间配置 | 设置Linux系统时间，并发送到FPGA |
| 502 | CPU占用率统计周期配置 | 记录日志（TODO: 实现CPU统计） |
| 503 | 相控阵操作模式配置 | 发送操作模式到FPGA |
| 504 | CPRI口工作模式配置 | 发送工作模式到FPGA |

#### FPGA命令发送

**IE 501 (系统时间配置)**:
- 解析时间字段: 秒(1B), 分(1B), 时(1B), 日(1B), 月(1B), 年(1B)
- 使用`clock_settime()`设置Linux系统时间
- 注意: 需要root权限才能设置系统时间
- 注意: 系统时间不发送到FPGA

**IE 503 (相控阵操作模式配置)**:
- 解析操作模式值（4字节）
- 调用`fpga_send_cpri_config(mode)`发送到FPGA

**IE 504 (CPRI口工作模式配置)**:
- 解析主光纤端口号、辅光纤端口号、工作模式
- 调用`fpga_send_cpri_mode(work_mode)`发送到FPGA
- 工作模式值: 0=独立, 1=级联, 2=环形

### 3. 通道建立响应 (MSG_CHANNEL_SETUP_CFG_ACK, ID=3)

**方向**: PAAU -> BBU
**触发时机**: 处理完通道建立配置后自动发送
**实现位置**: [src/channel_setup.c:channel_setup_create_response()](src/channel_setup.c)

#### 包含的IE列表

| IE ID | IE名称 | 内容 |
|-------|--------|------|
| 21 | 通道建立响应 | 结果码: 0=成功, 1=失败 |

## 代码结构

### 文件列表

- `include/channel_setup.h`: 通道建立模块头文件
- `src/channel_setup.c`: 通道建立模块实现
- `src/msg_handler.c`: 消息处理器（调用通道建立模块）
- `src/main.c`: 主程序（TCP连接成功时触发通道建立请求）

### 关键函数

```c
/* 生成通道建立请求消息 */
int channel_setup_create_request(cpri_message_t *msg, channel_setup_reason_t reason);

/* 处理通道建立配置消息 */
int channel_setup_handle_config(const cpri_message_t *msg);

/* 生成通道建立响应消息 */
int channel_setup_create_response(cpri_message_t *msg, uint32_t result);
```

### 消息处理流程

```
TCP连接建立
    ↓
on_tcp_connected()
    ↓
channel_setup_create_request()  ← 读取配置文件 + FPGA状态
    ↓
发送通道建立请求 (MSG_CHANNEL_SETUP_REQ, serial_num=1)
    ↓
    ... (等待BBU响应，每5秒重发一次)
    ↓
5秒后未收到响应
    ↓
再次发送通道建立请求 (serial_num=2)
    ↓
    ... (继续等待)
    ↓
收到通道建立配置 (MSG_CHANNEL_SETUP_CFG, serial_num=X)
    ↓
停止周期发送
    ↓
handle_channel_setup_req()
    ↓
channel_setup_handle_config()  → 设置系统时间 + 发送命令到FPGA
    ↓
channel_setup_create_response()
    ↓
复制BBU请求的流水号 (serial_num=X)
    ↓
发送通道建立响应 (MSG_CHANNEL_SETUP_CFG_ACK, serial_num=X)
    ↓
通道建立完成，进入运行态
```

## IE编码格式

所有IE使用小端序（Little-Endian），格式如下：

```
+--------+--------+--------+--------+
| IE类型 | IE长度 | IE数据 | ...    |
| 2字节  | 2字节  | N字节  | ...    |
| (小端) | (小端) |        |        |
+--------+--------+--------+--------+
```

- **IE类型**: 2字节，标识IE类型
- **IE长度**: 2字节，包含IE头(4字节) + IE数据长度
- **IE数据**: 变长，根据IE类型不同

## 使用示例

### 1. 配置产品信息

编辑 `config/antenna_mgmt.conf`:

```ini
PRODUCT_MANUFACTURER=YourCompany
PRODUCT_NAME=Your Product Name
PRODUCT_SERIAL=SN2026XXXXXXX
PRODUCT_DATE=2026-03-11
SOFTWARE_VERSION=v2.0.0
FIRMWARE_VERSION=FPGA_v2.0.0
```

### 2. 启动程序

```bash
./antenna_mgmt
```

程序启动后会自动：
1. 连接到BBU服务器
2. 发送通道建立请求
3. 等待并处理通道建立配置
4. 发送通道建立响应
5. 进入正常运行状态

### 3. 查看日志

```bash
tail -f logs/antenna_mgmt.log
```

日志中会显示：
- TCP连接状态
- 通道建立请求发送
- 通道建立配置接收和处理
- FPGA命令发送
- 通道建立响应发送

## 注意事项

1. **FPGA状态依赖**:
   - IE 3和IE 5优先使用FPGA真实值
   - 如果FPGA状态未就绪（首次状态查询未完成），使用默认值
   - 建议等待FPGA状态就绪后再发送通道建立请求

2. **配置文件完整性**:
   - 确保所有产品标识字段都已配置
   - 字符串长度不要超过字段限制（16字节）

3. **周期发送**:
   - 通道建立请求每5秒发送一次，直到收到BBU配置
   - 收到配置后自动停止周期发送
   - TCP断开重连后会重新开始周期发送

4. **流水号管理**:
   - PAAU维护自己的流水号，从1开始递增
   - 响应消息必须带回请求消息中的流水号
   - 不同的请求使用不同的流水号

5. **系统时间同步**:
   - 收到IE 501后会设置Linux系统时间
   - 需要root权限才能设置系统时间
   - 设置成功后会同步到FPGA

6. **CPRI工作模式**:
   - 收到IE 503或IE 504后会立即发送到FPGA
   - 确保FPGA已准备好接收该命令

7. **错误处理**:
   - 如果通道建立失败，会在响应中返回失败码
   - 检查日志了解失败原因

## 扩展功能

系统时间同步、操作模式配置和CPRI工作模式配置已经实现。

### 其他扩展示例

如需添加其他IE处理，在 `channel_setup_handle_config()` 中添加新的case分支：

```c
case IE_TYPE_YOUR_NEW_IE: {
    /* 解析IE数据 */
    /* 执行相应操作 */
    /* 记录日志 */
    break;
}
```

## 故障排查

| 问题 | 可能原因 | 解决方案 |
|------|----------|----------|
| 通道建立请求未发送 | TCP连接失败 | 检查BBU IP和端口配置 |
| 持续周期发送请求 | BBU未响应配置消息 | 检查BBU服务器状态和网络 |
| IE数据不正确 | 配置文件缺失 | 检查antenna_mgmt.conf完整性 |
| FPGA命令发送失败 | UART未连接 | 检查UART设备和权限 |
| 系统时间设置失败 | 权限不足 | 使用root权限运行程序 |
| 流水号不匹配 | 响应未带回请求流水号 | 检查msg_handler中的流水号复制逻辑 |
| 通道建立超时 | BBU未响应 | 检查BBU服务器状态和网络 |
