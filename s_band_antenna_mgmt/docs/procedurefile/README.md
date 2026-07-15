# S频段天线管理面软件

## 项目简介

S频段天线管理面软件是一个基于Linux的嵌入式软件，用于管理星载天线系统（PAAU）与基带单元（BBU）之间的通信。软件实现了：
- **CPRI接口协议**：通过TCP/IP与BBU通信
- **UART串口通信**：通过串口与FPGA通信
- **双向数据转换**：FPGA状态与CPRI消息的相互转换

## 系统架构

```
┌──────────────────────────────────────────────────────┐
│              S频段天线管理面软件                      │
│                                                      │
│  ┌──────────┐      ┌──────────────┐                │
│  │ 主程序   │─────▶│ 配置管理模块 │                │
│  └──────────┘      └──────────────┘                │
│       │                                             │
│       ├──────┬──────────┬──────────┬──────────┐   │
│       ▼      ▼          ▼          ▼          ▼   │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌──┐│
│  │TCP客户端│ │UART客户端│ │CPRI协议│ │FPGA协议│ │日志││
│  │  模块  │ │  模块  │ │编解码  │ │编解码  │ │模块││
│  └───┬────┘ └───┬────┘ └────────┘ └────────┘ └──┘│
│      │          │                                  │
└──────┼──────────┼──────────────────────────────────┘
       │          │
       │ TCP/IP   │ UART (921600-8N1)
       ▼          ▼
  ┌─────────┐  ┌──────┐
  │   BBU   │  │ FPGA │
  │ Server  │  │      │
  └─────────┘  └──────┘
```

## 核心功能

### 1. TCP客户端连接管理（BBU通信）
- PAAU作为TCP Client，主动连接BBU Server
- 默认连接参数：BBU IP 10.10.10.6:30000
- 支持断线自动重连（默认3秒重连间隔）
- 多线程架构：接收线程 + 重连线程

### 2. UART串口通信（FPGA通信）
- 串口设备：/dev/ttyAMA1
- 波特率：921600，8位数据位，无校验，1位停止位
- 大端字节序传输
- 支持12种下发消息类型
- 自动轮询FPGA状态（1秒/次）
- 解析133字节状态响应帧

### 3. CPRI协议支持
- 完整的CPRI消息编解码
- 支持30+种消息类型
- 消息头格式：15字节（消息ID + 长度 + PAAU ID + BBU ID + 端口号 + 流水号）
- 支持IE（Information Element）结构
- 小端字节序

### 4. FPGA协议支持
- 帧格式：帧头(0xEB90) + 消息ID + 载荷 + 帧尾(0x5555)
- 支持12种控制消息
- 状态查询响应（133字节）
- 特殊频率编解码算法
- 大端字节序

### 5. 状态转换与上报
- FPGA状态自动转换为CPRI消息
- 告警检测与自动上报
- 温度监控（3个温度点）
- 本振状态监控
- 时钟同步状态监控
  - 通道建立
  - 心跳消息
  - 状态查询
  - 参数配置
  - 版本管理
  - 告警上报

### 4. 配置管理
- 基于文本的配置文件
- 支持运行时配置加载
- 可配置项：
  - BBU服务器地址和端口
  - PAAU客户端地址
  - 重连间隔
  - 心跳间隔
  - 日志级别

### 5. 日志系统
- 多级别日志：DEBUG/INFO/WARN/ERROR
- 同时输出到控制台和文件
- 线程安全
- 带时间戳和源码位置

## 目录结构

```
s_band_antenna_mgmt/
├── src/                       # 源代码目录
│   ├── main.c                # 主程序
│   ├── tcp_client.c          # TCP客户端实现
│   ├── uart_client.c         # UART客户端实现
│   ├── cpri_protocol.c       # CPRI协议编解码
│   ├── fpga_protocol.c       # FPGA协议编解码
│   ├── msg_handler.c         # CPRI消息处理器
│   ├── fpga_handler.c        # FPGA消息处理器
│   ├── fpga_to_cpri.c        # FPGA状态到CPRI转换
│   ├── config.c              # 配置管理
│   └── logger.c              # 日志模块
├── include/                  # 头文件目录
│   ├── common.h              # 通用定义
│   ├── tcp_client.h          # TCP客户端接口
│   ├── uart_client.h         # UART客户端接口
│   ├── cpri_protocol.h       # CPRI协议定义
│   ├── fpga_protocol.h       # FPGA协议定义
│   ├── msg_handler.h         # CPRI消息处理接口
│   ├── fpga_handler.h        # FPGA处理接口
│   ├── fpga_to_cpri.h        # 状态转换接口
│   ├── config.h              # 配置管理接口
│   └── logger.h              # 日志接口
├── config/                   # 配置文件目录
│   └── antenna_mgmt.conf     # 主配置文件
├── logs/                     # 日志文件目录
├── build/                    # 编译输出目录
├── Makefile                  # 编译脚本
├── README.md                 # 本文档
└── UART_FPGA_README.md       # UART/FPGA详细文档
```

