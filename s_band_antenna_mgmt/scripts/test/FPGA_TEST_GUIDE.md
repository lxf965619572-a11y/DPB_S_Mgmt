# FPGA固件上注测试指南

本文档说明如何在没有BBU的情况下测试FPGA固件上注功能。

## 测试方案

### 方案1：直接测试FPGA固件上注（推荐）

**适用场景**：只有FPGA硬件，没有BBU

**工具**：`fpga_injector_test.py`

**步骤**：

1. 准备固件文件（.bin格式）
2. 连接RS-422串口到FPGA
3. 运行测试脚本

```bash
# 在D2000上执行
cd /path/to/s_band_antenna_mgmt/scripts/test

# 安装依赖
pip3 install pyserial

# 运行测试
python3 fpga_injector_test.py fpga_firmware.bin /dev/ttyAMA1
```

**测试流程**：
```
1. 传输开始 → FPGA准备接收
2. 分段传输 → 逐段发送固件数据
3. 传输结束 → FPGA校验CRC16
4. 重构启动 → FPGA开始重构
5. 状态查询 → 轮询重构结果
```

**预期输出**：
```
============================================================
FPGA Firmware Injection Test
============================================================

=== Step 1: Transfer Start ===
[FPGA] File: fpga_firmware.bin
[FPGA] Size: 12345 bytes
[FPGA] CRC16: 0xABCD
[FPGA] Total segments: 13
[FPGA] Last segment length: 347
[FPGA] Sent 25 bytes
[FPGA] Waiting for transfer start ack...
[FPGA] Received frame: cmd=0x015A, payload_len=1
[FPGA] Transfer start ACK: READY (0x00)

=== Step 2: Transfer File Data ===
[FPGA] Sent segment 0/12 (1000 bytes)
[FPGA] Received frame: cmd=0x018A, payload_len=1
[FPGA] Segment 0 ACK: SUCCESS
...
[FPGA] All 13 segments sent successfully

=== Step 3: Transfer End ===
[FPGA] Sent 10 bytes
[FPGA] Waiting for transfer end ack...
[FPGA] Received frame: cmd=0x01BB, payload_len=1
[FPGA] Transfer end ACK: CRC OK (0x00)

=== Step 4: Reconfig Start ===
[FPGA] Sent reconfig start command

=== Step 5: Query Reconfig Status ===
[FPGA] Reconfig status: IN_PROGRESS (0x11), retry 1/120
...
[FPGA] Reconfig status: SUCCESS (0x00)

============================================================
FPGA Firmware Injection SUCCESS!
============================================================
```

---

### 方案2：模拟BBU完整测试

**适用场景**：测试完整的版本管理流程（下载+激活+固件上注）

**工具**：`mock_bbu_server.py`

**步骤**：

1. 准备版本包（antenna_vX.X.X.tar.gz）
2. 在一台机器上运行模拟BBU服务器
3. 在D2000上运行天线管理程序

```bash
# 终端1：启动模拟BBU服务器
cd /path/to/s_band_antenna_mgmt/scripts/test
python3 mock_bbu_server.py antenna_v1.0.1.tar.gz

# 终端2：启动天线管理程序
cd /opt/vendor/current
./antenna_mgmt
```

**测试流程**：
```
1. BBU等待PAAU连接
2. 通道建立流程
3. BBU发送版本下载请求
4. PAAU下载并校验版本包
5. BBU发送版本激活指示
6. PAAU激活新版本并重启
7. PAAU重启后自动进行FPGA固件上注
```

---

## 测试前准备

### 1. 检查串口设备

```bash
# 查看串口设备
ls -l /dev/ttyAMA1

# 检查串口权限
sudo chmod 666 /dev/ttyAMA1

# 或添加用户到dialout组
sudo usermod -a -G dialout $USER
```

### 2. 准备固件文件

```bash
# 方案1：准备.bin文件
cp fpga_firmware.bin /tmp/

# 方案2：准备版本包
cd /path/to/s_band_antenna_mgmt/scripts
./create_version_package.sh -v v1.0.1 -b ../build/release/bin/antenna_mgmt
```

### 3. 安装Python依赖

```bash
# 安装pyserial（用于串口通信）
pip3 install pyserial

# 验证安装
python3 -c "import serial; print(serial.__version__)"
```

---

## 故障排查

### 问题1：串口打开失败

**错误**：
```
[FPGA] Failed to open serial port: [Errno 13] Permission denied: '/dev/ttyAMA1'
```

**解决**：
```bash
sudo chmod 666 /dev/ttyAMA1
# 或
sudo usermod -a -G dialout $USER
# 然后重新登录
```

### 问题2：传输开始超时

**错误**：
```
[FPGA] Transfer start timeout
```

**原因**：
- FPGA未准备好
- 串口连接问题
- 波特率不匹配

**解决**：
1. 检查FPGA电源和状态
2. 检查串口连接线
3. 确认波特率为921600

### 问题3：CRC校验失败

**错误**：
```
[FPGA] Transfer end ACK: CRC FAILED (0x11)
```

**原因**：
- 数据传输错误
- 固件文件损坏

**解决**：
1. 重新传输
2. 验证固件文件完整性
3. 检查串口通信质量

### 问题4：重构失败

**错误**：
```
[FPGA] Reconfig status: FAILED (0xFF)
```

**原因**：
- 固件格式错误
- FPGA硬件问题

**解决**：
1. 检查固件文件格式
2. 联系FPGA开发人员

---

## 测试脚本说明

### fpga_injector_test.py

**功能**：直接通过RS-422串口测试FPGA固件上注

**参数**：
- `firmware_file`：固件文件路径（.bin格式）
- `serial_port`：串口设备（默认/dev/ttyAMA1）

**返回值**：
- 0：成功
- 1：失败

**示例**：
```bash
# 基本用法
python3 fpga_injector_test.py fpga_firmware.bin

# 指定串口
python3 fpga_injector_test.py fpga_firmware.bin /dev/ttyUSB0

# 查看详细输出
python3 fpga_injector_test.py fpga_firmware.bin 2>&1 | tee test.log
```

### mock_bbu_server.py

**功能**：模拟BBU服务器，测试完整的版本管理流程

**参数**：
- `version_file`：版本包文件（.tar.gz格式）

**端口**：30000（TCP）

**示例**：
```bash
# 启动模拟BBU
python3 mock_bbu_server.py antenna_v1.0.1.tar.gz

# 在另一个终端启动天线管理程序
./antenna_mgmt
```

---

## 注意事项

1. **串口独占**：确保没有其他程序占用串口
2. **权限问题**：确保有串口访问权限
3. **固件格式**：确保固件文件格式正确
4. **超时设置**：传输大文件时可能需要更长时间
5. **日志记录**：建议保存测试日志便于分析

---

## 联系支持

如遇到问题，请提供以下信息：
1. 测试脚本输出日志
2. 固件文件大小和CRC16
3. 串口配置信息
4. FPGA硬件版本
