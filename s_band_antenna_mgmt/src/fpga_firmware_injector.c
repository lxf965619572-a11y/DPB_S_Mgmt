#include "fpga_firmware_injector.h"
#include "rs422_protocol.h"
#include "logger.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>  /* 添加此头文件以支持 tcflush 和 TCIFLUSH */

/* 全局注入任务 */
static fpga_injection_task_t g_injection_task;

/* 前向声明 */
static void* injection_thread_func(void *arg);
static int handle_parsing_metadata(fpga_injection_task_t *task);
static int handle_transfer_start(fpga_injection_task_t *task);
static int handle_transfer_data(fpga_injection_task_t *task);
static int handle_transfer_end(fpga_injection_task_t *task);
static int handle_reconfig_start(fpga_injection_task_t *task);
static int handle_reconfig_polling(fpga_injection_task_t *task);
static void transition_state(fpga_injection_task_t *task, fpga_injection_state_t new_state);
static int send_transfer_abort(fpga_injection_task_t *task);

int fpga_firmware_injection_init(uart_rs422_client_t *uart_client)
{
    if (!uart_client) {
        return ERROR_INVALID_PARAM;
    }

    memset(&g_injection_task, 0, sizeof(g_injection_task));
    g_injection_task.state = FPGA_INJ_STATE_IDLE;
    g_injection_task.uart_client = uart_client;
    g_injection_task.max_retries = FPGA_MAX_PACKET_RETRIES;
    g_injection_task.in_progress = false;
    g_injection_task.abort_requested = false;

    pthread_mutex_init(&g_injection_task.state_mutex, NULL);

    LOG_INFO("FPGA固件注入器已初始化");
    return SUCCESS;
}

int fpga_firmware_injection_start(const char *version_dir, const char *version_num)
{
    if (!version_dir || !version_num) {
        return ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_injection_task.state_mutex);

    if (g_injection_task.in_progress) {
        LOG_WARN("FPGA固件注入任务已在进行中");
        pthread_mutex_unlock(&g_injection_task.state_mutex);
        return ERROR_GENERAL;
    }

    /* 初始化任务上下文 */
    strncpy(g_injection_task.version_dir, version_dir, sizeof(g_injection_task.version_dir) - 1);
    strncpy(g_injection_task.version_num, version_num, sizeof(g_injection_task.version_num) - 1);
    g_injection_task.state = FPGA_INJ_STATE_PARSING_METADATA;
    g_injection_task.start_time = time(NULL);
    g_injection_task.last_activity = time(NULL);
    g_injection_task.in_progress = true;
    g_injection_task.abort_requested = false;
    g_injection_task.current_segment = 0;
    g_injection_task.retry_count = 0;
    g_injection_task.bin_fp = NULL;

    /* 创建后台线程 */
    int ret = pthread_create(&g_injection_task.injection_thread, NULL,
                            injection_thread_func, &g_injection_task);
    if (ret != 0) {
        LOG_ERROR("创建FPGA固件注入线程失败: %d", ret);
        g_injection_task.in_progress = false;
        pthread_mutex_unlock(&g_injection_task.state_mutex);
        return ERROR_GENERAL;
    }

    pthread_detach(g_injection_task.injection_thread);

    pthread_mutex_unlock(&g_injection_task.state_mutex);

    LOG_INFO("启动FPGA固件注入任务: %s", version_num);
    return SUCCESS;
}

int fpga_firmware_injection_get_progress(uint32_t *current, uint32_t *total,
                                          fpga_injection_state_t *state)
{
    pthread_mutex_lock(&g_injection_task.state_mutex);

    if (current) *current = g_injection_task.current_segment;
    if (total) *total = g_injection_task.total_segments;
    if (state) *state = g_injection_task.state;

    pthread_mutex_unlock(&g_injection_task.state_mutex);
    return SUCCESS;
}