## 编译和运行

### 编译

```bash
# 编译程序
make

# 清理编译产物
make clean

# 查看帮助
make help
```

### 运行

```bash
# 使用默认配置运行
./antenna_mgmt

# 指定配置文件运行
./antenna_mgmt /path/to/config.conf

# 直接编译并运行
make run
```

### 调试

```bash
# 使用GDB调试
make debug
```

### 安装

```bash
# 安装到系统目录
make install
```

## 配置说明

配置文件位于 `config/antenna_mgmt.conf`，主要配置项：

```ini
# BBU服务器配置
BBU_IP=10.10.10.6          # BBU服务器IP地址
BBU_PORT=30000             # BBU服务器端口

# PAAU客户端配置
PAAU_IP=10.10.10.8         # PAAU客户端IP地址
PAAU_ID=0                  # PAAU设备ID

# UART配置（与FPGA通信）
UART_DEVICE=/dev/ttyAMA1   # UART设备路径

# 重连配置
RECONNECT_INTERVAL_MS=3000 # 重连间隔（毫秒）

# 心跳配置
HEARTBEAT_INTERVAL_MS=3000 # 心跳间隔（毫秒）

# 日志配置
LOG_LEVEL=INFO             # 日志级别：DEBUG/INFO/WARN/ERROR
LOG_FILE=./logs/antenna_mgmt.log  # 日志文件路径
```

## 开发指南

### 添加新的消息处理器

1. 在 `msg_handler.h` 中声明处理函数：
```c
int handle_new_message(const cpri_message_t *msg);
```

2. 在 `msg_handler.c` 中实现处理函数：
```c
int handle_new_message(const cpri_message_t *msg)
{
    LOG_INFO("Handle new message");
    // 实现处理逻辑
    return SUCCESS;
}
```

3. 在 `g_msg_handlers` 映射表中注册：
```c
{MSG_NEW_TYPE, handle_new_message},
```

### 发送CPRI消息

```c
cpri_message_t msg;
memset(&msg, 0, sizeof(msg));

// 填充消息头
msg.header.msg_id = MSG_PAAU_HEARTBEAT;
msg.header.paau_id = 0;
msg.header.bbu_id = 0;
msg.header.port_num = 0;
msg.header.serial_num = get_next_serial_num();

// 填充载荷（如果需要）
msg.payload = payload_data;
msg.payload_len = payload_size;

// 编码消息
uint8_t buffer[1024];
int len = cpri_encode_message(&msg, buffer, sizeof(buffer));

// 发送消息
tcp_client_send(&g_tcp_client, buffer, len);
```

## 后续开发计划

- [ ] 实现完整的通道建立流程
- [ ] 实现心跳机制
- [ ] 实现状态查询和参数配置
- [ ] 实现版本管理功能
- [ ] 实现告警上报功能
- [ ] 完善FPGA消息处理逻辑
- [ ] 实现BBU配置到FPGA的转发
- [ ] 添加单元测试
- [ ] 添加性能监控
- [ ] 支持多BBU连接

## 相关文档

- [UART_FPGA_README.md](UART_FPGA_README.md) - UART与FPGA通信详细文档
- [19-星载天线与基带接口技术要求-option8（CPRI）-V1.6.pdf](../天线管里面需求/) - CPRI接口协议文档

## 技术规格

- 开发语言：C
- 编译器：GCC
- 操作系统：Linux
- 线程库：POSIX Threads (pthread)
- 网络协议：TCP/IP
- 串口协议：UART (921600-8N1)
- 应用协议：CPRI (自定义协议) + FPGA协议

## 许可证

内部项目，版权所有。

## 联系方式

如有问题，请联系开发团队。
