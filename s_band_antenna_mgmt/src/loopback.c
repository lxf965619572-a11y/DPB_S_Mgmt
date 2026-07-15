#include <string.h>
#include <time.h>
#include "loopback.h"
#include "tcp_client.h"
#include "cpri_protocol.h"
#include "logger.h"
#include "alarm_manager.h"
#include "fpga_handler.h"

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;

/* 模块初始化标志 */
static int g_loopback_initialized = 0;

/* 端口和波束范围限制 */
#define MAX_PORT_NUMBER     7
#define MAX_BEAM_NUMBER     255

/**
 * 初始化环回模块
 */
int loopback_init(void)
{
    if (g_loopback_initialized) {
        LOG_WARN("Loopback module already initialized");
        return SUCCESS;
    }

    LOG_INFO("Initializing loopback module...");
    g_loopback_initialized = 1;
    LOG_INFO("Loopback module initialized successfully");

    return SUCCESS;
}

/**
 * 清理环回模块
 */
void loopback_cleanup(void)
{
    if (!g_loopback_initialized) {
        return;
    }

    LOG_INFO("Cleaning up loopback module...");
    g_loopback_initialized = 0;
    LOG_INFO("Loopback module cleaned up");
}

/**
 * 获取环回类型字符串
 */
static const char* get_loopback_type_str(uint8_t type)
{
    switch (type) {
        case LOOPBACK_TYPE_OTHER:    return "OTHER";
        case LOOPBACK_TYPE_PHY_PORT: return "PHY_PORT";
        case LOOPBACK_TYPE_BEAM:     return "BEAM";
        case LOOPBACK_TYPE_DISABLE:  return "DISABLE";
        default:                     return "UNKNOWN";
    }
}

/**
 * 验证环回请求合法性
 */
int loopback_validate_request(const ie_loopback_req_t *req)
{
    if (!req) {
        LOG_ERROR("Invalid request pointer");
        return LOOPBACK_ERR_FORMAT;
    }

    /* 验证环回类型 */
    if (req->loopback_type > LOOPBACK_TYPE_DISABLE) {
        LOG_ERROR("Invalid loopback type: %u", req->loopback_type);
        return LOOPBACK_ERR_INVALID_TYPE;
    }

    /* 验证端口号 */
    if (req->port_number > MAX_PORT_NUMBER) {
        LOG_ERROR("Port number out of range: %u (max: %u)",
                 req->port_number, MAX_PORT_NUMBER);
        return LOOPBACK_ERR_INVALID_PORT;
    }

    /* 验证波束号（暂时不限制上限，使用uint8_t范围） */
    /* 如果有具体限制，可以在这里添加 */

    return SUCCESS;
}

/**
 * 解析环回请求消息
 */
int loopback_parse_request(const uint8_t *data, size_t len,
                           loopback_parse_result_t *result)
{
    if (!data || !result) {
        LOG_ERROR("Invalid parameters for loopback parse");
        return LOOPBACK_ERR_FORMAT;
    }

    /* 检查数据长度 */
    if (len < sizeof(ie_loopback_req_t)) {
        LOG_ERROR("Loopback request data too short: %zu bytes (expected: %zu)",
                 len, sizeof(ie_loopback_req_t));
        return LOOPBACK_ERR_FORMAT;
    }

    /* 初始化结果 */
    memset(result, 0, sizeof(*result));
    result->msg_id = MSG_LOOPBACK_REQ;

    /* 获取当前时间戳 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(result->timestamp, sizeof(result->timestamp),
             "%Y-%m-%d %H:%M:%S", tm_info);

    /* 解析IE数据 */
    const ie_loopback_req_t *req = (const ie_loopback_req_t *)data;
    memcpy(&result->request, req, sizeof(ie_loopback_req_t));

    /* 字节序转换 */
    result->request.test_period = le16toh(result->request.test_period);

    /* 验证请求 */
    int ret = loopback_validate_request(&result->request);
    result->parse_status = ret;

    if (ret != SUCCESS) {
        LOG_ERROR("Loopback request validation failed: error=%d", ret);
    } else {
        LOG_DEBUG("Loopback request parsed successfully");
    }

    return ret;
}

