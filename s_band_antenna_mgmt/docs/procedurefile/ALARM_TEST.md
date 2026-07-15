# 告警管理模块测试指南

## 测试环境准备

1. 编译程序
```bash
cd /path/to/s_band_antenna_mgmt
make clean
make
```

2. 配置文件
确保 `config/antenna_mgmt.conf` 中配置正确的BBU地址和端口。

## 测试用例

### 测试1: 本振失锁告警 (50001)

**测试步骤**:
1. 启动程序
2. 模拟FPGA返回 `lo_lock_status = 0`
3. 观察日志输出

**预期结果**:
```
[WARN] Alarm triggered: 本振失锁告警 (code=50001, sub=0) - LO unlock detected
[INFO] Sent alarm report: 本振失锁告警 (code=50001, sub=0, clear=0, serial=xxx)
```

**清除测试**:
1. 模拟FPGA返回 `lo_lock_status = 1` (锁定)
2. 观察日志输出

**预期结果**:
```
[INFO] Alarm cleared: 本振失锁告警 (code=50001, sub=0)
[INFO] Sent alarm report: 本振失锁告警 (code=50001, sub=0, clear=1, serial=xxx)
```

### 测试2: 光链路同步丢失告警 (1097)

**测试步骤**:
1. 模拟FPGA返回 `link_success_flag` 某位为0
2. 例如: `link_success_flag = 0xFE` (bit0=0, 主路0丢失)
3. 观察日志输出

**预期结果**:
```
[WARN] Alarm triggered: 光链路同步码流丢失 (code=1097, sub=0) - Optical port 0 sync lost
[INFO] Sent alarm report: 光链路同步码流丢失 (code=1097, sub=0, clear=0, serial=xxx)
```

**测试不同光口**:
- 主路0-3: sub_code = 0-3
- 备路0-3: sub_code = 4-7

### 测试3: 过温告警 (1156)

**测试步骤**:
1. 模拟FPGA返回温度超过门限
2. 例如: `temp1 = 90°C` (超过默认门限85°C)
3. 观察日志输出

**预期结果**:
```
[WARN] Alarm triggered: 过温告警 (code=1156, sub=0) - Probe 0 temp=90°C (threshold: -40~85°C)
[INFO] Sent alarm report: 过温告警 (code=1156, sub=0, clear=0, serial=xxx)
```

**测试不同探测点**:
- temp1: sub_code = 0
- temp2: sub_code = 1
- temp3: sub_code = 2

### 测试4: 通道故障过多告警 (1159)

**测试步骤**:
1. 模拟FPGA返回 `channel_fault_count = 15` (超过默认门限10)
2. 观察日志输出

**预期结果**:
```
[WARN] Alarm triggered: 通道故障过多 (code=1159, sub=0) - Channel fault count=15 (threshold: 10)
[INFO] Sent alarm report: 通道故障过多 (code=1159, sub=0, clear=0, serial=xxx)
```

### 测试5: TCP断开告警 (1160)

**测试步骤**:
1. 启动程序,等待TCP连接建立
2. 断开BBU服务器(或拔网线)
3. 观察日志输出

**预期结果**:
```
[WARN] TCP disconnected from BBU
[WARN] Alarm triggered: TCP连接断开 (code=1160, sub=0) - TCP disconnected: Connection lost
[DEBUG] TCP not connected, alarm will be reported later
```

**重连测试**:
1. 恢复BBU服务器连接
2. 等待TCP重连成功
3. 观察日志输出

**预期结果**:
```
[INFO] Connected to BBU server successfully
[INFO] TCP connected, reporting pending alarms
[INFO] Sent alarm report: TCP连接断开 (code=1160, sub=0, clear=0, serial=xxx)
[INFO] Reported 1 pending alarms
[INFO] Alarm cleared: TCP连接断开 (code=1160, sub=0)
[INFO] Sent alarm report: TCP连接断开 (code=1160, sub=0, clear=1, serial=xxx)
```

### 测试6: 批量告警上报

**测试步骤**:
1. 在TCP未连接时,触发多个告警
2. 等待TCP连接建立
3. 观察所有告警是否批量上报

**预期结果**:
```
[INFO] TCP connected, reporting pending alarms
[INFO] Sent alarm report: 本振失锁告警 (code=50001, sub=0, clear=0, serial=xxx)
[INFO] Sent alarm report: 过温告警 (code=1156, sub=0, clear=0, serial=xxx)
[INFO] Sent alarm report: 通道故障过多 (code=1159, sub=0, clear=0, serial=xxx)
[INFO] Reported 3 pending alarms
```

