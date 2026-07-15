# S频段天线管理面软件 - 项目总结

## 项目概述

本项目为S频段星载天线管理面软件，实现了PAAU（天线端）与BBU（基带端）、FPGA之间的双向通信管理。

## 项目规模

- **源文件**: 10个C文件
- **头文件**: 10个头文件
- **代码行数**: 约2000行
- **文档**: 2个Markdown文档
- **配置文件**: 1个配置文件

## 技术架构

### 通信接口

1. **TCP/IP接口（与BBU通信）**
   - 协议：CPRI（自定义协议）
   - 角色：TCP Client
   - 连接：10.10.10.6:30000
   - 字节序：小端（Little-Endian）
   - 特性：断线自动重连

2. **UART接口（与FPGA通信）**
   - 设备：/dev/ttyAMA1
   - 波特率：921600
   - 格式：8N1（8位数据，无校验，1位停止）
   - 字节序：大端（Big-Endian）
   - 特性：自动轮询（1秒/次）

### 核心模块

| 模块名称 | 文件 | 功能描述 |
|---------|------|---------|
| TCP客户端 | tcp_client.c/h | BBU连接管理、断线重连 |
| UART客户端 | uart_client.c/h | FPGA串口通信、帧解析 |
| CPRI协议 | cpri_protocol.c/h | CPRI消息编解码 |
| FPGA协议 | fpga_protocol.c/h | FPGA消息编解码、频率算法 |
| CPRI消息处理 | msg_handler.c/h | CPRI消息分发处理 |
| FPGA消息处理 | fpga_handler.c/h | FPGA状态管理、消息发送 |
| 状态转换 | fpga_to_cpri.c/h | FPGA状态转CPRI消息 |
| 配置管理 | config.c/h | 配置文件解析 |
| 日志系统 | logger.c/h | 多级别日志输出 |
| 主程序 | main.c | 系统初始化、主循环 |

## 关键特性

### 1. 双协议支持
- **CPRI协议**：30+种消息类型，小端序，TCP传输
- **FPGA协议**：12种控制消息，大端序，UART传输

### 2. 多线程架构
- TCP接收线程
- TCP重连线程
- UART接收线程
- UART轮询线程

### 3. 状态管理
- FPGA状态实时监控（133字节状态帧）
- 状态缓存与互斥保护
- 自动告警检测与上报

### 4. 频率编解码
- 特殊压缩算法：商|余数格式
- 编码：freq_khz -> (freq/50000) | ((freq%50000)*2)
- 解码：逆运算恢复物理频率

### 5. 告警监控
- 温度超限检测（3个温度点）
- 本振失锁检测
- 时钟失步检测
- 自动生成CPRI告警消息

## 支持的消息类型

### CPRI消息（BBU <-> PAAU）
- 通道建立（请求/配置/应答）
- 版本管理（查询/下载/激活）
- 状态查询（PAAU状态/参数）
- 参数配置
- 校准指示
- 环回测试
- 告警上报
- 日志上传
- 复位指示
- 心跳消息
- NR小区配置

### FPGA消息（PAAU <-> FPGA）
- 相控阵工作模式（0x01）
- CPRI工作模式（0x02）
- 指令波束数（0x03）
- 业务波束数（0x04）
- 相控阵复位（0x05）
- 校准指示（0x06）
- 环回控制（0x07）
- 频率与带宽（0x08）
- 波束次数（0x09）
- 系统时间（0x0A）
- 发射控制（0x0B）
- 状态检询（0xFF）

## 数据流向

```
BBU (TCP Server)
    ↕ CPRI协议（小端）
PAAU (TCP Client)
    ↕ 状态转换
FPGA状态管理
    ↕ FPGA协议（大端）
FPGA (UART)
```

## 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| BBU_IP | 10.10.10.6 | BBU服务器IP |
| BBU_PORT | 30000 | BBU服务器端口 |
| PAAU_IP | 10.10.10.8 | PAAU客户端IP |
| UART_DEVICE | /dev/ttyAMA1 | UART设备路径 |
| RECONNECT_INTERVAL_MS | 3000 | TCP重连间隔 |
| HEARTBEAT_INTERVAL_MS | 3000 | 心跳间隔 |
| LOG_LEVEL | INFO | 日志级别 |

## 编译与运行

```bash
# 编译
make

# 运行
./antenna_mgmt

# 或使用启动脚本
./start.sh

# 清理
make clean
```

## 日志输出

日志同时输出到：
- 控制台（stdout）
- 日志文件（./logs/antenna_mgmt.log）

日志级别：DEBUG < INFO < WARN < ERROR

## 开发建议

### 添加新的CPRI消息处理
1. 在 `cpri_protocol.h` 中定义消息类型
2. 在 `msg_handler.h` 中声明处理函数
3. 在 `msg_handler.c` 中实现处理函数
4. 在 `g_msg_handlers` 映射表中注册

### 添加新的FPGA消息
1. 在 `fpga_protocol.h` 中定义消息ID
2. 在 `fpga_handler.h` 中声明发送函数
3. 在 `fpga_handler.c` 中实现发送函数

### 调试技巧
1. 设置 `LOG_LEVEL=DEBUG` 查看详细日志
2. 使用 `make debug` 启动GDB调试
3. 检查串口权限：`sudo chmod 666 /dev/ttyAMA1`
4. 监控串口数据：`minicom -D /dev/ttyAMA1 -b 921600`

## 注意事项

1. **字节序差异**：CPRI使用小端，FPGA使用大端，注意转换
2. **频率单位**：FPGA使用压缩格式，必须使用专用编解码函数
3. **线程安全**：状态管理器使用互斥锁保护
4. **UART权限**：确保程序有权限访问串口设备
5. **首次状态**：首次状态查询响应前，状态无效

## 后续扩展

- [ ] 完善BBU消息处理逻辑
- [ ] 实现BBU配置到FPGA的转发
- [ ] 添加心跳机制
- [ ] 实现版本管理
- [ ] 添加单元测试
- [ ] 性能优化
- [ ] 错误恢复机制

## 文档索引

- [README.md](README.md) - 项目主文档
- [UART_FPGA_README.md](UART_FPGA_README.md) - UART/FPGA详细文档
- [config/antenna_mgmt.conf](config/antenna_mgmt.conf) - 配置文件

## 版本信息

- **版本**: v1.0
- **日期**: 2026-03-11
- **作者**: 开发团队
- **状态**: 框架完成，待业务逻辑完善