int fpga_firmware_injection_abort(void)
{
    pthread_mutex_lock(&g_injection_task.state_mutex);

    if (!g_injection_task.in_progress) {
        pthread_mutex_unlock(&g_injection_task.state_mutex);
        return ERROR_GENERAL;
    }

    g_injection_task.abort_requested = true;
    LOG_WARN("请求中止FPGA固件注入任务");

    pthread_mutex_unlock(&g_injection_task.state_mutex);

    /* 发送中止命令到FPGA */
    uint8_t abort_frame[256];
    int frame_len = rs422_build_transfer_abort(abort_frame, sizeof(abort_frame));
    if (frame_len > 0) {
        uart_rs422_send_frame(g_injection_task.uart_client, abort_frame, frame_len);
        LOG_INFO("已发送传输中止命令(3-9)");
    }

    return SUCCESS;
}

int fpga_firmware_injection_wait(uint32_t timeout_sec)
{
    time_t start = time(NULL);

    while (1) {
        pthread_mutex_lock(&g_injection_task.state_mutex);
        bool in_progress = g_injection_task.in_progress;
        fpga_injection_state_t state = g_injection_task.state;
        pthread_mutex_unlock(&g_injection_task.state_mutex);

        if (!in_progress || state == FPGA_INJ_STATE_COMPLETED || state == FPGA_INJ_STATE_FAILED) {
            return (state == FPGA_INJ_STATE_COMPLETED) ? SUCCESS : ERROR_GENERAL;
        }

        if (timeout_sec > 0 && (time(NULL) - start) >= timeout_sec) {
            return ERROR_TIMEOUT;
        }

        usleep(500000);  /* 500ms */
    }
}

void fpga_firmware_injection_cleanup(void)
{
    pthread_mutex_lock(&g_injection_task.state_mutex);

    if (g_injection_task.in_progress) {
        g_injection_task.abort_requested = true;
        pthread_mutex_unlock(&g_injection_task.state_mutex);

        /* 等待线程结束 */
        sleep(2);

        pthread_mutex_lock(&g_injection_task.state_mutex);
    }

    if (g_injection_task.bin_fp) {
        fclose(g_injection_task.bin_fp);
        g_injection_task.bin_fp = NULL;
    }

    pthread_mutex_unlock(&g_injection_task.state_mutex);
    pthread_mutex_destroy(&g_injection_task.state_mutex);

    LOG_INFO("FPGA固件注入器已清理");
}

const char* fpga_injection_get_state_name(fpga_injection_state_t state)
{
    switch (state) {
        case FPGA_INJ_STATE_IDLE:               return "空闲";
        case FPGA_INJ_STATE_PARSING_METADATA:   return "解析元数据";
        case FPGA_INJ_STATE_TRANSFER_START:     return "传输开始";
        case FPGA_INJ_STATE_TRANSFER_DATA:      return "传输数据";
        case FPGA_INJ_STATE_TRANSFER_END:       return "传输结束";
        case FPGA_INJ_STATE_RECONFIG_START:     return "重构开始";
        case FPGA_INJ_STATE_RECONFIG_POLLING:   return "重构轮询";
        case FPGA_INJ_STATE_COMPLETED:          return "完成";
        case FPGA_INJ_STATE_FAILED:             return "失败";
        default:                                return "未知";
    }
}

/* 状态转换 */
static void transition_state(fpga_injection_task_t *task, fpga_injection_state_t new_state)
{
    pthread_mutex_lock(&task->state_mutex);
    fpga_injection_state_t old_state = task->state;
    task->state = new_state;
    task->last_activity = time(NULL);
    pthread_mutex_unlock(&task->state_mutex);

    LOG_INFO("FPGA固件注入状态转换: %s -> %s",
             fpga_injection_get_state_name(old_state),
             fpga_injection_get_state_name(new_state));
}