### 测试7: 版本激活失败告警 (1158)

**测试步骤**:
1. 在版本管理模块中调用:
```c
alarm_trigger_version_activate_fail("Version 1.2.3 activation failed");
```
2. 观察日志输出

**预期结果**:
```
[WARN] Alarm triggered: 版本激活失败 (code=1158, sub=0) - Version activation failed: Version 1.2.3 activation failed
[INFO] Sent alarm report: 版本激活失败 (code=1158, sub=0, clear=0, serial=xxx)
```

## 抓包验证

使用Wireshark或tcpdump抓取TCP数据包,验证告警上报消息格式:

```bash
tcpdump -i eth0 -w alarm_test.pcap host 10.10.10.6 and port 30000
```

**验证内容**:
1. 消息ID = 111 (0x6F)
2. IE类型 = 1001 (0x03E9)
3. IE长度 = 138字节
4. 告警码、子码、清除标志正确
5. 时间戳格式正确: "yyyy-mm-dd hh:mm:ss"

## 性能测试

### 测试告警检测性能

```bash
# 使用time命令测试周期性检测耗时
time ./antenna_mgmt
```

**预期性能**:
- 每次检测耗时 < 100微秒
- CPU占用率 < 5%
- 内存占用 < 20MB

### 测试告警记录表容量

1. 连续触发64个不同的告警
2. 验证第65个告警是否被拒绝

**预期结果**:
```
[ERROR] Alarm record table full
```

## 日志分析

### 正常运行日志示例

```
[INFO] Alarm manager initialized (fault_threshold=10, temp_high=85, temp_low=-40)
[INFO] TCP connected to BBU
[INFO] TCP connected, reporting pending alarms
[WARN] Alarm triggered: 过温告警 (code=1156, sub=0) - Probe 0 temp=90°C (threshold: -40~85°C)
[INFO] Sent alarm report: 过温告警 (code=1156, sub=0, clear=0, serial=123)
[INFO] Alarm cleared: 过温告警 (code=1156, sub=0)
[INFO] Sent alarm report: 过温告警 (code=1156, sub=0, clear=1, serial=124)
```

### 异常情况日志

```
[ERROR] Alarm record table full
[ERROR] Failed to encode alarm report message
[ERROR] Invalid temperature probe ID: 15
```

## 故障排查

### 问题1: 告警未上报

**可能原因**:
1. TCP未连接
2. 告警记录表满
3. 编码失败

**排查步骤**:
1. 检查TCP连接状态
2. 检查日志中是否有 "Alarm record table full"
3. 检查日志中是否有编码错误

### 问题2: 告警重复上报

**可能原因**:
1. 状态跟踪器未正确更新
2. 告警未正确清除

**排查步骤**:
1. 检查 `alarm_is_active()` 返回值
2. 检查状态跟踪器字段

### 问题3: TCP断开告警未上报

**可能原因**:
1. TCP重连失败
2. 待上报标志未设置

**排查步骤**:
1. 检查TCP重连日志
2. 检查 `alarm_report_all_pending()` 是否被调用

## 自动化测试脚本

```bash
#!/bin/bash
# alarm_test.sh - 告警管理模块自动化测试脚本

echo "Starting alarm management test..."

# 启动程序
./antenna_mgmt &
PID=$!

# 等待初始化
sleep 2

# 测试1: 检查进程是否运行
if ps -p $PID > /dev/null; then
    echo "✓ Test 1: Process started successfully"
else
    echo "✗ Test 1: Process failed to start"
    exit 1
fi

# 测试2: 检查日志文件
if [ -f "./logs/antenna_mgmt.log" ]; then
    echo "✓ Test 2: Log file created"
else
    echo "✗ Test 2: Log file not found"
fi

# 测试3: 检查告警管理器初始化
if grep -q "Alarm manager initialized" ./logs/antenna_mgmt.log; then
    echo "✓ Test 3: Alarm manager initialized"
else
    echo "✗ Test 3: Alarm manager not initialized"
fi

# 等待运行一段时间
sleep 10

# 停止程序
kill $PID
wait $PID 2>/dev/null

echo "Test completed"
```

## 注意事项

1. 测试前确保BBU服务器可访问
2. 测试时注意观察日志文件
3. 某些告警需要FPGA硬件支持
4. TCP断开告警测试需要网络环境配合
5. 性能测试应在实际硬件环境中进行
