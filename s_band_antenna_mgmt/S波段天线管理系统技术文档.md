# S波段天线管理系统技术文档

**版本**: v1.0
**日期**: 2026-03-18
**项目**: StarNet SC ANTENNA - S-Band Antenna Management System

---

## 目录

1. [系统概述](#1-系统概述)
2. [系统功能模块](#2-系统功能模块)
3. [技术架构](#3-技术架构)
4. [核心技术实现](#4-核心技术实现)
5. [API接口规范](#5-api接口规范)
6. [部署指南](#6-部署指南)
7. [使用手册](#7-使用手册)
8. [性能指标与监控](#8-性能指标与监控)
9. [安全策略与权限管理](#9-安全策略与权限管理)
10. [故障排查](#10-故障排查)

---

## 1. 系统概述

### 1.1 项目简介

S波段天线管理系统是一个基于Linux的嵌入式软件，用于管理星载天线系统（PAAU - Phased Array Antenna Unit）与基带单元（BBU - Base Band Unit）之间的通信。系统实现了双协议通信架构，通过CPRI协议与BBU通信，通过RS-422/UART协议与FPGA通信。

### 1.2 核心能力

- **双协议支持**: CPRI协议（TCP/IP）+ FPGA协议（UART RS-422）
- **双向数据转换**: FPGA状态与CPRI消息的相互转换
- **固件远程升级**: 支持通过BBU远程升级FPGA固件
- **实时状态监控**: 温度、本振、时钟、通道状态实时监控
- **告警管理**: 自动检测并上报硬件故障告警
- **高可靠性**: 断线自动重连、状态轮询、重传机制

### 1.3 技术规格

| 项目 | 规格 |
|------|------|
| 开发语言 | C (C11标准) |
| 编译器 | GCC 7.5+ |
| 操作系统 | Linux (Ubuntu 18.04 / D2000) |
| 线程库 | POSIX Threads (pthread) |
| 网络协议 | TCP/IP |
| 串口协议 | UART RS-422 (921600-8N1) |
| 代码规模 | 约2000行C代码 |

---

## 2. 系统功能模块

### 2.1 功能模块总览

系统由10个核心功能模块组成，分为通信层、协议层、业务层和支撑层四个层次：

```
┌─────────────────────────────────────────────────────────────┐
│                        业务层                                │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐      │
│  │通道建立  │ │心跳管理  │ │状态查询  │ │参数配置  │      │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐      │
│  │版本管理  │ │告警管理  │ │日志上传  │ │固件上注  │      │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘      │
├─────────────────────────────────────────────────────────────┤
│                        协议层                                │
│  ┌──────────────────┐         ┌──────────────────┐         │
│  │  CPRI协议编解码  │         │  FPGA协议编解码  │         │
│  │  (小端序)        │         │  (大端序)        │         │
│  └──────────────────┘         └──────────────────┘         │
├─────────────────────────────────────────────────────────────┤
│                        通信层                                │
│  ┌──────────────────┐         ┌──────────────────┐         │
│  │  TCP客户端模块   │         │  UART客户端模块  │         │
│  │  (BBU通信)       │         │  (FPGA通信)      │         │
│  └──────────────────┘         └──────────────────┘         │
├─────────────────────────────────────────────────────────────┤
│                        支撑层                                │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐      │
│  │配置管理  │ │日志系统  │ │状态管理  │ │消息分发  │      │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘      │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 通信管理模块

#### 2.2.1 TCP客户端模块

**功能描述**：
- 作为TCP Client主动连接BBU Server
- 实现断线自动重连机制
- 多线程架构：接收线程 + 重连线程
- 支持消息的发送和接收

**关键参数**：
- BBU服务器地址：10.10.10.6:30000（可配置）
- 重连间隔：3秒（可配置）
- 接收缓冲区：4096字节

**核心文件**：
- `src/tcp_client.c`
- `include/tcp_client.h`

#### 2.2.2 UART客户端模块

**功能描述**：
- 通过RS-422串口与FPGA通信
- 自动轮询FPGA状态（1秒/次）
- 支持12种下发消息类型
- 解析133字节状态响应帧

**关键参数**：
- 串口设备：/dev/ttyAMA1（可配置）
- 波特率：921600
- 数据位：8位
- 校验位：无
- 停止位：1位
- 字节序：大端（Big-Endian）

**核心文件**：
- `src/uart_client.c`
- `include/uart_client.h`
- `src/uart_rs422_client.c`
- `include/uart_rs422_client.h`

### 2.3 协议处理模块

#### 2.3.1 CPRI协议模块

**功能描述**：
- 实现CPRI消息的编解码
- 支持30+种消息类型
- 小端字节序处理
- IE（Information Element）结构支持

**消息格式**：
```
┌────────────────────────────────────────┐
│        CPRI消息头 (15字节)             │
│  ┌──────────────────────────────────┐  │
│  │ 消息ID (4B)                      │  │
│  │ 消息长度 (4B)                    │  │
│  │ PAAU ID (1B)                     │  │
│  │ BBU ID (1B)                      │  │
│  │ 端口号 (1B)                      │  │
│  │ 流水号 (4B)                      │  │
│  └──────────────────────────────────┘  │
├────────────────────────────────────────┤
│        载荷 (可变长度)                 │
│  ┌──────────────────────────────────┐  │
│  │ IE1: IE头(4B) + IE数据           │  │
│  │ IE2: IE头(4B) + IE数据           │  │
│  │ ...                              │  │
│  └──────────────────────────────────┘  │
└────────────────────────────────────────┘
```

**支持的消息类型**：

| 消息ID | 消息名称 | 方向 | 说明 |
|--------|----------|------|------|
| 1 | 通道建立请求 | PAAU→BBU | PAAU主动发起通道建立 |
| 2 | 通道建立配置 | BBU→PAAU | BBU下发配置参数 |
| 3 | 通道建立应答 | PAAU→BBU | PAAU确认配置 |
| 11 | PAAU版本查询 | BBU→PAAU | 查询当前版本信息 |
| 12 | 版本查询应答 | PAAU→BBU | 返回版本信息 |
| 21 | 版本下载请求 | BBU→PAAU | 下载新版本固件 |
| 22 | 版本下载应答 | PAAU→BBU | 下载结果反馈 |
| 31 | 版本激活指示 | BBU→PAAU | 激活指定版本 |
| 32 | 版本激活应答 | PAAU→BBU | 激活结果反馈 |
| 41 | PAAU状态查询 | BBU→PAAU | 查询PAAU状态 |
| 42 | 状态查询应答 | PAAU→BBU | 返回状态信息 |
| 51 | PAAU参数查询 | BBU→PAAU | 查询参数配置 |
| 52 | 参数查询应答 | PAAU→BBU | 返回参数信息 |
| 61 | PAAU参数配置 | BBU→PAAU | 配置参数 |
| 62 | 参数配置应答 | PAAU→BBU | 配置结果反馈 |
| 71 | 校准指示 | BBU→PAAU | 启动校准流程 |
| 72 | 校准应答 | PAAU→BBU | 校准结果反馈 |
| 81 | 环回测试请求 | BBU→PAAU | 启动环回测试 |
| 82 | 环回测试应答 | PAAU→BBU | 测试结果反馈 |
| 111 | 告警上报请求 | PAAU→BBU | 主动上报告警 |
| 131 | 日志上传请求 | BBU→PAAU | 请求上传日志 |
| 141 | 复位指示 | BBU→PAAU | 复位PAAU |
| 171 | PAAU心跳 | PAAU→BBU | 心跳保活 |
| 181 | BBU心跳 | BBU→PAAU | 心跳保活 |
| 195 | NR小区配置 | BBU→PAAU | 配置NR小区参数 |
| 196 | 小区配置应答 | PAAU→BBU | 配置结果反馈 |

**核心文件**：
- `src/cpri_protocol.c`
- `include/cpri_protocol.h`

#### 2.3.2 FPGA协议模块

**功能描述**：
- 实现FPGA消息的编解码
- 支持12种控制消息
- 大端字节序处理
- 特殊频率编解码算法

**消息格式**：
```
┌────────────────────────────────────────┐
│ 帧头 (2B): 0xEB90                      │
├────────────────────────────────────────┤
│ 消息ID (1B)                            │
├────────────────────────────────────────┤
│ 长度 (1B)                              │
├────────────────────────────────────────┤
│ 载荷 (可变长度)                        │
├────────────────────────────────────────┤
│ 帧尾 (2B): 0x5555                      │
└────────────────────────────────────────┘
```

**支持的消息类型**：

| 消息ID | 消息名称 | 载荷长度 | 说明 |
|--------|----------|----------|------|
| 0x01 | 相控阵工作模式 | 4字节 | 0=业务,1=频谱,2=待机,3=自校准 |
| 0x02 | CPRI工作模式 | 4字节 | 1=普通,2=级联,3=主备,4=反向分担 |
| 0x03 | 指令波束数 | 1字节 | 信令波束个数 |
| 0x04 | 业务波束数 | 1字节 | 业务波束个数 |
| 0x05 | 相控阵复位 | 0字节 | 复位相控阵 |
| 0x06 | 校准指示 | 1字节 | 启动校准 |
| 0x07 | 环回控制 | 1字节 | 环回测试控制 |
| 0x08 | 频率与带宽 | 8字节 | 本振频率和带宽配置 |
| 0x09 | 波束次数 | 4字节 | 波束切换次数 |
| 0x0A | 系统时间 | 8字节 | 同步系统时间 |
| 0x0B | 发射控制 | 1字节 | 发射开关控制 |
| 0xFF | 状态检询 | 0字节 | 查询FPGA状态 |

**FPGA状态响应帧（133字节）**：
```c
typedef struct {
    uint16_t frame_header;              // 0xEB90
    uint8_t  msg_id;                    // 0xFF
    uint8_t  length;                    // 129
    fpga_status_data_t data;            // 127字节状态数据
    uint16_t frame_tail;                // 0x5555
} fpga_status_frame_t;
```

**核心文件**：
- `src/fpga_protocol.c`
- `include/fpga_protocol.h`
- `src/rs422_protocol.c`
- `include/rs422_protocol.h`

### 2.4 业务功能模块

#### 2.4.1 通道建立模块

**功能描述**：
- 实现PAAU与BBU之间的通道建立流程
- 周期性发送通道建立请求（5秒间隔）
- 处理BBU下发的配置参数
- 自动同步系统时间到FPGA

**工作流程**：
```
1. TCP连接建立
   ↓
2. PAAU发送通道建立请求 (MsgID: 1)
   - 包含产品标识、能力信息、版本信息
   ↓
3. 每5秒重发请求（直到收到配置）
   ↓
4. BBU下发通道建立配置 (MsgID: 2)
   - 系统时间、CPU统计周期、工作模式
   ↓
5. PAAU处理配置
   - 设置系统时间
   - 配置FPGA工作模式
   ↓
6. PAAU发送通道建立应答 (MsgID: 3)
   ↓
7. 通道建立完成
```

**核心文件**：
- `src/channel_setup.c`
- `include/channel_setup.h`

#### 2.4.2 心跳管理模块

**功能描述**：
- 周期性发送心跳消息保持连接
- 检测BBU心跳超时
- 心跳间隔可配置（默认3秒）

**核心文件**：
- `src/heartbeat.c`
- `include/heartbeat.h`

#### 2.4.3 状态查询模块

**功能描述**：
- 响应BBU的状态查询请求
- 返回PAAU当前运行状态
- 包含温度、功率、波束等信息

**核心文件**：
- `src/status_query.c`
- `include/status_query.h`

#### 2.4.4 参数配置模块

**功能描述**：
- 处理BBU下发的参数配置
- 转发配置到FPGA
- 返回配置结果

**核心文件**：
- `src/param_config.c`
- `include/param_config.h`
- `src/param_query.c`
- `include/param_query.h`

#### 2.4.5 版本管理模块

**功能描述**：
- 管理软件版本升级
- 支持FTP下载版本包
- 版本切换和激活
- SHA-256校验和验证

**版本管理流程**：
```
1. BBU发送版本下载请求 (MsgID: 21)
   - FTP路径、文件名、版本号、文件大小
   ↓
2. PAAU从FTP下载tar.gz包
   ↓
3. 解压到 /opt/vendor/versions/<version>/
   ↓
4. 读取checksum.txt
   ↓
5. 计算antenna_mgmt.bin的SHA-256
   ↓
6. 比对校验和
   ├─ 匹配 → 保存元数据，返回成功
   └─ 不匹配 → 删除文件，返回失败
   ↓
7. BBU发送版本激活指示 (MsgID: 31)
   ↓
8. PAAU切换符号链接
   /opt/vendor/current → /opt/vendor/versions/<version>
   ↓
9. 发送激活应答
   ↓
10. 自动重启服务
```

**核心文件**：
- `src/version_manager.c`
- `include/version_manager.h`

#### 2.4.6 告警管理模块

**功能描述**：
- 自动检测硬件故障
- 主动上报告警到BBU
- 告警清除机制
- 支持5种告警类型

**告警类型**：

| 告警码 | 告警名称 | 触发条件 | 清除条件 |
|--------|----------|----------|----------|
| 50001 | 本振失锁告警 | lo_lock_status != 0 | lo_lock_status == 0 |
| 1097 | 光链路同步码流丢失 | link_success_flag位为0 | link_success_flag位为1 |
| 1156 | 过温告警 | 温度超过门限 | 温度恢复正常 |
| 1158 | 版本激活失败 | 激活流程失败 | - |
| 1159 | 通道故障过多 | channel_fault_count > 10 | channel_fault_count <= 10 |

**核心文件**：
- `src/alarm_manager.c`
- `include/alarm_manager.h`
- `src/alarm_query.c`
- `include/alarm_query.h`

#### 2.4.7 日志上传模块

**功能描述**：
- 响应BBU的日志上传请求
- 打包日志文件
- 通过FTP上传到BBU

**核心文件**：
- `src/log_upload.c`
- `include/log_upload.h`

#### 2.4.8 FPGA固件上注模块

**功能描述**：
- 通过RS-422协议上注FPGA固件
- 状态机驱动的上注流程
- 支持分段传输和重传
- CRC16-CCITT-FALSE校验

**上注流程**：
```
1. 解析固件元数据
   ↓
2. 发送传输开始命令
   - 文件类型、大小、CRC16
   ↓
3. 等待FPGA准备就绪（轮询，超时5分钟）
   ↓
4. 分段传输文件数据
   - 首段: 1000字节
   - 其他段: 1002字节
   - 每段最多重试3次
   ↓
5. 发送传输结束命令
   ↓
6. FPGA校验CRC16
   ├─ 通过 → 返回0x00
   └─ 失败 → 返回0x11
   ↓
7. 发送重构启动命令
   ↓
8. 轮询重构状态（间隔5秒，超时10分钟）
   ├─ 0x00: 重构成功
   ├─ 0x11: 重构中
   ├─ 0x22: 未开始
   └─ 0xFF: 重构失败
   ↓
9. 上注完成
```

**核心文件**：
- `src/fpga_firmware_injector.c`
- `include/fpga_firmware_injector.h`
- `src/firmware_package.c`
- `include/firmware_package.h`

### 2.5 支撑模块

#### 2.5.1 配置管理模块

**功能描述**：
- 解析配置文件
- 运行时配置加载
- 参数验证

**配置文件示例**：
```ini
# BBU服务器配置
BBU_IP=10.10.10.6
BBU_PORT=30000

# PAAU客户端配置
PAAU_IP=10.10.10.8
PAAU_ID=0

# UART配置
UART_DEVICE=/dev/ttyAMA1

# 重连配置
RECONNECT_INTERVAL_MS=3000

# 心跳配置
HEARTBEAT_INTERVAL_MS=3000

# 日志配置
LOG_LEVEL=INFO
LOG_FILE=./logs/antenna_mgmt.log
```

**核心文件**：
- `src/config.c`
- `include/config.h`

#### 2.5.2 日志系统模块

**功能描述**：
- 多级别日志：DEBUG/INFO/WARN/ERROR
- 同时输出到控制台和文件
- 线程安全
- 带时间戳和源码位置

**日志格式**：
```
[2026-03-18 10:30:45.123] [INFO] [main.c:123] TCP connected to BBU
[2026-03-18 10:30:46.456] [WARN] [alarm_manager.c:89] Alarm raised: 本振失锁
[2026-03-18 10:30:47.789] [ERROR] [tcp_client.c:234] Connection lost
```

**核心文件**：
- `src/logger.c`
- `include/logger.h`

#### 2.5.3 消息分发模块

**功能描述**：
- CPRI消息路由和分发
- 消息处理器注册机制
- 统一的消息处理接口

**核心文件**：
- `src/msg_handler.c`
- `include/msg_handler.h`

#### 2.5.4 状态管理模块

**功能描述**：
- FPGA状态缓存
- 状态到CPRI消息转换
- 线程安全的状态访问

**核心文件**：
- `src/fpga_handler.c`
- `include/fpga_handler.h`
- `src/fpga_to_cpri.c`
- `include/fpga_to_cpri.h`

---

## 3. 技术架构

### 3.1 系统架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                         BBU (基带单元)                           │
│                      10.10.10.6:30000                           │
└────────────────────────┬────────────────────────────────────────┘
                         │ TCP/IP
                         │ CPRI协议 (小端序)
                         │
┌────────────────────────▼────────────────────────────────────────┐
│                  S波段天线管理系统 (PAAU)                        │
│                      10.10.10.8                                 │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                     主程序 (main.c)                       │  │
│  │  - 系统初始化                                             │  │
│  │  - 主事件循环                                             │  │
│  │  - 资源管理                                               │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌─────────────────────┐         ┌─────────────────────┐       │
│  │   TCP客户端模块     │         │   UART客户端模块    │       │
│  │  ┌───────────────┐  │         │  ┌───────────────┐  │       │
│  │  │ 接收线程      │  │         │  │ 接收线程      │  │       │
│  │  │ 重连线程      │  │         │  │ 轮询线程      │  │       │
│  │  │ 发送队列      │  │         │  │ 发送队列      │  │       │
│  │  └───────────────┘  │         │  └───────────────┘  │       │
│  └──────────┬──────────┘         └──────────┬──────────┘       │
│             │                               │                   │
│  ┌──────────▼──────────┐         ┌─────────▼──────────┐        │
│  │  CPRI协议编解码     │         │  FPGA协议编解码    │        │
│  │  - 消息编码         │         │  - 消息编码        │        │
│  │  - 消息解码         │         │  - 消息解码        │        │
│  │  - IE处理           │         │  - 频率编解码      │        │
│  │  - 小端序转换       │         │  - 大端序转换      │        │
│  └──────────┬──────────┘         └─────────┬──────────┘        │
│             │                               │                   │
│  ┌──────────▼───────────────────────────────▼──────────┐       │
│  │              消息处理与业务逻辑层                    │       │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐    │       │
│  │  │ 通道建立   │  │ 心跳管理   │  │ 状态查询   │    │       │
│  │  └────────────┘  └────────────┘  └────────────┘    │       │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐    │       │
│  │  │ 参数配置   │  │ 版本管理   │  │ 告警管理   │    │       │
│  │  └────────────┘  └────────────┘  └────────────┘    │       │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐    │       │
│  │  │ 日志上传   │  │ 固件上注   │  │ 环回测试   │    │       │
│  │  └────────────┘  └────────────┘  └────────────┘    │       │
│  └──────────────────────────────────────────────────────┘       │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                     支撑服务层                            │  │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐         │  │
│  │  │ 配置管理   │  │ 日志系统   │  │ 状态管理   │         │  │
│  │  └────────────┘  └────────────┘  └────────────┘         │  │
│  └──────────────────────────────────────────────────────────┘  │
└────────────────────────┬────────────────────────────────────────┘
                         │ UART RS-422
                         │ 921600-8N1 (大端序)
                         │
┌────────────────────────▼────────────────────────────────────────┐
│                         FPGA                                     │
│                      /dev/ttyAMA1                               │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 数据流向

#### 3.2.1 BBU → PAAU → FPGA 数据流

```
BBU发送CPRI消息
    ↓
TCP接收线程接收数据
    ↓
CPRI协议解码（小端序 → 主机序）
    ↓
消息分发器路由到对应处理器
    ↓
业务逻辑处理
    ↓
转换为FPGA消息
    ↓
FPGA协议编码（主机序 → 大端序）
    ↓
UART发送到FPGA
```

#### 3.2.2 FPGA → PAAU → BBU 数据流

```
FPGA发送状态帧（133字节）
    ↓
UART接收线程接收数据
    ↓
FPGA协议解码（大端序 → 主机序）
    ↓
状态管理器缓存状态
    ↓
告警检测（温度、本振、时钟等）
    ↓
转换为CPRI消息
    ↓
CPRI协议编码（主机序 → 小端序）
    ↓
TCP发送到BBU
```

### 3.3 线程模型

系统采用多线程架构，主要线程包括：

| 线程名称 | 功能 | 优先级 | 备注 |
|---------|------|--------|------|
| 主线程 | 系统初始化、事件循环 | 普通 | 主控线程 |
| TCP接收线程 | 接收BBU消息 | 高 | 阻塞式接收 |
| TCP重连线程 | 断线自动重连 | 普通 | 定时检测 |
| UART接收线程 | 接收FPGA消息 | 高 | 阻塞式接收 |
| UART轮询线程 | 定时查询FPGA状态 | 普通 | 1秒间隔 |
| 固件上注线程 | FPGA固件上注 | 普通 | 按需启动 |

**线程同步机制**：
- 互斥锁（pthread_mutex_t）：保护共享资源
- 条件变量（pthread_cond_t）：线程间通信
- 原子操作：标志位访问

### 3.4 目录结构

```
s_band_antenna_mgmt/
├── src/                          # 源代码目录
│   ├── main.c                    # 主程序入口
│   ├── tcp_client.c              # TCP客户端实现
│   ├── uart_client.c             # UART客户端实现
│   ├── uart_rs422_client.c       # RS-422客户端实现
│   ├── cpri_protocol.c           # CPRI协议编解码
│   ├── fpga_protocol.c           # FPGA协议编解码
│   ├── rs422_protocol.c          # RS-422协议编解码
│   ├── msg_handler.c             # CPRI消息处理器
│   ├── fpga_handler.c            # FPGA消息处理器
│   ├── fpga_to_cpri.c            # FPGA状态到CPRI转换
│   ├── channel_setup.c           # 通道建立模块
│   ├── heartbeat.c               # 心跳管理模块
│   ├── status_query.c            # 状态查询模块
│   ├── param_config.c            # 参数配置模块
│   ├── param_query.c             # 参数查询模块
│   ├── version_manager.c         # 版本管理模块
│   ├── alarm_manager.c           # 告警管理模块
│   ├── alarm_query.c             # 告警查询模块
│   ├── log_upload.c              # 日志上传模块
│   ├── fpga_firmware_injector.c  # FPGA固件上注模块
│   ├── firmware_package.c        # 固件包管理模块
│   ├── cell_config.c             # 小区配置模块
│   ├── loopback.c                # 环回测试模块
│   ├── reset.c                   # 复位模块
│   ├── transparent_msg.c         # 透传消息模块
│   ├── config.c                  # 配置管理模块
│   └── logger.c                  # 日志系统模块
│
├── include/                      # 头文件目录
│   ├── common.h                  # 通用定义
│   ├── tcp_client.h              # TCP客户端接口
│   ├── uart_client.h             # UART客户端接口
│   ├── uart_rs422_client.h       # RS-422客户端接口
│   ├── cpri_protocol.h           # CPRI协议定义
│   ├── fpga_protocol.h           # FPGA协议定义
│   ├── rs422_protocol.h          # RS-422协议定义
│   ├── msg_handler.h             # CPRI消息处理接口
│   ├── fpga_handler.h            # FPGA处理接口
│   ├── fpga_to_cpri.h            # 状态转换接口
│   ├── channel_setup.h           # 通道建立接口
│   ├── heartbeat.h               # 心跳管理接口
│   ├── status_query.h            # 状态查询接口
│   ├── param_config.h            # 参数配置接口
│   ├── param_query.h             # 参数查询接口
│   ├── version_manager.h         # 版本管理接口
│   ├── alarm_manager.h           # 告警管理接口
│   ├── alarm_query.h             # 告警查询接口
│   ├── log_upload.h              # 日志上传接口
│   ├── fpga_firmware_injector.h  # FPGA固件上注接口
│   ├── firmware_package.h        # 固件包管理接口
│   ├── cell_config.h             # 小区配置接口
│   ├── loopback.h                # 环回测试接口
│   ├── reset.h                   # 复位接口
│   ├── transparent_msg.h         # 透传消息接口
│   ├── config.h                  # 配置管理接口
│   └── logger.h                  # 日志接口
│
├── config/                       # 配置文件目录
│   └── antenna_mgmt.conf         # 主配置文件
│
├── scripts/                      # 脚本工具目录
│   ├── create_version_package.sh # 创建版本包脚本
│   ├── verify_package.sh         # 验证版本包脚本
│   ├── antenna-mgmt.service      # systemd服务文件
│   ├── antenna-mgmt-launcher.sh  # 服务启动脚本
│   └── restart-after-activate.sh # 激活后重启脚本
│
├── tests/                        # 测试代码目录
│   ├── unit/                     # 单元测试
│   │   ├── test_rs422_protocol.cpp
│   │   ├── test_firmware_package.cpp
│   │   └── test_fpga_firmware_injector.cpp
│   └── CMakeLists.txt            # 测试CMake配置
│
├── build/                        # 编译输出目录
│   ├── release/                  # 生产环境构建
│   └── coverage/                 # 测试覆盖率构建
│
├── logs/                         # 日志文件目录
│   └── antenna_mgmt.log          # 运行日志
│
├── docs/                         # 文档目录
│   └── README.md                 # 文档索引
│
├── Makefile                      # 生产环境构建脚本
├── Makefile.test                 # 测试构建脚本
├── CMakeLists.txt                # CMake配置文件
├── README.md                     # 项目说明文档
├── BUILD.md                      # 构建说明文档
├── DEPLOY_TO_D2000.md            # D2000部署指南
└── PROJECT_STRUCTURE.md          # 项目结构文档
```

### 3.5 编译构建

#### 3.5.1 生产环境构建

```bash
# 编译
make

# 清理
make clean

# 安装
make install

# 运行
./antenna_mgmt
```

**输出文件**：
- `build/release/bin/antenna_mgmt` - 可执行程序
- `build/release/obj/*.o` - 目标文件

#### 3.5.2 测试环境构建

```bash
# 方式1: 使用Makefile
make -f Makefile.test test
make -f Makefile.test coverage

# 方式2: 使用CMake
mkdir -p build/coverage
cd build/coverage
cmake -DCMAKE_BUILD_TYPE=Coverage ../..
make
ctest --output-on-failure
make coverage
```

**输出文件**：
- `build/coverage/bin/test_*` - 测试可执行程序
- `reports/html/index.html` - 覆盖率报告

### 3.6 依赖关系

#### 3.6.1 系统依赖

| 依赖项 | 版本要求 | 用途 |
|--------|----------|------|
| GCC | 7.5+ | C编译器 |
| Make | 3.81+ | 构建工具 |
| pthread | POSIX | 多线程支持 |
| libc | glibc 2.27+ | 标准C库 |

#### 3.6.2 测试依赖

| 依赖项 | 版本要求 | 用途 |
|--------|----------|------|
| Google Test | 1.8.0+ | 单元测试框架 |
| lcov | 1.13+ | 覆盖率报告生成 |
| gcov | 7.5+ | 覆盖率数据收集 |
| CMake | 3.10+ | 测试构建工具 |

---

## 4. 核心技术实现

### 4.1 CPRI协议实现

#### 4.1.1 消息编码

**函数原型**：
```c
int cpri_encode_message(const cpri_message_t *msg, uint8_t *buffer, uint32_t buffer_size);
```

**编码流程**：
```c
// 1. 编码消息头（15字节，小端序）
uint32_t msg_id_le = htole32(msg->header.msg_id);
uint32_t msg_length_le = htole32(msg->header.msg_length);
uint32_t serial_num_le = htole32(msg->header.serial_num);

memcpy(buffer + 0, &msg_id_le, 4);
memcpy(buffer + 4, &msg_length_le, 4);
buffer[8] = msg->header.paau_id;
buffer[9] = msg->header.bbu_id;
buffer[10] = msg->header.port_num;
memcpy(buffer + 11, &serial_num_le, 4);

// 2. 编码载荷（IE结构）
if (msg->payload && msg->payload_len > 0) {
    memcpy(buffer + CPRI_HEADER_LEN, msg->payload, msg->payload_len);
}

return CPRI_HEADER_LEN + msg->payload_len;
```

#### 4.1.2 消息解码

**函数原型**：
```c
int cpri_decode_message(const uint8_t *buffer, uint32_t buffer_len, cpri_message_t *msg);
```

**解码流程**：
```c
// 1. 解码消息头
memcpy(&msg->header.msg_id, buffer + 0, 4);
msg->header.msg_id = le32toh(msg->header.msg_id);

memcpy(&msg->header.msg_length, buffer + 4, 4);
msg->header.msg_length = le32toh(msg->header.msg_length);

msg->header.paau_id = buffer[8];
msg->header.bbu_id = buffer[9];
msg->header.port_num = buffer[10];

memcpy(&msg->header.serial_num, buffer + 11, 4);
msg->header.serial_num = le32toh(msg->header.serial_num);

// 2. 解码载荷
msg->payload_len = msg->header.msg_length - CPRI_HEADER_LEN;
if (msg->payload_len > 0) {
    msg->payload = malloc(msg->payload_len);
    memcpy(msg->payload, buffer + CPRI_HEADER_LEN, msg->payload_len);
}
```

#### 4.1.3 IE编码示例

```c
// 编码IE 1001（告警上报）
uint8_t ie_buffer[138];
uint16_t ie_type = htole16(1001);
uint16_t ie_length = htole16(138);

memcpy(ie_buffer + 0, &ie_type, 2);
memcpy(ie_buffer + 2, &ie_length, 2);

// IE数据（134字节）
uint16_t validity = htole16(0);
uint32_t alarm_code = htole32(50001);
uint32_t sub_code = htole32(0);
uint32_t clear_flag = htole32(0);

memcpy(ie_buffer + 4, &validity, 2);
memcpy(ie_buffer + 6, &alarm_code, 4);
memcpy(ie_buffer + 10, &sub_code, 4);
memcpy(ie_buffer + 14, &clear_flag, 4);

// 时间戳（20字节）
char timestamp[20];
time_t now = time(NULL);
struct tm *tm_info = localtime(&now);
strftime(timestamp, 20, "%Y-%m-%d %H:%M:%S", tm_info);
memcpy(ie_buffer + 18, timestamp, 20);

// 附加信息（100字节）
const char *info = "LO unlock detected";
memcpy(ie_buffer + 38, info, strlen(info));
```

### 4.2 FPGA协议实现

#### 4.2.1 消息编码

**函数原型**：
```c
int fpga_encode_message(const fpga_message_t *msg, uint8_t *buffer, uint32_t buffer_size);
```

**编码流程**：
```c
// 1. 帧头（大端序）
uint16_t header_be = htons(FPGA_FRAME_HEADER);  // 0xEB90
memcpy(buffer, &header_be, 2);

// 2. 消息ID
buffer[2] = msg->msg_id;

// 3. 长度字段
buffer[3] = msg->payload_len;

// 4. 载荷
if (msg->payload && msg->payload_len > 0) {
    memcpy(buffer + 4, msg->payload, msg->payload_len);
}

// 5. 帧尾（大端序）
uint16_t tail_be = htons(FPGA_FRAME_TAIL);  // 0x5555
memcpy(buffer + 4 + msg->payload_len, &tail_be, 2);

return 6 + msg->payload_len;
```

#### 4.2.2 频率编解码算法

FPGA使用特殊的频率压缩算法：

**编码算法**：
```c
uint32_t fpga_encode_frequency(uint32_t freq_khz)
{
    // freq_khz: 物理频率（单位: kHz）
    // 返回: 编码值

    uint32_t quotient = freq_khz / 50000;      // 商
    uint32_t remainder = freq_khz % 50000;     // 余数
    uint32_t encoded = (quotient << 17) | (remainder * 2);

    return encoded;
}
```

**解码算法**：
```c
uint32_t fpga_decode_frequency(uint32_t encoded_freq)
{
    // encoded_freq: 编码值
    // 返回: 物理频率（单位: kHz）

    uint32_t quotient = (encoded_freq >> 17) & 0x7FFF;
    uint32_t remainder_x2 = encoded_freq & 0x1FFFF;
    uint32_t remainder = remainder_x2 / 2;
    uint32_t freq_khz = quotient * 50000 + remainder;

    return freq_khz;
}
```

**示例**：
```c
// 编码: 2100000 kHz (2.1 GHz)
uint32_t encoded = fpga_encode_frequency(2100000);
// encoded = (42 << 17) | (0 * 2) = 0x00540000

// 解码
uint32_t freq = fpga_decode_frequency(0x00540000);
// freq = 42 * 50000 + 0 = 2100000 kHz
```

#### 4.2.3 状态帧解析

**函数原型**：
```c
int fpga_decode_status_frame(const uint8_t *buffer, uint32_t buffer_len,
                              fpga_status_frame_t *status);
```

**解析流程**：
```c
// 1. 验证帧长度
if (buffer_len != 133) {
    return ERROR_INVALID_PARAM;
}

// 2. 验证帧头
uint16_t header = ntohs(*(uint16_t*)buffer);
if (header != FPGA_FRAME_HEADER) {
    return ERROR_INVALID_PARAM;
}

// 3. 验证消息ID
if (buffer[2] != FPGA_MSG_STATUS_QUERY) {
    return ERROR_INVALID_PARAM;
}

// 4. 验证帧尾
uint16_t tail = ntohs(*(uint16_t*)(buffer + 131));
if (tail != FPGA_FRAME_TAIL) {
    return ERROR_INVALID_PARAM;
}

// 5. 解析状态数据（127字节，大端序）
status->frame_header = header;
status->msg_id = buffer[2];
status->length = buffer[3];

// 解析各字段（注意大端序转换）
status->data.link_success_flag = buffer[4];
status->data.main_fiber_num = buffer[5];
status->data.backup_fiber_num = buffer[6];
status->data.channel_setup_reason = buffer[7];

status->data.nr_beam_count = ntohl(*(uint32_t*)(buffer + 8));
status->data.max_tx_power = ntohs(*(uint16_t*)(buffer + 12));
status->data.supported_modes = ntohs(*(uint16_t*)(buffer + 14));

// ... 解析其他字段

status->frame_tail = tail;
```

### 4.3 RS-422固件上注协议实现

#### 4.3.1 帧结构

RS-422帧遵循QNIXY32110-2023规范：

```
+----------------+----------------+------------------+------------------+
|   标识符(2B)   |   包标识(2B)   | 包序列控制(2B)   | 数据域长度(2B)   |
+----------------+----------------+------------------+------------------+
|                          数据域(可变长度)                            |
+----------------------------------------------------------------------+
|                          校验和(2B)                                  |
+----------------------------------------------------------------------+
```

#### 4.3.2 校验和算法

**单字节累加求和取反**：
```c
uint16_t rs422_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;

    // 单字节累加求和
    for (uint32_t i = 0; i < len; i++) {
        sum += data[i];
    }

    // 取反，取低16位
    return (uint16_t)(~sum & 0xFFFF);
}
```

**注意**：
- 校验和计算不包含标识符（0xEB90）
- 校验和计算包含包标识、包序列控制、数据域长度、命令码和载荷
- 校验和本身不参与计算

#### 4.3.3 CRC16-CCITT-FALSE算法

用于文件完整性校验：

```c
uint16_t rs422_crc16_ccitt_false(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;  // 初始值
    const uint16_t poly = 0x1021;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ poly;
            } else {
                crc = crc << 1;
            }
        }
    }

    return crc;
}
```

**参数**：
- 初始值: 0xFFFF
- 多项式: 0x1021
- 输入反转: 否
- 输出反转: 否
- 异或输出: 否

#### 4.3.4 传输开始命令构造

```c
int rs422_build_transfer_start(uint8_t *frame_buf, uint32_t buf_size,
                                uint32_t file_size, uint16_t file_crc16,
                                uint32_t total_segments, uint32_t last_seg_len)
{
    uint32_t offset = 0;

    // 1. 标识符 (0xEB90, Big-Endian)
    uint16_t sync = htons(0xEB90);
    memcpy(frame_buf + offset, &sync, 2);
    offset += 2;

    // 2. 包标识 (控制帧APID: 0x03A0)
    uint16_t packet_id = (0b000 << 13) |  // 版本号=0
                         (0b0 << 12) |     // 类型=0
                         (0b0 << 11) |     // 副导头=0
                         (0x03A0 & 0x07FF); // APID
    uint16_t packet_id_be = htons(packet_id);
    memcpy(frame_buf + offset, &packet_id_be, 2);
    offset += 2;

    // 3. 包序列控制 (单帧)
    static uint16_t seq_counter = 0;
    uint16_t seq_ctrl = (0b11 << 14) | (seq_counter & 0x3FFF);
    seq_counter++;
    uint16_t seq_ctrl_be = htons(seq_ctrl);
    memcpy(frame_buf + offset, &seq_ctrl_be, 2);
    offset += 2;

    // 4. 数据域长度 (命令码2B + 载荷15B - 1 = 16)
    uint16_t data_len = htons(16);
    memcpy(frame_buf + offset, &data_len, 2);
    offset += 2;

    // 5. 命令码 (0x0155)
    uint16_t cmd_code = htons(0x0155);
    memcpy(frame_buf + offset, &cmd_code, 2);
    offset += 2;

    // 6. 载荷 (15字节)
    uint8_t device_id = (0x03A0 >> 4) & 0x7F;
    frame_buf[offset++] = device_id;
    frame_buf[offset++] = 0xFF;  // file_type
    frame_buf[offset++] = 0x00;  // file_sub_type

    uint16_t segment_info = htons((3 << 14) | total_segments);
    memcpy(frame_buf + offset, &segment_info, 2);
    offset += 2;

    uint32_t file_length_be = htonl(file_size);
    memcpy(frame_buf + offset, &file_length_be, 4);
    offset += 4;

    uint32_t last_seg_len_be = htonl(last_seg_len);
    memcpy(frame_buf + offset, &last_seg_len_be, 4);
    offset += 4;

    uint16_t file_checksum_be = htons(file_crc16);
    memcpy(frame_buf + offset, &file_checksum_be, 2);
    offset += 2;

    // 7. 校验和 (从字节偏移2开始)
    uint16_t checksum = rs422_checksum(frame_buf + 2, offset - 2);
    uint16_t checksum_be = htons(checksum);
    memcpy(frame_buf + offset, &checksum_be, 2);
    offset += 2;

    return offset;
}
```

#### 4.3.5 文件数据帧构造

```c
int rs422_build_file_data(uint8_t *frame_buf, uint32_t buf_size,
                           uint16_t segment_num, const uint8_t *data, uint32_t data_len,
                           uint32_t total_segments)
{
    uint32_t offset = 0;

    // 1. 标识符
    uint16_t sync = htons(0xEB90);
    memcpy(frame_buf + offset, &sync, 2);
    offset += 2;

    // 2. 包标识 (数据帧APID: 0x03AF)
    uint16_t packet_id = (0b000 << 13) |
                         (0b0 << 12) |
                         (0b0 << 11) |
                         (0x03AF & 0x07FF);
    uint16_t packet_id_be = htons(packet_id);
    memcpy(frame_buf + offset, &packet_id_be, 2);
    offset += 2;

    // 3. 包序列控制 (根据段号设置分组标志)
    static uint16_t seq_counter = 0;
    uint16_t group_flags;
    if (total_segments == 1) {
        group_flags = 0b11;  // 单帧
    } else if (segment_num == 0) {
        group_flags = 0b01;  // 首段
    } else if (segment_num == total_segments - 1) {
        group_flags = 0b10;  // 尾段
    } else {
        group_flags = 0b00;  // 中间段
    }

    uint16_t seq_ctrl = (group_flags << 14) | (seq_counter & 0x3FFF);
    seq_counter++;
    uint16_t seq_ctrl_be = htons(seq_ctrl);
    memcpy(frame_buf + offset, &seq_ctrl_be, 2);
    offset += 2;

    // 4. 数据域长度 (命令码2B + 段号2B + 数据 - 1)
    uint16_t data_len_field = htons(2 + 2 + data_len - 1);
    memcpy(frame_buf + offset, &data_len_field, 2);
    offset += 2;

    // 5. 命令码 (0x0180)
    uint16_t cmd_code = htons(0x0180);
    memcpy(frame_buf + offset, &cmd_code, 2);
    offset += 2;

    // 6. 段号
    uint16_t segment_num_be = htons(segment_num);
    memcpy(frame_buf + offset, &segment_num_be, 2);
    offset += 2;

    // 7. 文件数据
    memcpy(frame_buf + offset, data, data_len);
    offset += data_len;

    // 8. 校验和
    uint16_t checksum = rs422_checksum(frame_buf + 2, offset - 2);
    uint16_t checksum_be = htons(checksum);
    memcpy(frame_buf + offset, &checksum_be, 2);
    offset += 2;

    return offset;
}
```

### 4.4 状态机实现

#### 4.4.1 固件上注状态机

```c
typedef enum {
    FPGA_INJ_STATE_IDLE = 0,                /* 空闲状态 */
    FPGA_INJ_STATE_PARSING_METADATA,        /* 解析元数据 */
    FPGA_INJ_STATE_TRANSFER_START,          /* 传输开始 */
    FPGA_INJ_STATE_TRANSFER_DATA,           /* 传输数据 */
    FPGA_INJ_STATE_TRANSFER_END,            /* 传输结束 */
    FPGA_INJ_STATE_RECONFIG_START,          /* 重构开始 */
    FPGA_INJ_STATE_RECONFIG_POLLING,        /* 重构轮询 */
    FPGA_INJ_STATE_COMPLETED,               /* 完成 */
    FPGA_INJ_STATE_FAILED                   /* 失败 */
} fpga_injection_state_t;
```

**状态转换图**：
```
IDLE
  ↓
PARSING_METADATA
  ↓
TRANSFER_START
  ↓ (FPGA返回0x00)
TRANSFER_DATA
  ↓ (所有段传输完成)
TRANSFER_END
  ↓ (FPGA CRC校验通过)
RECONFIG_START
  ↓
RECONFIG_POLLING
  ↓ (FPGA返回0x00)
COMPLETED

任何阶段出错 → FAILED
```

**状态处理函数**：
```c
static int handle_state_transfer_start(fpga_injection_task_t *task)
{
    // 发送传输开始命令
    int ret = send_transfer_start_command(task);
    if (ret != SUCCESS) {
        return ret;
    }

    // 轮询FPGA准备状态
    time_t start_time = time(NULL);
    while (1) {
        uint8_t ack_result;
        ret = receive_transfer_start_ack(&ack_result);

        if (ret == SUCCESS) {
            if (ack_result == 0x00) {
                // 准备就绪，进入数据传输状态
                task->state = FPGA_INJ_STATE_TRANSFER_DATA;
                return SUCCESS;
            } else if (ack_result == 0x11) {
                // 准备中，继续轮询
                sleep(1);
            } else {
                // 拒绝或异常
                return ERROR_GENERAL;
            }
        }

        // 检查超时
        if (time(NULL) - start_time > FPGA_TRANSFER_START_TIMEOUT_SEC) {
            return ERROR_TIMEOUT;
        }
    }
}
```

### 4.5 线程安全实现

#### 4.5.1 互斥锁保护

```c
typedef struct {
    fpga_status_frame_t status;
    pthread_mutex_t mutex;
    bool valid;
} fpga_status_manager_t;

static fpga_status_manager_t g_status_mgr;

// 初始化
int fpga_status_manager_init(void)
{
    pthread_mutex_init(&g_status_mgr.mutex, NULL);
    g_status_mgr.valid = false;
    return SUCCESS;
}

// 更新状态
int fpga_status_manager_update(const fpga_status_frame_t *status)
{
    pthread_mutex_lock(&g_status_mgr.mutex);
    memcpy(&g_status_mgr.status, status, sizeof(fpga_status_frame_t));
    g_status_mgr.valid = true;
    pthread_mutex_unlock(&g_status_mgr.mutex);
    return SUCCESS;
}

// 获取状态
int fpga_status_manager_get(fpga_status_frame_t *status)
{
    pthread_mutex_lock(&g_status_mgr.mutex);
    if (!g_status_mgr.valid) {
        pthread_mutex_unlock(&g_status_mgr.mutex);
        return ERROR_NOT_INITIALIZED;
    }
    memcpy(status, &g_status_mgr.status, sizeof(fpga_status_frame_t));
    pthread_mutex_unlock(&g_status_mgr.mutex);
    return SUCCESS;
}
```

#### 4.5.2 原子操作

```c
#include <stdatomic.h>

typedef struct {
    atomic_bool connected;
    atomic_bool channel_established;
    atomic_uint serial_num;
} global_state_t;

static global_state_t g_state;

// 初始化
void global_state_init(void)
{
    atomic_store(&g_state.connected, false);
    atomic_store(&g_state.channel_established, false);
    atomic_store(&g_state.serial_num, 1);
}

// 获取下一个流水号
uint32_t get_next_serial_num(void)
{
    return atomic_fetch_add(&g_state.serial_num, 1);
}

// 检查连接状态
bool is_connected(void)
{
    return atomic_load(&g_state.connected);
}
```

### 4.6 错误处理机制

#### 4.6.1 错误码定义

```c
#define SUCCESS             0
#define ERROR_GENERAL      -1
#define ERROR_INVALID_PARAM -2
#define ERROR_MEMORY       -3
#define ERROR_NETWORK      -4
#define ERROR_TIMEOUT      -5
#define ERROR_NOT_INITIALIZED -6
```

#### 4.6.2 错误处理模式

```c
int some_function(void)
{
    int ret;

    // 参数验证
    if (param == NULL) {
        LOG_ERROR("Invalid parameter");
        return ERROR_INVALID_PARAM;
    }

    // 资源分配
    void *buffer = malloc(size);
    if (buffer == NULL) {
        LOG_ERROR("Memory allocation failed");
        return ERROR_MEMORY;
    }

    // 操作执行
    ret = do_operation(buffer);
    if (ret != SUCCESS) {
        LOG_ERROR("Operation failed: %d", ret);
        free(buffer);
        return ret;
    }

    // 资源释放
    free(buffer);
    return SUCCESS;
}
```

---

## 5. API接口规范

### 5.1 TCP客户端API

#### 5.1.1 初始化和连接

```c
/**
 * @brief 初始化TCP客户端
 * @param client TCP客户端结构体指针
 * @param server_ip BBU服务器IP地址
 * @param server_port BBU服务器端口
 * @return 成功返回SUCCESS，失败返回错误码
 */
int tcp_client_init(tcp_client_t *client, const char *server_ip, uint16_t server_port);

/**
 * @brief 连接到BBU服务器
 * @param client TCP客户端结构体指针
 * @return 成功返回SUCCESS，失败返回错误码
 */
int tcp_client_connect(tcp_client_t *client);

/**
 * @brief 断开连接
 * @param client TCP客户端结构体指针
 */
void tcp_client_disconnect(tcp_client_t *client);

/**
 * @brief 清理TCP客户端资源
 * @param client TCP客户端结构体指针
 */
void tcp_client_cleanup(tcp_client_t *client);
```

#### 5.1.2 数据收发

```c
/**
 * @brief 发送数据到BBU
 * @param client TCP客户端结构体指针
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return 成功返回发送的字节数，失败返回错误码
 */
int tcp_client_send(tcp_client_t *client, const uint8_t *data, uint32_t len);

/**
 * @brief 设置数据接收回调
 * @param client TCP客户端结构体指针
 * @param callback 回调函数指针
 */
void tcp_client_set_recv_callback(tcp_client_t *client,
                                   void (*callback)(const uint8_t *data, uint32_t len));

/**
 * @brief 设置连接状态回调
 * @param client TCP客户端结构体指针
 * @param on_connected 连接成功回调
 * @param on_disconnected 连接断开回调
 */
void tcp_client_set_callbacks(tcp_client_t *client,
                               void (*on_connected)(void),
                               void (*on_disconnected)(void));
```

### 5.2 UART客户端API

#### 5.2.1 初始化和连接

```c
/**
 * @brief 初始化UART客户端
 * @param client UART客户端结构体指针
 * @param device 串口设备路径（如 /dev/ttyAMA1）
 * @param baudrate 波特率（如 921600）
 * @return 成功返回SUCCESS，失败返回错误码
 */
int uart_client_init(uart_client_t *client, const char *device, uint32_t baudrate);

/**
 * @brief 打开串口
 * @param client UART客户端结构体指针
 * @return 成功返回SUCCESS，失败返回错误码
 */
int uart_client_open(uart_client_t *client);

/**
 * @brief 关闭串口
 * @param client UART客户端结构体指针
 */
void uart_client_close(uart_client_t *client);

/**
 * @brief 清理UART客户端资源
 * @param client UART客户端结构体指针
 */
void uart_client_cleanup(uart_client_t *client);
```

#### 5.2.2 数据收发

```c
/**
 * @brief 发送数据到FPGA
 * @param client UART客户端结构体指针
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return 成功返回发送的字节数，失败返回错误码
 */
int uart_client_send(uart_client_t *client, const uint8_t *data, uint32_t len);

/**
 * @brief 接收数据（阻塞式）
 * @param client UART客户端结构体指针
 * @param buffer 接收缓冲区
 * @param buffer_size 缓冲区大小
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回接收的字节数，失败返回错误码
 */
int uart_client_recv(uart_client_t *client, uint8_t *buffer,
                     uint32_t buffer_size, uint32_t timeout_ms);

/**
 * @brief 设置数据接收回调
 * @param client UART客户端结构体指针
 * @param callback 回调函数指针
 */
void uart_client_set_recv_callback(uart_client_t *client,
                                    void (*callback)(const uint8_t *data, uint32_t len));
```

### 5.3 CPRI协议API

#### 5.3.1 消息编解码

```c
/**
 * @brief 编码CPRI消息
 * @param msg CPRI消息结构体指针
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return 成功返回编码后的字节数，失败返回错误码
 */
int cpri_encode_message(const cpri_message_t *msg, uint8_t *buffer, uint32_t buffer_size);

/**
 * @brief 解码CPRI消息
 * @param buffer 输入缓冲区
 * @param buffer_len 缓冲区长度
 * @param msg 输出CPRI消息结构体指针
 * @return 成功返回SUCCESS，失败返回错误码
 */
int cpri_decode_message(const uint8_t *buffer, uint32_t buffer_len, cpri_message_t *msg);

/**
 * @brief 释放CPRI消息资源
 * @param msg CPRI消息结构体指针
 */
void cpri_free_message(cpri_message_t *msg);

/**
 * @brief 获取消息类型名称
 * @param msg_id 消息ID
 * @return 消息类型名称字符串
 */
const char* cpri_get_msg_type_name(uint32_t msg_id);
```

### 5.4 FPGA协议API

#### 5.4.1 消息编解码

```c
/**
 * @brief 编码FPGA消息
 * @param msg FPGA消息结构体指针
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return 成功返回编码后的字节数，失败返回错误码
 */
int fpga_encode_message(const fpga_message_t *msg, uint8_t *buffer, uint32_t buffer_size);

/**
 * @brief 解码FPGA消息
 * @param buffer 输入缓冲区
 * @param buffer_len 缓冲区长度
 * @param msg 输出FPGA消息结构体指针
 * @return 成功返回SUCCESS，失败返回错误码
 */
int fpga_decode_message(const uint8_t *buffer, uint32_t buffer_len, fpga_message_t *msg);

/**
 * @brief 解码FPGA状态帧
 * @param buffer 输入缓冲区（133字节）
 * @param buffer_len 缓冲区长度
 * @param status 输出状态帧结构体指针
 * @return 成功返回SUCCESS，失败返回错误码
 */
int fpga_decode_status_frame(const uint8_t *buffer, uint32_t buffer_len,
                              fpga_status_frame_t *status);

/**
 * @brief 释放FPGA消息资源
 * @param msg FPGA消息结构体指针
 */
void fpga_free_message(fpga_message_t *msg);
```

#### 5.4.2 频率编解码

```c
/**
 * @brief 编码频率值
 * @param freq_khz 物理频率（单位: kHz）
 * @return 编码后的频率值
 */
uint32_t fpga_encode_frequency(uint32_t freq_khz);

/**
 * @brief 解码频率值
 * @param encoded_freq 编码的频率值
 * @return 物理频率（单位: kHz）
 */
uint32_t fpga_decode_frequency(uint32_t encoded_freq);
```

### 5.5 RS-422协议API

#### 5.5.1 帧编解码

```c
/**
 * @brief 编码RS-422帧
 * @param apid 应用标识符
 * @param group_flags 分组标志（0b00/0b01/0b10/0b11）
 * @param cmd_code 命令码
 * @param payload 载荷数据
 * @param payload_len 载荷长度
 * @param frame_buf 输出帧缓冲区
 * @param buf_size 缓冲区大小
 * @return 成功返回帧长度，失败返回错误码
 */
int rs422_encode_frame(uint16_t apid, uint16_t group_flags, uint16_t cmd_code,
                        const uint8_t *payload, uint32_t payload_len,
                        uint8_t *frame_buf, uint32_t buf_size);

/**
 * @brief 解码RS-422帧
 * @param frame_buf 输入帧缓冲区
 * @param frame_len 帧长度
 * @param apid 输出应用标识符
 * @param group_flags 输出分组标志
 * @param cmd_code 输出命令码
 * @param payload 输出载荷数据
 * @param payload_len 输出载荷长度
 * @return 成功返回SUCCESS，失败返回错误码
 */
int rs422_decode_frame(const uint8_t *frame_buf, uint32_t frame_len,
                        uint16_t *apid, uint16_t *group_flags, uint16_t *cmd_code,
                        uint8_t **payload, uint32_t *payload_len);
```

#### 5.5.2 校验和计算

```c
/**
 * @brief 计算RS-422校验和（单字节累加求和取反）
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return 校验和值（16位）
 */
uint16_t rs422_checksum(const uint8_t *data, uint32_t len);

/**
 * @brief 计算CRC16-CCITT-FALSE
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return CRC16值
 */
uint16_t rs422_crc16_ccitt_false(const uint8_t *data, uint32_t len);
```

#### 5.5.3 命令构造

```c
/**
 * @brief 构造传输开始命令
 * @param frame_buf 输出帧缓冲区
 * @param buf_size 缓冲区大小
 * @param file_size 文件总大小
 * @param file_crc16 文件CRC16校验和
 * @param total_segments 总段数
 * @param last_seg_len 最后一段长度
 * @return 成功返回帧长度，失败返回错误码
 */
int rs422_build_transfer_start(uint8_t *frame_buf, uint32_t buf_size,
                                uint32_t file_size, uint16_t file_crc16,
                                uint32_t total_segments, uint32_t last_seg_len);

/**
 * @brief 构造文件数据帧
 * @param frame_buf 输出帧缓冲区
 * @param buf_size 缓冲区大小
 * @param segment_num 段号（从0开始）
 * @param data 文件数据
 * @param data_len 数据长度
 * @param total_segments 总段数
 * @return 成功返回帧长度，失败返回错误码
 */
int rs422_build_file_data(uint8_t *frame_buf, uint32_t buf_size,
                           uint16_t segment_num, const uint8_t *data, uint32_t data_len,
                           uint32_t total_segments);

/**
 * @brief 构造传输结束命令
 * @param frame_buf 输出帧缓冲区
 * @param buf_size 缓冲区大小
 * @return 成功返回帧长度，失败返回错误码
 */
int rs422_build_transfer_end(uint8_t *frame_buf, uint32_t buf_size);

/**
 * @brief 构造重构启动命令
 * @param frame_buf 输出帧缓冲区
 * @param buf_size 缓冲区大小
 * @param file_type 文件类型
 * @param file_sub_type 文件子类型
 * @return 成功返回帧长度，失败返回错误码
 */
int rs422_build_reconfig_start(uint8_t *frame_buf, uint32_t buf_size,
                                 uint8_t file_type, uint8_t file_sub_type);

/**
 * @brief 构造重构查询命令
 * @param frame_buf 输出帧缓冲区
 * @param buf_size 缓冲区大小
 * @return 成功返回帧长度，失败返回错误码
 */
int rs422_build_reconfig_query(uint8_t *frame_buf, uint32_t buf_size);
```

### 5.6 固件上注API

#### 5.6.1 初始化和启动

```c
/**
 * @brief 初始化FPGA固件注入器
 * @param uart_client UART RS-422客户端
 * @return 成功返回SUCCESS，失败返回错误码
 */
int fpga_firmware_injection_init(uart_rs422_client_t *uart_client);

/**
 * @brief 启动固件注入任务（后台线程）
 * @param version_dir 版本目录路径
 * @param version_num 版本号
 * @return 成功返回SUCCESS，失败返回错误码
 */
int fpga_firmware_injection_start(const char *version_dir, const char *version_num);

/**
 * @brief 获取注入进度
 * @param current 输出当前进度
 * @param total 输出总进度
 * @param state 输出当前状态
 * @return 成功返回SUCCESS，失败返回错误码
 */
int fpga_firmware_injection_get_progress(uint32_t *current, uint32_t *total,
                                          fpga_injection_state_t *state);

/**
 * @brief 中止注入任务
 * @return 成功返回SUCCESS，失败返回错误码
 */
int fpga_firmware_injection_abort(void);

/**
 * @brief 等待注入完成
 * @param timeout_sec 超时时间（秒），0表示无限等待
 * @return 成功返回SUCCESS，超时返回ERROR_TIMEOUT，失败返回错误码
 */
int fpga_firmware_injection_wait(uint32_t timeout_sec);

/**
 * @brief 清理FPGA固件注入器
 */
void fpga_firmware_injection_cleanup(void);
```

### 5.7 告警管理API

#### 5.7.1 初始化和销毁

```c
/**
 * @brief 初始化告警管理器
 * @return 成功返回SUCCESS，失败返回错误码
 */
int alarm_manager_init(void);

/**
 * @brief 销毁告警管理器
 */
void alarm_manager_destroy(void);
```

#### 5.7.2 告警操作

```c
/**
 * @brief 产生告警
 * @param alarm_code 告警码
 * @param sub_code 告警子码
 * @param additional_info 附加信息
 * @return 成功返回SUCCESS，失败返回错误码
 */
int alarm_raise(uint32_t alarm_code, uint32_t sub_code, const char *additional_info);

/**
 * @brief 清除告警
 * @param alarm_code 告警码
 * @param sub_code 告警子码
 * @return 成功返回SUCCESS，失败返回错误码
 */
int alarm_clear(uint32_t alarm_code, uint32_t sub_code);

/**
 * @brief 发送告警上报
 * @param alarm_code 告警码
 * @param sub_code 告警子码
 * @param clear_flag 清除标志（0=产生，1=清除）
 * @param additional_info 附加信息
 * @return 成功返回SUCCESS，失败返回错误码
 */
int alarm_send_report(uint32_t alarm_code, uint32_t sub_code, uint32_t clear_flag,
                      const char *additional_info);

/**
 * @brief 检查FPGA状态并产生告警
 * @param fpga_status FPGA状态帧指针
 * @return 成功返回SUCCESS，失败返回错误码
 */
int alarm_check_fpga_status(const fpga_status_frame_t *fpga_status);
```

### 5.8 版本管理API

#### 5.8.1 版本下载

```c
/**
 * @brief 处理版本下载请求
 * @param ftp_server FTP服务器地址
 * @param ftp_port FTP服务器端口
 * @param file_path 文件路径
 * @param file_name 文件名
 * @param file_ver 版本号
 * @param file_len 文件大小
 * @return 成功返回SUCCESS，失败返回错误码
 */
int version_manager_download(const char *ftp_server, uint16_t ftp_port,
                              const char *file_path, const char *file_name,
                              const char *file_ver, uint32_t file_len);
```

#### 5.8.2 版本激活

```c
/**
 * @brief 激活指定版本
 * @param version_num 版本号
 * @return 成功返回SUCCESS，失败返回错误码
 */
int version_manager_activate(const char *version_num);

/**
 * @brief 查询当前版本
 * @param current_ver 输出当前版本号
 * @param ver_buf_size 版本号缓冲区大小
 * @return 成功返回SUCCESS，失败返回错误码
 */
int version_manager_get_current(char *current_ver, uint32_t ver_buf_size);

/**
 * @brief 查询所有已安装版本
 * @param versions 输出版本列表
 * @param max_count 最大版本数
 * @param actual_count 输出实际版本数
 * @return 成功返回SUCCESS，失败返回错误码
 */
int version_manager_list_versions(char versions[][40], uint32_t max_count,
                                   uint32_t *actual_count);
```

### 5.9 日志系统API

```c
/**
 * @brief 初始化日志系统
 * @param log_file 日志文件路径
 * @param log_level 日志级别
 * @return 成功返回SUCCESS，失败返回错误码
 */
int logger_init(const char *log_file, log_level_t log_level);

/**
 * @brief 记录日志
 * @param level 日志级别
 * @param file 源文件名
 * @param line 源文件行号
 * @param fmt 格式化字符串
 * @param ... 可变参数
 */
void logger_log(log_level_t level, const char *file, int line, const char *fmt, ...);

/**
 * @brief 关闭日志系统
 */
void logger_close(void);

// 便捷宏定义
#define LOG_DEBUG(fmt, ...) logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  logger_log(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
```

## 6. 部署指南

### 6.1 环境准备

#### 6.1.1 硬件要求

| 项目 | 要求 |
|------|------|
| CPU | ARM Cortex-A53 或更高 (D2000) |
| 内存 | 最小512MB，推荐1GB+ |
| 存储 | 最小100MB可用空间 |
| 串口 | RS-422接口（/dev/ttyAMA1） |
| 网络 | 以太网接口 |

#### 6.1.2 软件要求

| 项目 | 版本要求 |
|------|----------|
| 操作系统 | Ubuntu 18.04 LTS 或 D2000 Linux |
| GCC | 7.5.0+ |
| Make | 3.81+ |
| glibc | 2.27+ |

### 6.2 依赖安装

```bash
# 更新软件源
sudo apt-get update

# 安装编译工具
sudo apt-get install -y build-essential gcc g++ make

# 安装开发库
sudo apt-get install -y libc6-dev

# 验证安装
gcc --version
make --version
```

### 6.3 编译部署

#### 6.3.1 获取源代码

```bash
# 从Windows拷贝到D2000
cd /tmp
tar -xzf s_band_antenna_mgmt.tar.gz
cd s_band_antenna_mgmt
```

#### 6.3.2 编译程序

```bash
# 清理旧的编译产物
make clean

# 编译
make

# 验证编译结果
ls -lh build/release/bin/antenna_mgmt
```

#### 6.3.3 安装到系统

```bash
# 安装到 /opt/vendor/
sudo make install

# 验证安装
ls -l /opt/vendor/current/antenna_mgmt
```

### 6.4 配置文件

#### 6.4.1 创建配置文件

```bash
sudo mkdir -p /etc/antenna_mgmt
sudo cp config/antenna_mgmt.conf /etc/antenna_mgmt/
```

#### 6.4.2 修改配置参数

```bash
sudo vi /etc/antenna_mgmt/antenna_mgmt.conf
```

**关键配置项**：
```ini
# BBU服务器配置
BBU_IP=10.10.10.6
BBU_PORT=30000

# PAAU客户端配置
PAAU_IP=10.10.10.8
PAAU_ID=0

# UART配置
UART_DEVICE=/dev/ttyAMA1

# 日志配置
LOG_LEVEL=INFO
LOG_FILE=/var/log/antenna_mgmt/antenna_mgmt.log
```

### 6.5 systemd服务配置

#### 6.5.1 安装服务文件

```bash
# 拷贝服务文件
sudo cp scripts/antenna-mgmt.service /etc/systemd/system/

# 拷贝启动脚本
sudo cp scripts/antenna-mgmt-launcher.sh /opt/vendor/current/

# 设置执行权限
sudo chmod +x /opt/vendor/current/antenna-mgmt-launcher.sh
sudo chmod +x /opt/vendor/current/antenna_mgmt

# 重新加载systemd
sudo systemctl daemon-reload
```

#### 6.5.2 启动服务

```bash
# 启动服务
sudo systemctl start antenna-mgmt

# 查看状态
sudo systemctl status antenna-mgmt

# 设置开机自启
sudo systemctl enable antenna-mgmt
```

#### 6.5.3 服务管理命令

```bash
# 停止服务
sudo systemctl stop antenna-mgmt

# 重启服务
sudo systemctl restart antenna-mgmt

# 查看日志
sudo journalctl -u antenna-mgmt -f

# 禁用开机自启
sudo systemctl disable antenna-mgmt
```

### 6.6 串口权限配置

```bash
# 添加用户到dialout组
sudo usermod -a -G dialout $USER

# 或直接修改串口权限
sudo chmod 666 /dev/ttyAMA1

# 验证权限
ls -l /dev/ttyAMA1
```

### 6.7 日志目录创建

```bash
# 创建日志目录
sudo mkdir -p /var/log/antenna_mgmt

# 设置权限
sudo chown $USER:$USER /var/log/antenna_mgmt
sudo chmod 755 /var/log/antenna_mgmt
```

### 6.8 验证部署

#### 6.8.1 检查进程

```bash
# 查看进程
ps aux | grep antenna_mgmt

# 查看端口
netstat -anp | grep antenna_mgmt
```

#### 6.8.2 检查日志

```bash
# 查看系统日志
tail -f /var/log/antenna_mgmt/antenna_mgmt.log

# 查看systemd日志
journalctl -u antenna-mgmt -n 100
```

#### 6.8.3 测试连接

```bash
# 测试TCP连接到BBU
telnet 10.10.10.6 30000

# 测试串口
minicom -D /dev/ttyAMA1 -b 921600
```

---

## 7. 使用手册

### 7.1 启动和停止

#### 7.1.1 手动启动

```bash
# 直接运行
cd /opt/vendor/current
./antenna_mgmt

# 指定配置文件
./antenna_mgmt /etc/antenna_mgmt/antenna_mgmt.conf

# 后台运行
nohup ./antenna_mgmt > /dev/null 2>&1 &
```

#### 7.1.2 服务方式启动

```bash
# 启动服务
sudo systemctl start antenna-mgmt

# 停止服务
sudo systemctl stop antenna-mgmt

# 重启服务
sudo systemctl restart antenna-mgmt
```

### 7.2 日志查看

#### 7.2.1 实时日志

```bash
# 查看应用日志
tail -f /var/log/antenna_mgmt/antenna_mgmt.log

# 查看systemd日志
journalctl -u antenna-mgmt -f

# 过滤错误日志
tail -f /var/log/antenna_mgmt/antenna_mgmt.log | grep ERROR
```

#### 7.2.2 历史日志

```bash
# 查看最近100行
tail -n 100 /var/log/antenna_mgmt/antenna_mgmt.log

# 查看指定时间段
journalctl -u antenna-mgmt --since "2026-03-18 10:00:00" --until "2026-03-18 11:00:00"

# 搜索关键字
grep "Alarm" /var/log/antenna_mgmt/antenna_mgmt.log
```

### 7.3 版本管理操作

#### 7.3.1 创建版本包

```bash
# 编译新版本
make clean
make

# 创建版本包
cd scripts
./create_version_package.sh -v v1.0.2 -b ../build/release/bin/antenna_mgmt

# 验证版本包
./verify_package.sh antenna_v1.0.2.tar.gz
```

#### 7.3.2 上传版本包

```bash
# 上传到FTP服务器
curl -T antenna_v1.0.2.tar.gz --user anonymous: ftp://10.10.10.6/versions/
```

#### 7.3.3 触发下载和激活

通过BBU管理界面或API发送CPRI消息：
- 版本下载请求 (MsgID: 21)
- 版本激活指示 (MsgID: 31)

#### 7.3.4 查看当前版本

```bash
# 查看符号链接
readlink -f /opt/vendor/current

# 查看版本列表
ls -l /opt/vendor/versions/
```

### 7.4 告警处理

#### 7.4.1 查看告警日志

```bash
# 查看告警
grep "Alarm raised" /var/log/antenna_mgmt/antenna_mgmt.log

# 查看告警清除
grep "Alarm cleared" /var/log/antenna_mgmt/antenna_mgmt.log
```

#### 7.4.2 常见告警处理

**本振失锁告警 (50001)**：
```bash
# 检查FPGA状态
# 查看日志中的本振状态
grep "lo_lock_status" /var/log/antenna_mgmt/antenna_mgmt.log

# 处理建议：检查硬件连接，重启FPGA
```

**过温告警 (1156)**：
```bash
# 查看温度值
grep "temp" /var/log/antenna_mgmt/antenna_mgmt.log

# 处理建议：检查散热，降低环境温度
```

**光链路丢失 (1097)**：
```bash
# 查看光链路状态
grep "link_success_flag" /var/log/antenna_mgmt/antenna_mgmt.log

# 处理建议：检查光纤连接
```

### 7.5 配置修改

#### 7.5.1 修改BBU地址

```bash
# 编辑配置文件
sudo vi /etc/antenna_mgmt/antenna_mgmt.conf

# 修改BBU_IP和BBU_PORT
BBU_IP=10.10.10.100
BBU_PORT=30001

# 重启服务
sudo systemctl restart antenna-mgmt
```

#### 7.5.2 修改日志级别

```bash
# 编辑配置文件
sudo vi /etc/antenna_mgmt/antenna_mgmt.conf

# 修改LOG_LEVEL
LOG_LEVEL=DEBUG  # DEBUG/INFO/WARN/ERROR

# 重启服务
sudo systemctl restart antenna-mgmt
```

### 7.6 故障恢复

#### 7.6.1 重启服务

```bash
# 重启服务
sudo systemctl restart antenna-mgmt

# 查看状态
sudo systemctl status antenna-mgmt
```

#### 7.6.2 清理日志

```bash
# 清理旧日志
sudo rm -f /var/log/antenna_mgmt/antenna_mgmt.log.*

# 或使用logrotate
sudo logrotate -f /etc/logrotate.d/antenna-mgmt
```

#### 7.6.3 版本回滚

```bash
# 切换到旧版本
sudo rm /opt/vendor/current
sudo ln -s /opt/vendor/versions/v1.0.1 /opt/vendor/current

# 重启服务
sudo systemctl restart antenna-mgmt
```

---

## 8. 性能指标与监控

### 8.1 性能指标

#### 8.1.1 通信性能

| 指标 | 目标值 | 说明 |
|------|--------|------|
| TCP连接建立时间 | < 1秒 | 到BBU的连接时间 |
| TCP重连间隔 | 3秒 | 断线后重连间隔 |
| UART波特率 | 921600 bps | 与FPGA通信速率 |
| FPGA状态轮询间隔 | 1秒 | 状态查询频率 |
| 心跳间隔 | 3秒 | 保活消息间隔 |

#### 8.1.2 固件上注性能

| 指标 | 目标值 | 说明 |
|------|--------|------|
| 首段大小 | 1000字节 | 第一段数据大小 |
| 其他段大小 | 1002字节 | 其他段数据大小 |
| 段重传次数 | 最多3次 | 失败后重试次数 |
| 传输开始超时 | 5分钟 | FPGA准备超时 |
| 重构超时 | 10分钟 | FPGA重构超时 |
| 重构轮询间隔 | 5秒 | 重构状态查询间隔 |

#### 8.1.3 资源占用

| 指标 | 典型值 | 说明 |
|------|--------|------|
| 内存占用 | < 50MB | 运行时内存 |
| CPU占用 | < 5% | 空闲时CPU使用率 |
| 磁盘占用 | < 10MB | 程序文件大小 |
| 日志增长速度 | < 1MB/天 | INFO级别日志 |

### 8.2 监控方案

#### 8.2.1 进程监控

```bash
# 监控脚本
#!/bin/bash
while true; do
    if ! pgrep -x "antenna_mgmt" > /dev/null; then
        echo "$(date): Process died, restarting..."
        systemctl restart antenna-mgmt
    fi
    sleep 10
done
```

#### 8.2.2 日志监控

```bash
# 监控错误日志
tail -f /var/log/antenna_mgmt/antenna_mgmt.log | grep -E "ERROR|WARN"

# 统计错误数量
grep "ERROR" /var/log/antenna_mgmt/antenna_mgmt.log | wc -l
```

#### 8.2.3 连接监控

```bash
# 检查TCP连接
netstat -anp | grep antenna_mgmt | grep ESTABLISHED

# 检查串口占用
lsof | grep /dev/ttyAMA1
```

#### 8.2.4 资源监控

```bash
# 监控内存和CPU
top -p $(pgrep antenna_mgmt)

# 或使用ps
ps aux | grep antenna_mgmt
```

### 8.3 性能优化建议

#### 8.3.1 网络优化

```bash
# 增大TCP缓冲区
sudo sysctl -w net.core.rmem_max=8388608
sudo sysctl -w net.core.wmem_max=8388608

# 启用TCP快速打开
sudo sysctl -w net.ipv4.tcp_fastopen=3
```

#### 8.3.2 串口优化

```bash
# 设置串口低延迟模式
stty -F /dev/ttyAMA1 low_latency
```

#### 8.3.3 日志优化

```ini
# 生产环境使用INFO级别
LOG_LEVEL=INFO

# 调试时使用DEBUG级别
LOG_LEVEL=DEBUG
```

---

## 9. 安全策略与权限管理

### 9.1 文件权限

#### 9.1.1 程序文件权限

```bash
# 可执行程序
chmod 755 /opt/vendor/current/antenna_mgmt

# 配置文件（只读）
chmod 644 /etc/antenna_mgmt/antenna_mgmt.conf

# 日志目录
chmod 755 /var/log/antenna_mgmt
```

#### 9.1.2 版本目录权限

```bash
# 版本目录
chmod 755 /opt/vendor/versions

# 版本文件
chmod 644 /opt/vendor/versions/*/antenna_mgmt.bin
chmod 644 /opt/vendor/versions/*/checksum.txt
chmod 644 /opt/vendor/versions/*/metadata.json
```

### 9.2 用户权限

#### 9.2.1 运行用户

建议创建专用用户运行服务：

```bash
# 创建用户
sudo useradd -r -s /bin/false antenna

# 设置文件所有者
sudo chown -R antenna:antenna /opt/vendor
sudo chown -R antenna:antenna /var/log/antenna_mgmt

# 添加到dialout组（串口权限）
sudo usermod -a -G dialout antenna
```

#### 9.2.2 systemd服务用户

修改服务文件：

```ini
[Service]
User=antenna
Group=antenna
```

### 9.3 网络安全

#### 9.3.1 防火墙配置

```bash
# 允许到BBU的连接
sudo iptables -A OUTPUT -d 10.10.10.6 -p tcp --dport 30000 -j ACCEPT

# 拒绝其他出站连接（可选）
sudo iptables -A OUTPUT -p tcp --dport 30000 -j DROP
```

#### 9.3.2 TCP连接限制

```c
// 代码中限制只连接到配置的BBU地址
if (strcmp(server_ip, g_config.bbu_ip) != 0) {
    LOG_ERROR("Unauthorized server IP: %s", server_ip);
    return ERROR_INVALID_PARAM;
}
```

### 9.4 数据安全

#### 9.4.1 版本包校验

系统使用SHA-256校验和验证版本包完整性：

```c
// 计算文件SHA-256
uint8_t hash[32];
sha256_file(file_path, hash);

// 比对校验和
if (memcmp(hash, expected_hash, 32) != 0) {
    LOG_ERROR("Checksum mismatch!");
    return ERROR_GENERAL;
}
```

#### 9.4.2 配置文件保护

```bash
# 配置文件只读
sudo chmod 644 /etc/antenna_mgmt/antenna_mgmt.conf

# 防止意外修改
sudo chattr +i /etc/antenna_mgmt/antenna_mgmt.conf

# 解除保护（需要修改时）
sudo chattr -i /etc/antenna_mgmt/antenna_mgmt.conf
```

### 9.5 日志安全

#### 9.5.1 日志轮转

创建 `/etc/logrotate.d/antenna-mgmt`：

```
/var/log/antenna_mgmt/antenna_mgmt.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    create 644 antenna antenna
}
```

#### 9.5.2 敏感信息过滤

代码中避免记录敏感信息：

```c
// 不要记录完整的密码或密钥
LOG_INFO("FTP login: user=%s, pass=***", username);

// 不要记录完整的IP地址（可选）
LOG_INFO("Connected to BBU: %s", mask_ip(bbu_ip));
```

---

## 10. 故障排查

### 10.1 常见问题

#### 10.1.1 TCP连接失败

**现象**：
```
[ERROR] TCP connect failed: Connection refused
```

**排查步骤**：
1. 检查BBU服务器是否运行
   ```bash
   telnet 10.10.10.6 30000
   ```

2. 检查网络连通性
   ```bash
   ping 10.10.10.6
   ```

3. 检查防火墙规则
   ```bash
   sudo iptables -L -n
   ```

4. 检查配置文件
   ```bash
   cat /etc/antenna_mgmt/antenna_mgmt.conf | grep BBU
   ```

#### 10.1.2 串口打开失败

**现象**：
```
[ERROR] UART open failed: Permission denied
```

**排查步骤**：
1. 检查串口设备
   ```bash
   ls -l /dev/ttyAMA1
   ```

2. 检查用户权限
   ```bash
   groups $USER
   # 应包含dialout组
   ```

3. 添加权限
   ```bash
   sudo usermod -a -G dialout $USER
   # 或
   sudo chmod 666 /dev/ttyAMA1
   ```

4. 检查串口占用
   ```bash
   lsof | grep /dev/ttyAMA1
   ```

#### 10.1.3 FPGA状态无响应

**现象**：
```
[WARN] FPGA status query timeout
```

**排查步骤**：
1. 检查串口连接
   ```bash
   minicom -D /dev/ttyAMA1 -b 921600
   ```

2. 检查FPGA电源

3. 检查波特率配置
   ```bash
   stty -F /dev/ttyAMA1
   ```

4. 重启FPGA

#### 10.1.4 固件上注失败

**现象**：
```
[ERROR] Firmware injection failed: CRC mismatch
```

**排查步骤**：
1. 验证固件包
   ```bash
   cd scripts
   ./verify_package.sh antenna_v1.0.1.tar.gz
   ```

2. 检查文件完整性
   ```bash
   sha256sum /opt/vendor/versions/v1.0.1/antenna_mgmt.bin
   ```

3. 重新下载固件包

4. 检查FPGA存储空间

#### 10.1.5 告警误报

**现象**：
```
[WARN] Alarm raised: 过温告警 (code=1156, sub=0)
```

**排查步骤**：
1. 查看实际温度值
   ```bash
   grep "temp1=" /var/log/antenna_mgmt/antenna_mgmt.log | tail -1
   ```

2. 检查温度门限配置

3. 校准温度传感器

4. 调整告警门限

### 10.2 调试技巧

#### 10.2.1 启用DEBUG日志

```bash
# 修改配置
sudo vi /etc/antenna_mgmt/antenna_mgmt.conf
LOG_LEVEL=DEBUG

# 重启服务
sudo systemctl restart antenna-mgmt

# 查看详细日志
tail -f /var/log/antenna_mgmt/antenna_mgmt.log
```

#### 10.2.2 使用GDB调试

```bash
# 编译调试版本
make clean
make DEBUG=1

# 启动GDB
gdb ./antenna_mgmt

# 设置断点
(gdb) break main
(gdb) break tcp_client_connect

# 运行
(gdb) run

# 查看变量
(gdb) print g_config
(gdb) print *client
```

#### 10.2.3 抓包分析

```bash
# 抓取TCP包
sudo tcpdump -i eth0 host 10.10.10.6 and port 30000 -w tcp.pcap

# 抓取串口数据
sudo cat /dev/ttyAMA1 | hexdump -C > uart.log
```

#### 10.2.4 strace跟踪

```bash
# 跟踪系统调用
sudo strace -p $(pgrep antenna_mgmt) -o strace.log

# 跟踪文件操作
sudo strace -e trace=open,read,write -p $(pgrep antenna_mgmt)

# 跟踪网络操作
sudo strace -e trace=socket,connect,send,recv -p $(pgrep antenna_mgmt)
```

### 10.3 日志分析

#### 10.3.1 查找错误模式

```bash
# 统计错误类型
grep "ERROR" /var/log/antenna_mgmt/antenna_mgmt.log | awk '{print $NF}' | sort | uniq -c

# 查找频繁错误
grep "ERROR" /var/log/antenna_mgmt/antenna_mgmt.log | tail -100

# 按时间段分析
grep "2026-03-18 10:" /var/log/antenna_mgmt/antenna_mgmt.log | grep ERROR
```

#### 10.3.2 性能分析

```bash
# 统计消息处理时间
grep "Message processed" /var/log/antenna_mgmt/antenna_mgmt.log | awk '{print $NF}' | sort -n

# 统计连接次数
grep "TCP connected" /var/log/antenna_mgmt/antenna_mgmt.log | wc -l

# 统计告警次数
grep "Alarm raised" /var/log/antenna_mgmt/antenna_mgmt.log | wc -l
```

### 10.4 联系支持

如遇到无法解决的问题，请收集以下信息并联系技术支持：

1. 系统信息
   ```bash
   uname -a
   cat /etc/os-release
   ```

2. 程序版本
   ```bash
   readlink -f /opt/vendor/current
   ```

3. 配置文件
   ```bash
   cat /etc/antenna_mgmt/antenna_mgmt.conf
   ```

4. 最近日志
   ```bash
   tail -n 500 /var/log/antenna_mgmt/antenna_mgmt.log > antenna_mgmt.log
   ```

5. 系统日志
   ```bash
   journalctl -u antenna-mgmt -n 500 > systemd.log
   ```

---

## 附录

### A. 配置文件完整示例

```ini
# S波段天线管理系统配置文件
# 版本: v1.0

# ==================== BBU服务器配置 ====================
# BBU服务器IP地址
BBU_IP=10.10.10.6

# BBU服务器端口
BBU_PORT=30000

# ==================== PAAU客户端配置 ====================
# PAAU客户端IP地址
PAAU_IP=10.10.10.8

# PAAU设备ID
PAAU_ID=0

# ==================== UART配置 ====================
# UART设备路径
UART_DEVICE=/dev/ttyAMA1

# ==================== 重连配置 ====================
# TCP重连间隔（毫秒）
RECONNECT_INTERVAL_MS=3000

# ==================== 心跳配置 ====================
# 心跳间隔（毫秒）
HEARTBEAT_INTERVAL_MS=3000

# ==================== 日志配置 ====================
# 日志级别: DEBUG/INFO/WARN/ERROR
LOG_LEVEL=INFO

# 日志文件路径
LOG_FILE=/var/log/antenna_mgmt/antenna_mgmt.log

# ==================== 产品标识配置 ====================
# 厂商名称
VENDOR_NAME=StarNet

# 产品名称
PRODUCT_NAME=S-Band-PAAU

# 序列号
SERIAL_NUMBER=SN20260318001

# 生产日期
MANUFACTURE_DATE=2026-03-18

# ==================== 版本信息配置 ====================
# 软件版本
SOFTWARE_VERSION=v1.0.0

# 固件版本
FIRMWARE_VERSION=v1.0.0
```

### B. systemd服务文件示例

```ini
[Unit]
Description=S-Band Antenna Management Service
After=network.target

[Service]
Type=simple
User=antenna
Group=antenna
WorkingDirectory=/opt/vendor/current
ExecStart=/opt/vendor/current/antenna-mgmt-launcher.sh
Restart=always
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

### C. 错误码参考

| 错误码 | 宏定义 | 说明 |
|--------|--------|------|
| 0 | SUCCESS | 成功 |
| -1 | ERROR_GENERAL | 一般错误 |
| -2 | ERROR_INVALID_PARAM | 无效参数 |
| -3 | ERROR_MEMORY | 内存分配失败 |
| -4 | ERROR_NETWORK | 网络错误 |
| -5 | ERROR_TIMEOUT | 超时 |
| -6 | ERROR_NOT_INITIALIZED | 未初始化 |

### D. 相关文档索引

- [README.md](../s_band_antenna_mgmt/README.md) - 项目主文档
- [BUILD.md](../s_band_antenna_mgmt/BUILD.md) - 构建说明
- [DEPLOY_TO_D2000.md](../s_band_antenna_mgmt/DEPLOY_TO_D2000.md) - D2000部署指南
- [PROJECT_STRUCTURE.md](../s_band_antenna_mgmt/PROJECT_STRUCTURE.md) - 项目结构
- [FPGA固件上注技术文档_part5.md](./FPGA固件上注技术文档_part5.md) - RS-422协议详细说明

---

**文档结束**

**版本**: v1.0
**最后更新**: 2026-03-18
**维护者**: StarNet开发团队