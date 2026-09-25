# S波段天线管理系统

## 项目简介

S波段天线管理系统是一个用于管理和控制S波段天线阵列的嵌入式软件系统。

## 目录结构

```
.
├── s_band_antenna_mgmt/      # 主要代码目录
│   ├── src/                  # 源代码（33 个 .c）
│   ├── include/              # 头文件（34 个 .h）
│   ├── config/               # 配置文件
│   ├── scripts/              # 部署和管理脚本
│   ├── tests/                # 测试代码
│   ├── docs/                 # 文档（procedurefile/ 为各模块说明）
│   └── build/                # 编译输出目录（由 make 生成，不纳入版本控制）
├── shell_test/               # shell 模块测试脚手架
├── rollback_backup/          # cell_config 改动前备份
├── _analysis_extract/        # 分析文档提取脚本
├── verify_calib.py           # 验证：校准结果查询如实回报（IE 308 -> IE 358）
├── verify_msg_id.py          # 验证：msg_id 白名单 + 透传通道开关
├── verify_p1_2.py            # 验证：天线模式校验已不在 TCP 接收线程执行
├── verify_paau_id.py         # 验证：入站 paau_id 校验接口
├── verify_reconnect.py       # 验证：反复断线重连时接收线程可正确回收
├── verify_reset.py           # 验证：复位失败如实返回、重启动作异步可配置
├── mock_bbu_test.py          # mock BBU：验证 msg 195 -> 196 往返链路
├── probe_truncation.py       # 探测：截断构造的 msg_id 如何处理（对照基线）
├── .gitignore                # Git忽略文件配置
└── README.md                 # 本文件
```

## 版本历史

### 20260925 版本（当前版本）
- 新增入站报文身份校验（`PAAU_ID_CHECK`），防止链路上冒充 BBU 的主机下发复位、
  固件上注、透传写 FPGA 等指令
- 透传通道（msg 231-240）新增开关，无需求时可整体封禁
- 复位动作改为可配置且异步执行，不再在接收线程内 sleep；FPGA 复位命令失败时如实返回失败
- 校准结果查询回真实遥测，不再恒报成功
- cell_config 改为 worker 线程异步提交，新增任务队列
- 新增 shell_util 模块

### 20260521 版本
- 新增网络配置相关文档
- 新增自动启动快速指南
- 更新核心代码模块
- 新增部署和日志查看脚本
- 完善系统集成功能

### 初始版本
- 基础系统框架
- FPGA固件上注功能
- 天线参数配置
- 状态查询功能
- 告警管理

## 快速开始

### 依赖

| 用途 | 依赖 |
|---|---|
| 编译 | gcc、make、**OpenSSL 开发包**（链接 `-lssl -lcrypto`）、pthread |
| 测试 | CMake、GoogleTest、gcov（`run_tests.sh` 走 CMake + ctest） |

Debian/Ubuntu 下可安装：

```bash
sudo apt install build-essential libssl-dev cmake libgtest-dev
```

### 编译

```bash
cd s_band_antenna_mgmt
make
```

产物为当前目录下的 `antenna_mgmt`。

> 注意：`Makefile` 以 `wildcard src/*.c` 编译 **全部** 源文件；
> 而 `CMakeLists.txt` 目前只列出了 4 个源文件，仅用于测试构建。
> 常规编译请用 `make`。

### 运行

```bash
./start.sh
```

### 测试

```bash
./run_tests.sh
```

该脚本以 Coverage 模式配置 CMake、运行 `ctest`，并生成 HTML 覆盖率报告到
`build/coverage/reports/html/index.html`。单元测试位于 `s_band_antenna_mgmt/tests/unit/`，
现有 5 个测试文件（覆盖 rs422_protocol、firmware_package、fpga_firmware_injector 等）。

## 配置说明

配置文件为 `s_band_antenna_mgmt/config/antenna_mgmt.conf`。除设备身份、网络和日志等
常规项外，以下开关会改变系统的对外行为，**部署前请确认取值**：

| 配置项 | 取值 | 说明 |
|---|---|---|
| `PAAU_ID_CHECK` | `0` / `1` / `2` | 入站报文 paau_id 校验。`0`=关闭（与改造前行为一致）；`1`=仅记录，不匹配时打 WARN 但仍受理；`2`=拒绝，不匹配则丢弃该报文 |
| `PAAU_ID_BROADCAST` | 整数 | 报文 paau_id 等于该值时一律受理（面向全体 PAAU 的广播报文）。不需要广播语义时设为负数（如 `-1`）关闭 |
| `TRANSPARENT_ENABLE` | `0` / `1` | 透传通道（msg 231-240）总开关。该通道将 BBU 内容原样写 FPGA，不校验命令码、不检查参数范围，是全项目权限最高的入站路径。正式部署若无需求，建议置 `0` 直接封禁 |
| `TRANSPARENT_AUTO_REPLY` | `0` / `1` | 收到 BBU→PAAU 后是否自动回发。置 `0` 可同时消除"收到什么都原样弹回"的反射行为 |
| `CALIB_QUERY_REPORT_REAL` | `0` / `1` | `1`=如实回报遥测 calib_result；`0`=兼容旧行为，恒报成功。遥测不可用时回报 `2`（CALIB_RESULT_UNAVAILABLE），不伪造成成功 |
| `RESET_RESTART_SERVICE` | `0` / `1` | 软复位后是否重启本服务 |
| `RESET_REBOOT_SYSTEM` | `0` / `1` | 硬复位后是否重启整个系统。影响面大，默认关闭 |

> ⚠️ **`PAAU_ID_CHECK` 切到 `2` 之前，必须先用 `1` 跑一段时间**，确认日志中没有
> `Inbound paau_id=... does not match local=...`。否则会把 BBU 链路直接切断。
> 首次出现该 WARN 时会同时打印 BBU 实际下发的值，据此把 `PAAU_ID` 改成对应值再切 `2`。

## 主要功能

- FPGA固件管理和上注
- 天线参数配置
- 状态监控和查询
- 告警管理
- 心跳检测
- 日志管理
- 网络配置和集成

### 安全加固（20260925 起）

- **入站报文身份校验** —— 校验报文的 paau_id，阻止链路上冒充 BBU 的端点下发指令
- **透传通道可控** —— 可整体关闭权限最高的原始写入路径
- **复位动作落地** —— 复位不再"只打日志不执行"，且异步执行、不阻塞接收线程
- **校准结果如实回传** —— 硬件校准失败时如实上报，避免相控阵长期带错误相位工作

## 技术文档

详细技术文档请参考：
- `s_band_antenna_mgmt/S波段天线管理系统技术文档.md`
- `s_band_antenna_mgmt/FPGA固件上注技术文档_完整版.md`
- `s_band_antenna_mgmt/docs/` 目录下的各类文档（`procedurefile/` 为各模块说明）

## Git 提交规范

- 使用中文提交信息
- 提交信息格式：`<类型>: <简短描述>`
- 类型包括：新增、修复、更新、重构、文档等

## 许可证

内部项目，版权所有。