/* 注入线程主函数 */
static void* injection_thread_func(void *arg)
{
    fpga_injection_task_t *task = (fpga_injection_task_t *)arg;

    LOG_INFO("FPGA固件注入线程已启动");

    while (!task->abort_requested) {
        pthread_mutex_lock(&task->state_mutex);
        fpga_injection_state_t current_state = task->state;
        pthread_mutex_unlock(&task->state_mutex);

        int ret = SUCCESS;

        switch (current_state) {
            case FPGA_INJ_STATE_PARSING_METADATA:
                ret = handle_parsing_metadata(task);
                break;

            case FPGA_INJ_STATE_TRANSFER_START:
                ret = handle_transfer_start(task);
                break;

            case FPGA_INJ_STATE_TRANSFER_DATA:
                ret = handle_transfer_data(task);
                break;

            case FPGA_INJ_STATE_TRANSFER_END:
                ret = handle_transfer_end(task);
                break;

            case FPGA_INJ_STATE_RECONFIG_START:
                ret = handle_reconfig_start(task);
                break;

            case FPGA_INJ_STATE_RECONFIG_POLLING:
                ret = handle_reconfig_polling(task);
                break;

            case FPGA_INJ_STATE_COMPLETED:
            case FPGA_INJ_STATE_FAILED:
                goto exit_thread;

            default:
                usleep(100000);  /* 100ms */
                break;
        }

        if (ret != SUCCESS && current_state != FPGA_INJ_STATE_FAILED) {
            /* 某些状态处理失败但未转换到FAILED状态,继续重试 */
            usleep(100000);
        }
    }

exit_thread:
    /* 清理资源 */
    if (task->bin_fp) {
        fclose(task->bin_fp);
        task->bin_fp = NULL;
    }

    pthread_mutex_lock(&task->state_mutex);
    task->in_progress = false;
    fpga_injection_state_t final_state = task->state;
    pthread_mutex_unlock(&task->state_mutex);

    LOG_INFO("FPGA固件注入线程退出,最终状态: %s",
             fpga_injection_get_state_name(final_state));

    return NULL;
}