/**
 * 记录结构化审计日志
 */
static void log_loopback_audit(const loopback_parse_result_t *result)
{
    LOG_INFO("========================================");
    LOG_INFO("Loopback Message Audit Log");
    LOG_INFO("========================================");
    LOG_INFO("Message ID:    %u", result->msg_id);
    LOG_INFO("Timestamp:     %s", result->timestamp);
    LOG_INFO("Source IP:     %s", result->source_ip[0] ? result->source_ip : "N/A");
    LOG_INFO("Parse Status:  %d %s", result->parse_status,
             result->parse_status == SUCCESS ? "(SUCCESS)" : "(FAILED)");
    LOG_INFO("----------------------------------------");
    LOG_INFO("Loopback Type: %u (%s)", result->request.loopback_type,
             get_loopback_type_str(result->request.loopback_type));
    LOG_INFO("Test Period:   %u ms", result->request.test_period);
    LOG_INFO("Port Number:   %u", result->request.port_number);
    LOG_INFO("Beam Number:   %u", result->request.beam_number);
    LOG_INFO("========================================");
}

/**
 * 触发解析失败告警
 */
static void trigger_loopback_parse_alarm(int error_code, const char *source_ip)
{
    const char *error_desc = "Unknown error";
    uint32_t alarm_code = 60001;  /* 环回解析失败基础告警码 */

    switch (error_code) {
        case LOOPBACK_ERR_FORMAT:
            error_desc = "Loopback message format error";
            alarm_code = 60001;
            break;
        case LOOPBACK_ERR_PERMISSION:
            error_desc = "Loopback permission denied";
            alarm_code = 60002;
            break;
        case LOOPBACK_ERR_OUT_OF_RANGE:
            error_desc = "Loopback field out of range";
            alarm_code = 60003;
            break;
        case LOOPBACK_ERR_INVALID_TYPE:
            error_desc = "Invalid loopback type";
            alarm_code = 60004;
            break;
        case LOOPBACK_ERR_INVALID_PORT:
            error_desc = "Invalid port number";
            alarm_code = 60005;
            break;
        case LOOPBACK_ERR_INVALID_BEAM:
            error_desc = "Invalid beam number";
            alarm_code = 60006;
            break;
    }

    char alarm_info[256];
    snprintf(alarm_info, sizeof(alarm_info),
             "Loopback parse failed from %s: %s (error=%d)",
             source_ip ? source_ip : "unknown", error_desc, error_code);

    LOG_ERROR("%s", alarm_info);

    /* 触发告警 */
    alarm_trigger(alarm_code, 0, error_desc);
}

/**
 * 发送环回响应
 */
