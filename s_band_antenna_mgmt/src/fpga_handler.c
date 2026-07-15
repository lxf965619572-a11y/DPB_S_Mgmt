#include "fpga_handler.h"
#include "uart_client.h"
#include "alarm_manager.h"
#include "logger.h"

/* 全局FPGA状态管理器 */
fpga_state_manager_t g_fpga_state;

/* 外部UART客户端引用（在main.c中定义） */
extern uart_client_t g_uart_client;

int fpga_handler_init(void)
{
    memset(&g_fpga_state, 0, sizeof(g_fpga_state));
    g_fpga_state.status_valid = false;

    if (pthread_mutex_init(&g_fpga_state.status_mutex, NULL) != 0) {
        LOG_ERROR("Failed to init FPGA state mutex");
        return ERROR_GENERAL;
    }

    LOG_INFO("FPGA handler initialized");
    return SUCCESS;
}

int fpga_handler_on_status_update(const fpga_status_frame_t *status)
{
    if (!status) {
        return ERROR_INVALID_PARAM;
    }

    /* 立即创建本地副本，最小化对传入指针的依赖 */
    fpga_status_frame_t local_copy;
    memcpy(&local_copy, status, sizeof(fpga_status_frame_t));

    pthread_mutex_lock(&g_fpga_state.status_mutex);

    /* 使用本地副本更新状态 */
    memcpy(&g_fpga_state.current_status, &local_copy, sizeof(fpga_status_frame_t));
    g_fpga_state.status_valid = true;
    g_fpga_state.last_update_time = time(NULL);

    pthread_mutex_unlock(&g_fpga_state.status_mutex);

    /* 暂时禁用所有日志输出，测试是否是日志导致段错误 */
    #if 0
    /* 解码本振频率 */
    uint32_t lo_freq_khz = fpga_decode_frequency(status->data.lo_frequency);

    LOG_INFO("FPGA status updated:");
    LOG_INFO("  Link: main_fiber=%u, backup_fiber=%u, link_flag=0x%02X",
             status->data.main_fiber_num, status->data.backup_fiber_num, status->data.link_success_flag);
    LOG_INFO("  Beams: count=%u, current_power=%u (1/256dBm)",
             status->data.nr_beam_count, status->data.current_output_power);
    LOG_INFO("  LO: freq=%u kHz, lock_status=%u", lo_freq_khz, status->data.lo_lock_status);
    LOG_INFO("  Clock: sync_status=%u", status->data.clock_sync_status);
    LOG_INFO("  CPRI: Toffset=%u, T2a=%u, Ta3=%u", status->data.toffset, status->data.t2a, status->data.ta3);
    LOG_INFO("  Temp: T1=%d°C, T2=%d°C, T3=%d°C", status->data.temp1, status->data.temp2, status->data.temp3);
    #endif
    alarm_periodic_check(&local_copy);

    return SUCCESS;
}

int fpga_handler_get_status(fpga_status_frame_t *status)
{
    if (!status) {
        return ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_fpga_state.status_mutex);

    if (!g_fpga_state.status_valid) {
        pthread_mutex_unlock(&g_fpga_state.status_mutex);
        return ERROR_GENERAL;
    }

    memcpy(status, &g_fpga_state.current_status, sizeof(fpga_status_frame_t));

    pthread_mutex_unlock(&g_fpga_state.status_mutex);

    return SUCCESS;
}

/* 获取光纤端口信息 */
int fpga_handler_get_fiber_ports(uint8_t *main_port, uint8_t *sub_port)
{
    if (!main_port || !sub_port) {
        return ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_fpga_state.status_mutex);

    if (!g_fpga_state.status_valid) {
        pthread_mutex_unlock(&g_fpga_state.status_mutex);
        LOG_WARN("FPGA status not available, cannot get fiber ports");
        return ERROR_GENERAL;
    }

    /* 从FPGA状态中读取光纤端口信息 */
    *main_port = g_fpga_state.current_status.data.main_fiber_num;
    *sub_port = g_fpga_state.current_status.data.backup_fiber_num;

    pthread_mutex_unlock(&g_fpga_state.status_mutex);

    LOG_DEBUG("Retrieved fiber ports from FPGA: main=%u, backup=%u", *main_port, *sub_port);

    return SUCCESS;
}

/* 发送CPRI工作模式配置 */
int fpga_send_cpri_mode(uint32_t mode)
{
    fpga_message_t msg;
    uint32_t payload = HTONL(mode);

    msg.msg_id = FPGA_MSG_CPRI_CONFIG;
    msg.payload = (uint8_t*)&payload;
    msg.payload_len = sizeof(payload);

    uint8_t buffer[32];
    int len = fpga_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        LOG_INFO("Sending CPRI mode: 0x%08X", mode);
        return uart_client_send(&g_uart_client, buffer, len);
    }

    return ERROR_GENERAL;
}

/* 发送CPRI配置 */
int fpga_send_cpri_config(uint32_t config)
{
    fpga_message_t msg;
    uint32_t payload = HTONL(config);

    msg.msg_id = FPGA_MSG_CPRI_MODE;
    msg.payload = (uint8_t*)&payload;
    msg.payload_len = sizeof(payload);

    uint8_t buffer[32];
    int len = fpga_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        LOG_INFO("Sending CPRI config: 0x%08X", config);
        return uart_client_send(&g_uart_client, buffer, len);
    }

    return ERROR_GENERAL;
}

