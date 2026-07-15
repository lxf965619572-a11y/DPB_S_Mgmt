# FPGA固件注入问题分析与解决方案

## 问题现象
1. 偶尔出现 `LOG_WARN("数据段 %u 发送失败,重试 %u/%u")`，但无其他相关WARN/ERROR
2. FPGA数据帧应答为 0xFF（RS422_DATA_RECEIVED_ERROR）

## 根本原因分析

### 1. 应答0xFF的可能原因
根据协议定义，0xFF表示FPGA端数据接收异常：
- **数据校验失败**：帧校验和不匹配（最可能）
- **段号错误**：段号不连续或重复
- **FPGA缓冲区满**：处理速度跟不上发送速度
- **数据长度异常**：超出预期长度

### 2. 发送失败的隐藏原因
`uart_rs422_send_and_wait()` 返回失败的三种情况：

#### 情况A：UART发送失败（硬件层）
- 串口写入失败（errno）
- 重试3次后仍失败
- **现象**：无ERROR日志，只有WARN

#### 情况B：接收超时（默认5秒）
- FPGA处理慢，未在5秒内应答
- 环形缓冲区有数据但包不完整
- **现象**：`LOG_WARN("UART接收超时但缓冲区有%u字节数据")`

#### 情况C：帧解析失败
- 校验和不匹配
- 包头错误
- 包长度异常
- **现象**：`LOG_WARN("校验和不匹配")` 或 `LOG_ERROR("包解析错误")`

### 3. 关键隐患点

#### 3.1 段间延迟不足（10ms）
```c
usleep(10000);  /* 10ms - fpga_firmware_injector.c:615 */
```
**问题**：FPGA需要时间处理数据（写Flash、校验、更新状态），10ms可能不够。

**建议**：增加到 50-100ms，或根据FPGA应答时间动态调整。

#### 3.2 接收超时设置（5秒）
```c
#define DEFAULT_TIMEOUT_MS  5000  /* uart_rs422_client.c:16 */
```
**问题**：FPGA在写Flash时可能需要更长时间，尤其是首段或最后一段。

**建议**：数据传输阶段使用更长超时（10-15秒）。

#### 3.3 串口奇校验配置
```c
//use_parity = 0;  /* uart_rs422_client.c:156 - 被注释 */
```
**问题**：如果FPGA期望奇校验但实际未启用，会导致数据位错误。

**建议**：确认FPGA端配置，保持一致。

#### 3.4 环形缓冲区溢出处理
新代码在缓冲区满时会清空，但这会丢失未解析的部分包数据。

**建议**：
- 增大缓冲区（5120 -> 10240）
- 或在清空前尝试解析已有数据

#### 3.5 错误日志不详细
```c
if (ret != SUCCESS) {
    LOG_WARN("数据段 %u 发送失败,重试 %u/%u", ...);
    // 没有打印具体失败原因（ret的值）
}
```

**建议**：增加详细错误码日志。

## 解决方案

### 方案1：增强错误日志（立即实施）
在 `handle_transfer_data()` 中增加详细日志：

```c
if (ret != SUCCESS) {
    task->retry_count++;
    LOG_WARN("数据段 %u 发送失败(错误码=%d),重试 %u/%u",
             task->current_segment, ret, task->retry_count, task->max_retries);
    
    // 打印最近的UART错误信息
    if (ret == ERROR_TIMEOUT) {
        LOG_WARN("  原因: 接收超时，FPGA未在规定时间内应答");
    } else if (ret == ERROR_GENERAL) {
        LOG_WARN("  原因: 通信或解析错误");
    }
    
    if (task->retry_count >= task->max_retries) {
        LOG_ERROR("数据段 %u 发送失败,已达最大重试次数,发送中止命令", task->current_segment);
        send_transfer_abort(task);
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }
    fseek(task->bin_fp, -(long)bytes_read, SEEK_CUR);
    return ERROR_GENERAL;
}
```

### 方案2：增加段间延迟（推荐）
```c
/* 段间延迟(避免FPGA缓冲区溢出) */
if (task->current_segment == 0) {
    usleep(100000);  /* 首段后延迟100ms，FPGA需要初始化Flash */
} else {
    usleep(50000);   /* 其他段延迟50ms */
}
```

### 方案3：动态调整超时
```c
/* 数据传输阶段使用更长超时 */
uint32_t old_timeout = task->uart_client->timeout_ms;
uart_rs422_set_timeout(task->uart_client, 15000);  /* 15秒 */

int ret = uart_rs422_send_and_wait(...);

uart_rs422_set_timeout(task->uart_client, old_timeout);  /* 恢复 */
```

### 方案4：应答0xFF时的详细处理
```c
if (result != RS422_DATA_RECEIVED_OK) {
    LOG_ERROR("FPGA拒绝数据段 %u, result=0x%02X: %s",
              task->current_segment, result,
              rs422_get_result_desc(RS422_CMD_DATA_ACK, result));
    
    // 0xFF表示FPGA端接收异常，可能原因：
    if (result == RS422_DATA_RECEIVED_ERROR) {
        LOG_ERROR("  可能原因:");
        LOG_ERROR("    1. 数据帧校验和不匹配");
        LOG_ERROR("    2. 段号错误或不连续");
        LOG_ERROR("    3. FPGA缓冲区满或处理异常");
        LOG_ERROR("    4. 数据长度超出预期");
        LOG_ERROR("  建议: 增加段间延迟或检查串口配置");
    }
    
    task->retry_count++;
    // ... 重试逻辑
}
```

### 方案5：增加重传前的延迟
```c
if (ret != SUCCESS) {
    task->retry_count++;
    LOG_WARN("数据段 %u 发送失败,重试 %u/%u",
             task->current_segment, task->retry_count, task->max_retries);
    
    /* 重传前等待FPGA恢复 */
    usleep(200000);  /* 200ms */
    
    fseek(task->bin_fp, -(long)bytes_read, SEEK_CUR);
    return ERROR_GENERAL;
}
```

## 排查步骤

1. **启用详细日志**：实施方案1，观察具体失败原因
2. **检查串口配置**：确认奇校验设置与FPGA一致
3. **增加段间延迟**：实施方案2，从50ms开始测试
4. **监控应答时间**：记录每个段的应答时间，找出慢的段
5. **分析0xFF模式**：记录哪些段返回0xFF，是否有规律（首段、末段、特定大小）

## 预期效果

- 减少"发送失败"的频率
- 降低0xFF应答的出现率
- 提供更详细的错误信息，便于定位问题