int send_loopback_response(const cpri_msg_header_t *req_header,
                           const ie_loopback_req_t *req_ie,
                           uint8_t result)
{
    if (!req_header || !req_ie) {
        LOG_ERROR("Invalid parameter");
        return ERROR_INVALID_PARAM;
    }

    /* 构造响应IE */
    uint8_t payload_buffer[128];
    uint32_t offset = 0;

    /* 写入IE头 */
    uint16_t ie_type = IE_LOOPBACK_RSP;  /* 0x0583 (1411) */
    uint16_t ie_length = 4 + sizeof(ie_loopback_rsp_t);  /* IE头(4) + 数据(4) = 8字节 */
    memcpy(payload_buffer + offset, &ie_type, 2);
    offset += 2;
    memcpy(payload_buffer + offset, &ie_length, 2);
    offset += 2;

    /* 写入IE数据 */
    ie_loopback_rsp_t rsp_ie;
    memset(&rsp_ie, 0, sizeof(rsp_ie));

    rsp_ie.loopback_type = req_ie->loopback_type;
    rsp_ie.port_number = req_ie->port_number;
    rsp_ie.beam_number = req_ie->beam_number;
    rsp_ie.result = result;
    memcpy(payload_buffer + offset, &rsp_ie, sizeof(ie_loopback_rsp_t));
    offset += sizeof(ie_loopback_rsp_t);

    /* 构造CPRI消息 */
    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    response.header.msg_id = MSG_LOOPBACK_REQ_RSP;
    response.header.paau_id = req_header->paau_id;
    response.header.bbu_id = req_header->bbu_id;
    response.header.port_num = req_header->port_num;
    response.header.serial_num = req_header->serial_num;
    response.payload_len = offset;
    response.payload = payload_buffer;

    /* 编码并发送 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Loopback response sent: type=%u, port=%u, beam=%u, result=%u",
                 rsp_ie.loopback_type, rsp_ie.port_number,
                 rsp_ie.beam_number, rsp_ie.result);
    } else {
        LOG_ERROR("Failed to encode loopback response");
    }

    /* 无需free，使用的是栈缓冲区 */
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

/**
 * 处理环回请求
 */
int handle_loopback_request(const cpri_msg_header_t *req_header,
                            const ie_loopback_req_t *req_ie,
                            const char *source_ip)
{
    if (!g_loopback_initialized) {
        LOG_ERROR("Loopback module not initialized");
        return ERROR_NOT_INITIALIZED;
    }

    if (!req_header || !req_ie) {
        LOG_ERROR("Invalid parameter");
        return ERROR_INVALID_PARAM;
    }

    /* 解析和验证请求 */
    loopback_parse_result_t parse_result;
    memset(&parse_result, 0, sizeof(parse_result));

    /* 复制来源IP */
    if (source_ip) {
        strncpy(parse_result.source_ip, source_ip,
                sizeof(parse_result.source_ip) - 1);
    }

    /* 解析请求 */
    int ret = loopback_parse_request((const uint8_t *)req_ie,
                                     sizeof(*req_ie), &parse_result);

    /* 记录审计日志 */
    log_loopback_audit(&parse_result);

    /* 如果解析失败，触发告警并返回错误响应 */
    if (ret != SUCCESS) {
        trigger_loopback_parse_alarm(ret, source_ip);
        send_loopback_response(req_header, req_ie, LOOPBACK_RESULT_NOT_SUPPORT);
        return ret;
    }

    /* 现阶段仅记录日志，不执行实际业务 */
    LOG_INFO("========================================");
    LOG_INFO("LOOPBACK RECEIVED - Echo Mode");
    LOG_INFO("========================================");
    LOG_INFO("Type:        %s", get_loopback_type_str(req_ie->loopback_type));
    LOG_INFO("Test Period: %u ms", le16toh(req_ie->test_period));
    LOG_INFO("Port:        %u", req_ie->port_number);
    LOG_INFO("Beam:        %u", req_ie->beam_number);
    LOG_INFO("Source IP:   %s", source_ip ? source_ip : "N/A");
    LOG_INFO("========================================");
    LOG_INFO("Payload Echo:");
    LOG_INFO("  loopback_type = 0x%02X", req_ie->loopback_type);
    LOG_INFO("  test_period   = 0x%04X", req_ie->test_period);
    LOG_INFO("  port_number   = 0x%02X", req_ie->port_number);
    LOG_INFO("  beam_number   = 0x%02X", req_ie->beam_number);
    LOG_INFO("========================================");

    uint8_t loopback_result = LOOPBACK_RESULT_SUCCESS;

    int fpga_ret = fpga_send_loopback(req_ie->loopback_type);

    LOG_INFO("Forwarding loopback command to FPGA: type=%u", req_ie->loopback_type);

    if (fpga_ret != SUCCESS) {
        LOG_ERROR("Failed to send loopback command to FPGA");
        loopback_result = LOOPBACK_RESULT_NOT_SUPPORT;
    } else {
        LOG_INFO("Loopback command sent to FPGA successfully");
        loopback_result = LOOPBACK_RESULT_SUCCESS;
    }

    /* 发送成功响应 */
    ret = send_loopback_response(req_header, req_ie, loopback_result);

    return ret;
}