/* 发送波束数配置 */
int fpga_send_beam_num(uint8_t cmd_beam_num, uint8_t service_beam_num)
{
    fpga_message_t msg;
    uint8_t payload[2];

    /* 先发送指令波束数 */
    msg.msg_id = FPGA_MSG_CMD_BEAM_NUM;
    payload[0] = cmd_beam_num;
    msg.payload = payload;
    msg.payload_len = 1;

    uint8_t buffer[32];
    int len = fpga_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        LOG_INFO("Sending command beam num: %u", cmd_beam_num);
        uart_client_send(&g_uart_client, buffer, len);
    }

    /* 再发送业务波束数 */
    msg.msg_id = FPGA_MSG_SERVICE_BEAM_NUM;
    payload[0] = service_beam_num;
    msg.payload = payload;
    msg.payload_len = 1;

    len = fpga_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        LOG_INFO("Sending service beam num: %u", service_beam_num);
        return uart_client_send(&g_uart_client, buffer, len);
    }

    return ERROR_GENERAL;
}

/* 发送相控阵复位 */
int fpga_send_phase_restore(uint32_t restore_type)
{
    fpga_message_t msg;
    uint32_t payload = HTONL(restore_type);

    msg.msg_id = FPGA_MSG_PHASE_RESTORE;
    msg.payload = (uint8_t*)&payload;
    msg.payload_len = sizeof(payload);

    uint8_t buffer[32];
    int len = fpga_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        LOG_INFO("Sending phase restore: 0x%08X", restore_type);
        return uart_client_send(&g_uart_client, buffer, len);
    }

    return ERROR_GENERAL;
}

/* 发送校准指示 */
int fpga_send_calibration(uint32_t calib_params)
{
    fpga_message_t msg;
    uint32_t payload = HTONL(calib_params);

    msg.msg_id = FPGA_MSG_CALIBRATION;
    msg.payload = (uint8_t*)&payload;
    msg.payload_len = sizeof(payload);

    uint8_t buffer[32];
    int len = fpga_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        LOG_INFO("Sending calibration: 0x%08X", calib_params);
        return uart_client_send(&g_uart_client, buffer, len);
    }

    return ERROR_GENERAL;
}

/* 发送环回控制 */
int fpga_send_loopback(uint8_t loopback_type)
{
    fpga_message_t msg;
    uint8_t payload = loopback_type;

    msg.msg_id = FPGA_MSG_LOOPBACK;
    msg.payload = &payload;
    msg.payload_len = sizeof(payload);

    uint8_t buffer[32];
    int len = fpga_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        LOG_INFO("Sending loopback: %u", loopback_type);
        return uart_client_send(&g_uart_client, buffer, len);
    }

    return ERROR_GENERAL;
}

/* 发送频率与带宽配置 */
int fpga_send_freq_band(uint32_t dl_freq, uint32_t ul_freq, uint32_t bandwidth)
{
    fpga_message_t msg;
    uint8_t payload[12];

    /* 编码频率（使用压缩算法） */
    uint32_t dl_encoded = fpga_encode_frequency(dl_freq);
    uint32_t ul_encoded = fpga_encode_frequency(ul_freq);

    /* 大端序 */
    uint32_t dl_be = HTONL(dl_encoded);
    uint32_t ul_be = HTONL(ul_encoded);
    uint32_t bw_be = HTONL(bandwidth);

    memcpy(payload, &dl_be, 4);
    memcpy(payload + 4, &bw_be, 4);
    memcpy(payload + 8, &ul_be, 4);

    msg.msg_id = FPGA_MSG_FREQ_BAND;
    msg.payload = payload;
    msg.payload_len = sizeof(payload);

    uint8_t buffer[64];
    int len = fpga_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        LOG_INFO("Sending freq/band: DL=%u kHz, UL=%u kHz, BW=%u MHz", dl_freq, ul_freq, bandwidth);
        return uart_client_send(&g_uart_client, buffer, len);
    }

    return ERROR_GENERAL;
}

/* 发送系统时间 */
int fpga_send_system_time(void)
{
    fpga_message_t msg;
    uint8_t payload[6];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    payload[0] = tm_info->tm_sec;
    payload[1] = tm_info->tm_min;
    payload[2] = tm_info->tm_hour;
    payload[3] = tm_info->tm_mday;
    payload[4] = tm_info->tm_mon + 1;
    payload[5] = (tm_info->tm_year + 1900) & 0xFF;

    msg.msg_id = FPGA_MSG_SYSTEM_TIME;
    msg.payload = payload;
    msg.payload_len = sizeof(payload);

    uint8_t buffer[32];
    int len = fpga_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        LOG_INFO("Sending system time: %04d-%02d-%02d %02d:%02d:%02d",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
        return uart_client_send(&g_uart_client, buffer, len);
    }

    return ERROR_GENERAL;
}

/* 发送发射控制 */
int fpga_send_tx_control(uint8_t tx_enable)
{
    fpga_message_t msg;
    uint8_t payload = tx_enable;

    msg.msg_id = FPGA_MSG_TX_CONTROL;
    msg.payload = &payload;
    msg.payload_len = sizeof(payload);

    uint8_t buffer[32];
    int len = fpga_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        LOG_INFO("Sending TX control: %s", tx_enable ? "ON" : "OFF");
        return uart_client_send(&g_uart_client, buffer, len);
    }

    return ERROR_GENERAL;
}

/* 设置CPRI工作模式 (用于参数配置) */
int fpga_handler_set_work_mode(uint32_t work_mode)
{
    return fpga_send_cpri_mode(work_mode);
}

void fpga_handler_destroy(void)
{
    pthread_mutex_destroy(&g_fpga_state.status_mutex);
    LOG_INFO("FPGA handler destroyed");
}