/* 处理元数据解析状态 */
static int handle_parsing_metadata(fpga_injection_task_t *task)
{
    LOG_INFO("开始解析固件元数据: %s", task->version_dir);

    /* 解析metadata.json */
    int ret = firmware_parse_metadata(task->version_dir, &task->metadata);
    if (ret != SUCCESS) {
        LOG_ERROR("解析固件元数据失败");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    /* 验证SHA256 */
    ret = firmware_verify_integrity(task->metadata.bin_file_path, task->metadata.sha256);
    if (ret != SUCCESS) {
        LOG_ERROR("固件SHA256校验失败");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    /* 计算总段数（按1MB分段） */
    task->total_segments = (task->metadata.file_size + FPGA_SEGMENT_SIZE - 1) / FPGA_SEGMENT_SIZE;
    LOG_INFO("固件文件大小: %u 字节, 总段数(1MB): %u", task->metadata.file_size, task->total_segments);

    /* 计算文件CRC16-CCITT-FALSE */
    FILE *fp = fopen(task->metadata.bin_file_path, "rb");
    if (!fp) {
        LOG_ERROR("无法打开固件文件: %s", task->metadata.bin_file_path);
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    /* 固件上注是低频维护操作，允许临时分配大内存 */
    uint8_t *file_data = (uint8_t *)malloc(task->metadata.file_size);
    if (!file_data) {
        fclose(fp);
        LOG_ERROR("内存分配失败: 需要 %u 字节", task->metadata.file_size);
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_MEMORY;
    }

    size_t bytes_read = fread(file_data, 1, task->metadata.file_size, fp);
    fclose(fp);

    if (bytes_read != task->metadata.file_size) {
        free(file_data);
        LOG_ERROR("文件读取失败: 期望%u字节, 实际读取%zu字节",
                  task->metadata.file_size, bytes_read);
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    /* 调用CRC16-CCITT-FALSE计算函数 */
    task->file_checksum = rs422_crc16_ccitt_false(file_data, task->metadata.file_size);
    free(file_data);

    LOG_INFO("固件文件CRC16: 0x%04X", task->file_checksum);

    /* 转换到传输开始状态 */
    transition_state(task, FPGA_INJ_STATE_TRANSFER_START);
    return SUCCESS;
}

/* 处理传输开始状态 */
static int handle_transfer_start(fpga_injection_task_t *task)
{
    LOG_INFO("发送文件传输开始命令(3-3)");

    /* 构造传输开始参数 */
    rs422_cmd_transfer_start_t params;
    params.device_id = (RS422_APID_CONTROL >> 4) & 0x7F;  /* APID高7位 */
    params.file_type = task->metadata.file_type;
    params.file_sub_type = task->metadata.file_sub_type;
    params.segment_info = (task->total_segments > 1) ? (3 << 14) | task->total_segments : 0;

    /* 文件长度字段：不分段时为文件总长，分段时为每段长度(1MB) */
    if (task->total_segments == 1) {
        params.file_length = task->metadata.file_size;  /* 不分段，填总长度 */
    } else {
        params.file_length = FPGA_SEGMENT_SIZE;  /* 分段时，填每段长度(1MB) */
    }

    /* 计算最后一段长度 */
    if (task->total_segments == 1) {
        params.last_segment_len = task->metadata.file_size;
    } else {
        params.last_segment_len = task->metadata.file_size % FPGA_SEGMENT_SIZE;
        if (params.last_segment_len == 0) {
            params.last_segment_len = FPGA_SEGMENT_SIZE;
        }
    }
    params.file_checksum = task->file_checksum;  /* 使用计算好的校验和 */

    /* 构造帧 */
    uint8_t send_frame[512];
    int frame_len = rs422_build_transfer_start(&params, send_frame, sizeof(send_frame));
    if (frame_len < 0) {
        LOG_ERROR("构造传输开始命令失败");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }
     /* 打印发送的传输开始命令 */
    LOG_DEBUG("传输开始命令(3-3)参数: device_id=0x%02X, file_type=0x%02X, file_sub_type=0x%02X, "
             "segment_info=0x%04X, file_length=%u, last_segment_len=%u, checksum=0x%08X",
             params.device_id, params.file_type, params.file_sub_type,
             params.segment_info, params.file_length, params.last_segment_len, params.file_checksum);

    /* 发送并等待响应(轮询直到READY或超时) */
    time_t start_time = time(NULL);
    uint8_t result = RS422_TRANSFER_PREPARING;

    while (result == RS422_TRANSFER_PREPARING) {
        /* 检查超时 */
        if ((time(NULL) - start_time) > FPGA_TRANSFER_START_TIMEOUT_SEC) {
            LOG_ERROR("传输开始超时(5分钟)");
            transition_state(task, FPGA_INJ_STATE_FAILED);
            return ERROR_TIMEOUT;
        }

        /* 发送命令 */
        uint8_t recv_frame[256];
        uint32_t recv_len;
        int ret = uart_rs422_send_and_wait(task->uart_client,
                                           send_frame, frame_len,
                                           recv_frame, sizeof(recv_frame),
                                           &recv_len, RS422_CMD_TRANSFER_START_ACK);

        if (ret != SUCCESS) {
            LOG_WARN("发送传输开始命令失败,1秒后重试");
            sleep(1);
            continue;
        }

        /* 解析响应 */
        uint16_t apid, cmd_code;
        const uint8_t *payload;
        uint32_t payload_len;
        ret = rs422_decode_frame(recv_frame, recv_len, &apid, &cmd_code, &payload, &payload_len);
        if (ret != SUCCESS) {
            LOG_ERROR("解码传输开始应答失败");
            sleep(1);
            continue;
        }

        ret = rs422_parse_transfer_start_ack(payload, payload_len, &result);
        if (ret != SUCCESS) {
            LOG_ERROR("解析传输开始应答失败");
            sleep(1);
            continue;
        }

        LOG_INFO("传输开始应答: %s (0x%02X)",
                 rs422_get_result_desc(RS422_CMD_TRANSFER_START_ACK, result), result);

        if (result == RS422_TRANSFER_PREPARING) {
            sleep(1);  /* 继续轮询 */
        }
    }

    if (result != RS422_TRANSFER_READY) {
        LOG_ERROR("FPGA拒绝传输: result=0x%02X", result);
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    /* 打开.bin文件准备传输 */
    task->bin_fp = fopen(task->metadata.bin_file_path, "rb");
    if (!task->bin_fp) {
        LOG_ERROR("无法打开固件文件: %s", task->metadata.bin_file_path);
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }
    
    /**子阵擦除完成要等待20s，擦除其他子阵 */
    sleep(20);

    task->current_segment = 0;
    task->current_packet_in_segment = 0;
    task->segment_offset = 0;
    task->retry_count = 0;

    LOG_INFO("FPGA准备就绪,开始数据传输");
    transition_state(task, FPGA_INJ_STATE_TRANSFER_DATA);
    return SUCCESS;
}

/* 发送传输异常中止命令 */
static int send_transfer_abort(fpga_injection_task_t *task)
{
    LOG_WARN("发送文件传输异常中止命令(3-9)");

    /* 构造中止命令 */
    uint8_t abort_frame[256];
    int frame_len = rs422_build_transfer_abort(abort_frame, sizeof(abort_frame));
    if (frame_len < 0) {
        LOG_ERROR("构造传输中止命令失败");
        return ERROR_GENERAL;
    }

    /* 发送中止命令 */
    int ret = uart_rs422_send_frame(task->uart_client, abort_frame, frame_len);
    if (ret != SUCCESS) {
        LOG_ERROR("发送传输中止命令失败");
        return ERROR_GENERAL;
    }

    /* 等待中止应答(3-10) */
    uint8_t recv_buf[256];
    uint32_t recv_len;
    ret = uart_rs422_send_and_wait(task->uart_client,
                                   abort_frame, frame_len,
                                   recv_buf, sizeof(recv_buf),
                                   &recv_len, RS422_CMD_ABORT_ACK);

    if (ret != SUCCESS) {
        LOG_WARN("未收到中止应答,继续清理");
        return SUCCESS;  /* 即使没收到应答也继续 */
    }

    LOG_INFO("已收到传输中止应答(3-10)");
    return SUCCESS;
}


/* 处理数据传输状态 */
static int handle_transfer_data(fpga_injection_task_t *task)
{
    /* 检查是否所有段都已发送完成 */
    if (task->current_segment >= task->total_segments) {
        LOG_INFO("所有数据段发送完成");
        transition_state(task, FPGA_INJ_STATE_TRANSFER_END);
        return SUCCESS;
    }

    /* 计算当前段的大小和包数 */
    uint32_t current_segment_size;
    if (task->current_segment == task->total_segments - 1) {
        /* 最后一段：可能不足1MB */
        current_segment_size = task->metadata.file_size - (task->current_segment * FPGA_SEGMENT_SIZE);
    } else {
        /* 非最后一段：完整的1MB */
        current_segment_size = FPGA_SEGMENT_SIZE;
    }

    /* 如果是新段的开始，计算该段的总包数 */
    if (task->current_packet_in_segment == 0) {
        task->total_packets_in_segment = (current_segment_size + FPGA_PACKET_SIZE - 1) / FPGA_PACKET_SIZE;
        task->segment_offset = task->current_segment * FPGA_SEGMENT_SIZE;
        LOG_INFO("开始发送段 %u/%u (大小: %u 字节, 包数: %u)",
                 task->current_segment + 1, task->total_segments,
                 current_segment_size, task->total_packets_in_segment);

        /* 新段开始，定位到段起始位置 */
        if (fseek(task->bin_fp, task->segment_offset, SEEK_SET) != 0) {
            LOG_ERROR("文件定位失败: offset=%u", task->segment_offset);
            transition_state(task, FPGA_INJ_STATE_FAILED);
            return ERROR_GENERAL;
        }
    }

    /* 检查当前段是否发送完成 */
    if (task->current_packet_in_segment >= task->total_packets_in_segment) {
        /* 当前段发送完成，进入下一段 */
        task->current_segment++;
        task->current_packet_in_segment = 0;
        task->retry_count = 0;
        LOG_INFO("段 %u 发送完成", task->current_segment);
        return SUCCESS;  /* 继续下一段 */
    }

    /* 计算当前包的大小 */
    uint32_t packet_size = FPGA_PACKET_SIZE;

    /* 最后一个包可能不足1000字节 */
    if (task->current_packet_in_segment == task->total_packets_in_segment - 1) {
        uint32_t remaining = current_segment_size - (task->current_packet_in_segment * FPGA_PACKET_SIZE);
        if (remaining < FPGA_PACKET_SIZE) {
            packet_size = remaining;
        }
    }

    /* 如果是重发，需要定位到正确位置 */
    if (task->retry_count > 0) {
        uint32_t packet_offset = task->segment_offset + (task->current_packet_in_segment * FPGA_PACKET_SIZE);
        if (fseek(task->bin_fp, packet_offset, SEEK_SET) != 0) {
            LOG_ERROR("重发时文件定位失败: offset=%u", packet_offset);
            transition_state(task, FPGA_INJ_STATE_FAILED);
            return ERROR_GENERAL;
        }
    }

    /* 读取数据（正常情况下顺序读取，重发时已经fseek定位） */
    uint8_t data_buf[FPGA_PACKET_SIZE];
    size_t bytes_read = fread(data_buf, 1, packet_size, task->bin_fp);
    if (bytes_read != packet_size) {
        LOG_ERROR("文件读取失败: 期望%u字节, 实际读取%zu字节", packet_size, bytes_read);
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    /* 构造数据帧 - segment_num是1MB段号, packet_index是段内包索引 */
    uint8_t frame_buf[FPGA_PACKET_SIZE + 512];
    int frame_len = rs422_build_file_data(task->current_segment,
                                           data_buf, bytes_read,
                                           frame_buf, sizeof(frame_buf),
                                           task->current_packet_in_segment,
                                           task->total_packets_in_segment);
    if (frame_len < 0) {
        LOG_ERROR("构造数据帧失败");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    /* 发送前清空接收缓冲区 */
    tcflush(task->uart_client->fd, TCIOFLUSH);

    /* 发送数据帧并等待应答 */
    uint8_t recv_buf[256];
    uint32_t recv_len;
    int ret = uart_rs422_send_and_wait(task->uart_client,
                                       frame_buf, frame_len,
                                       recv_buf, sizeof(recv_buf),
                                       &recv_len, RS422_CMD_DATA_ACK);

    if (ret != SUCCESS) {
        /* 重试 */
        task->retry_count++;
        if (task->retry_count >= task->max_retries) {
            LOG_ERROR("段 %u 包 %u 发送失败,已达最大重试次数,发送中止命令",
                      task->current_segment, task->current_packet_in_segment);
            send_transfer_abort(task);
            transition_state(task, FPGA_INJ_STATE_FAILED);
            return ERROR_GENERAL;
        }
        LOG_WARN("段 %u 包 %u 发送失败(错误码=%d),重试 %u/%u",
                 task->current_segment, task->current_packet_in_segment,
                 ret, task->retry_count, task->max_retries);
        return ERROR_GENERAL;
    }

    /* 解析应答 */
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;
    ret = rs422_decode_frame(recv_buf, recv_len, &apid, &cmd_code, &payload, &payload_len);
    if (ret != SUCCESS) {
        LOG_ERROR("解码数据应答失败");
        task->retry_count++;
        if (task->retry_count >= task->max_retries) {
            LOG_ERROR("解码失败已达最大重试次数,发送中止命令");
            send_transfer_abort(task);
            transition_state(task, FPGA_INJ_STATE_FAILED);
            return ERROR_GENERAL;
        }
        return ERROR_GENERAL;
    }

    uint8_t result;
    ret = rs422_parse_data_ack(payload, payload_len, &result);
    if (ret != SUCCESS) {
        LOG_ERROR("解析数据应答失败");
        task->retry_count++;
        if (task->retry_count >= task->max_retries) {
            LOG_ERROR("解析失败已达最大重试次数,发送中止命令");
            send_transfer_abort(task);
            transition_state(task, FPGA_INJ_STATE_FAILED);
            return ERROR_GENERAL;
        }
        return ERROR_GENERAL;
    }

    if (result != RS422_DATA_RECEIVED_OK) {
        LOG_ERROR("FPGA拒绝段 %u 包 %u, result=0x%02X: %s",
                  task->current_segment, task->current_packet_in_segment, result,
                  rs422_get_result_desc(RS422_CMD_DATA_ACK, result));
        task->retry_count++;
        if (task->retry_count >= task->max_retries) {
            LOG_ERROR("段 %u 包 %u 已达最大重试次数,发送中止命令",
                      task->current_segment, task->current_packet_in_segment);
            send_transfer_abort(task);
            transition_state(task, FPGA_INJ_STATE_FAILED);
            return ERROR_GENERAL;
        }
        return ERROR_GENERAL;
    }

    /* 成功,继续下一包 */
    task->current_packet_in_segment++;
    task->retry_count = 0;
    
    usleep(1000); //1ms
    /* 进度回调 */
    if (task->progress_callback) {
        uint32_t total_packets = 0;
        uint32_t current_packets = 0;

        /* 计算总包数 */
        for (uint32_t i = 0; i < task->total_segments; i++) {
            uint32_t seg_size = (i == task->total_segments - 1) ?
                (task->metadata.file_size - i * FPGA_SEGMENT_SIZE) : FPGA_SEGMENT_SIZE;
            total_packets += (seg_size + FPGA_PACKET_SIZE - 1) / FPGA_PACKET_SIZE;
        }

        /* 计算已发送包数 */
        for (uint32_t i = 0; i < task->current_segment; i++) {
            uint32_t seg_size = (i == task->total_segments - 1) ?
                (task->metadata.file_size - i * FPGA_SEGMENT_SIZE) : FPGA_SEGMENT_SIZE;
            current_packets += (seg_size + FPGA_PACKET_SIZE - 1) / FPGA_PACKET_SIZE;
        }
        current_packets += task->current_packet_in_segment;

        task->progress_callback(current_packets, total_packets);
    }

    return SUCCESS;
}

/* 处理传输结束状态 */
static int handle_transfer_end(fpga_injection_task_t *task)
{
    LOG_INFO("发送文件传输结束命令(3-7)");

    /* 关闭文件 */
    if (task->bin_fp) {
        fclose(task->bin_fp);
        task->bin_fp = NULL;
    }

    /* 构造传输结束命令 */
    uint8_t send_frame[256];
    int frame_len = rs422_build_transfer_end(send_frame, sizeof(send_frame));
    if (frame_len < 0) {
        LOG_ERROR("构造传输结束命令失败");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    /* 发送并等待响应 */
    uint8_t recv_frame[256];
    uint32_t recv_len;
    int ret = uart_rs422_send_and_wait(task->uart_client,
                                       send_frame, frame_len,
                                       recv_frame, sizeof(recv_frame),
                                       &recv_len, RS422_CMD_TRANSFER_END_ACK);

    if (ret != SUCCESS) {
        LOG_ERROR("发送传输结束命令失败");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    /* 解析响应 */
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;
    ret = rs422_decode_frame(recv_frame, recv_len, &apid, &cmd_code, &payload, &payload_len);
    if (ret != SUCCESS) {
        LOG_ERROR("解码传输结束应答失败");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    uint8_t result;
    ret = rs422_parse_transfer_end_ack(payload, payload_len, &result);
    if (ret != SUCCESS) {
        LOG_ERROR("解析传输结束应答失败");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    LOG_INFO("传输结束应答: %s (0x%02X)",
             rs422_get_result_desc(RS422_CMD_TRANSFER_END_ACK, result), result);

    if (result != RS422_TRANSFER_COMPLETE_OK) {
        LOG_ERROR("FPGA文件传输异常: result=0x%02X", result);
        send_transfer_abort(task);
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    LOG_INFO("FPGA文件接收正常,传输完成");
    transition_state(task, FPGA_INJ_STATE_RECONFIG_START);
    return SUCCESS;
}

/* 处理重构开始状态 */
static int handle_reconfig_start(fpga_injection_task_t *task)
{
    LOG_INFO("发送重构启动命令(1-1)");

    /* 构造重构启动命令 */
    uint8_t send_frame[256];
    int frame_len = rs422_build_reconfig_start(task->metadata.file_type,
                                                task->metadata.file_sub_type,
                                                send_frame, sizeof(send_frame));
    if (frame_len < 0) {
        LOG_ERROR("构造重构启动命令失败");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    /* 发送命令(不等待立即响应) */
    int ret = uart_rs422_send_frame(task->uart_client, send_frame, frame_len);
    if (ret != SUCCESS) {
        LOG_ERROR("发送重构启动命令失败");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }

    LOG_INFO("重构启动命令已发送,开始轮询重构状态");
    task->last_activity = time(NULL);
    transition_state(task, FPGA_INJ_STATE_RECONFIG_POLLING);
    return SUCCESS;
}

/* 处理重构轮询状态 */
static int handle_reconfig_polling(fpga_injection_task_t *task)
{
    /* 检查超时 */
    if ((time(NULL) - task->last_activity) > FPGA_RECONFIG_TIMEOUT_SEC) {
        LOG_ERROR("重构超时(10分钟)");
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_TIMEOUT;
    }

    /* 构造重构查询命令 */
    uint8_t send_frame[256];
    int frame_len = rs422_build_reconfig_query(send_frame, sizeof(send_frame));
    if (frame_len < 0) {
        LOG_ERROR("构造重构查询命令失败");
        return ERROR_GENERAL;
    }

    /* 发送并等待响应 */
    uint8_t recv_frame[256];
    uint32_t recv_len;
    int ret = uart_rs422_send_and_wait(task->uart_client,
                                       send_frame, frame_len,
                                       recv_frame, sizeof(recv_frame),
                                       &recv_len, RS422_CMD_RECONFIG_ACK);

    if (ret != SUCCESS) {
        LOG_WARN("发送重构查询命令失败,继续轮询");
        sleep(FPGA_RECONFIG_POLL_INTERVAL_SEC);
        return ERROR_GENERAL;
    }

    /* 解析响应 */
    uint16_t apid, cmd_code;
    const uint8_t *payload;
    uint32_t payload_len;
    ret = rs422_decode_frame(recv_frame, recv_len, &apid, &cmd_code, &payload, &payload_len);
    if (ret != SUCCESS) {
        LOG_ERROR("解码重构应答失败");
        sleep(FPGA_RECONFIG_POLL_INTERVAL_SEC);
        return ERROR_GENERAL;
    }

    uint8_t result;
    ret = rs422_parse_reconfig_ack(payload, payload_len, &result);
    if (ret != SUCCESS) {
        LOG_ERROR("解析重构应答失败");
        sleep(FPGA_RECONFIG_POLL_INTERVAL_SEC);
        return ERROR_GENERAL;
    }

    LOG_INFO("重构状态: %s (0x%02X)",
             rs422_get_result_desc(RS422_CMD_RECONFIG_ACK, result), result);

    if (result == RS422_RECONFIG_SUCCESS) {
        LOG_INFO("FPGA重构成功!");
        transition_state(task, FPGA_INJ_STATE_COMPLETED);
        return SUCCESS;
    } else if (result == RS422_RECONFIG_IN_PROGRESS) {
        LOG_INFO("FPGA重构中,继续轮询...");
        sleep(FPGA_RECONFIG_POLL_INTERVAL_SEC);
        return SUCCESS;
    } else {
        LOG_ERROR("FPGA重构失败: result=0x%02X", result);
        transition_state(task, FPGA_INJ_STATE_FAILED);
        return ERROR_GENERAL;
    }
}
