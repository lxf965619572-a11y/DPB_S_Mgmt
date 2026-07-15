#include "phased_array_calib.h"
#include "tcp_client.h"
#include "logger.h"
#include <string.h>
#include "fpga_handler.h"

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;

/* 模块初始化标志 */
static int g_calib_initialized = 0;

/**
 * 初始化相控阵校准模块
 */
int phased_array_calib_init(void)
{
    if (g_calib_initialized) {
        LOG_WARN("Phased array calibration module already initialized");
        return SUCCESS;
    }

    LOG_INFO("Initializing phased array calibration module...");
    g_calib_initialized = 1;
    LOG_INFO("Phased array calibration module initialized successfully");

    return SUCCESS;
}

/**
 * 清理相控阵校准模块
 */
void phased_array_calib_cleanup(void)
{
    if (!g_calib_initialized) {
        return;
    }

    LOG_INFO("Cleaning up phased array calibration module...");
    g_calib_initialized = 0;
    LOG_INFO("Phased array calibration module cleaned up");
}

/**
 * 发送相控阵校准指示应答
 */
static int send_phased_array_calib_ack(const cpri_msg_header_t *req_header, uint8_t result)
{
    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    /* 填充消息头 */
    response.header.msg_id = MSG_PHASED_ARRAY_CALIB_IND_ACK;
    response.header.paau_id = req_header->paau_id;
    response.header.bbu_id = req_header->bbu_id;
    response.header.port_num = req_header->port_num;
    response.header.serial_num = req_header->serial_num;

    /* 构造应答 IE (IE 761) */
    uint16_t ie_type = IE_PHASED_ARRAY_CALIB_ACK;
    uint16_t ie_len = 4 + sizeof(ie_phased_array_calib_ack_t);  /* IE头(4) + 数据(1) */

    /* 使用栈上静态缓冲区 */
    uint8_t payload_buffer[16];
    response.payload_len = ie_len;
    response.payload = payload_buffer;

    /* 填充 IE 头 */
    uint32_t offset = 0;
    memcpy(response.payload + offset, &ie_type, 2);
    offset += 2;
    memcpy(response.payload + offset, &ie_len, 2);
    offset += 2;

    /* 填充 IE 数据 */
    ie_phased_array_calib_ack_t ack_data;
    ack_data.result = result;
    memcpy(response.payload + offset, &ack_data, sizeof(ie_phased_array_calib_ack_t));

    /* 编码并发送 */
    uint8_t buffer[256];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent phased array calibration ack: result=%u (0=success), serial=%u",
                 result, response.header.serial_num);
    } else {
        LOG_ERROR("Failed to encode phased array calibration ack");
    }

    /* 无需 cpri_free_message，使用的是栈缓冲区 */
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

/**
 * 处理相控阵校准指示请求
 */
int handle_phased_array_calib_request(const cpri_msg_header_t *req_header,
                                      const ie_phased_array_calib_ind_t *req_ie)
{
    if (!g_calib_initialized) {
        LOG_ERROR("Phased array calibration module not initialized");
        return ERROR_NOT_INITIALIZED;
    }

    if (!req_header) {
        LOG_ERROR("Invalid parameter");
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("========================================");
    LOG_INFO("Phased Array Calibration Request Received");
    LOG_INFO("========================================");
    LOG_INFO("Serial Number: %u", req_header->serial_num);

    /* 默认返回成功 */
    uint8_t result = CALIB_RESULT_SUCCESS;

    /* 向FPGA发送校准命令 */
    LOG_INFO("Forwarding calibration command to FPGA");

    /* 发送校准命令到FPGA
     * calib_params参数根据协议定义，这里使用0表示默认校准参数
     * 如果协议有具体定义，可以从req_ie中解析参数
     */
    int fpga_ret = fpga_send_calibration(0);

    if (fpga_ret != SUCCESS) {
        LOG_ERROR("Failed to send calibration command to FPGA");
        result = CALIB_RESULT_FAILURE;
    } else {
        LOG_INFO("Calibration command sent to FPGA successfully");
        result = CALIB_RESULT_SUCCESS;
    }

    LOG_INFO("Calibration result: %s", result == CALIB_RESULT_SUCCESS ? "SUCCESS" : "FAILURE");

    /* 发送应答 */
    int ret = send_phased_array_calib_ack(req_header, result);

    LOG_INFO("========================================");

    return ret;
}
