#include <string.h>
#include <time.h>
#include "transparent_msg.h"
#include "tcp_client.h"
#include "cpri_protocol.h"
#include "logger.h"

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;

/* 模块初始化标志 */
static int g_transparent_initialized = 0;

/**
 * 初始化透传消息模块
 */
int transparent_msg_init(void)
{
    if (g_transparent_initialized) {
        LOG_WARN("Transparent message module already initialized");
        return SUCCESS;
    }

    LOG_INFO("Initializing transparent message module...");
    g_transparent_initialized = 1;
    LOG_INFO("Transparent message module initialized successfully");

    return SUCCESS;
}

/**
 * 清理透传消息模块
 */
void transparent_msg_cleanup(void)
{
    if (!g_transparent_initialized) {
        return;
    }

    LOG_INFO("Cleaning up transparent message module...");
    g_transparent_initialized = 0;
    LOG_INFO("Transparent message module cleaned up");
}

/**
 * 检查消息ID是否为透传消息
 */
bool is_transparent_message(uint16_t msg_id)
{
    return (msg_id >= MSG_TRANSPARENT_PAAU_TO_BBU_BASE &&
            msg_id <= MSG_TRANSPARENT_PAAU_TO_BBU_10) ||
           (msg_id >= MSG_TRANSPARENT_BBU_TO_PAAU_BASE &&
            msg_id <= MSG_TRANSPARENT_BBU_TO_PAAU_10);
}

/**
 * 获取透传目标名称
 */
const char* get_transparent_target_name(uint8_t target_id)
{
    switch (target_id) {
        case TRANSPARENT_TARGET_OMC:
            return "OMC";
        case TRANSPARENT_TARGET_BBU_DEBUG:
            return "BBU_DEBUG";
        default:
            return "UNKNOWN";
    }
}

/**
 * 获取消息方向字符串
 */
static const char* get_message_direction(uint16_t msg_id)
{
    if (msg_id >= MSG_TRANSPARENT_PAAU_TO_BBU_BASE &&
        msg_id <= MSG_TRANSPARENT_PAAU_TO_BBU_10) {
        return "PAAU->BBU";
    } else if (msg_id >= MSG_TRANSPARENT_BBU_TO_PAAU_BASE &&
               msg_id <= MSG_TRANSPARENT_BBU_TO_PAAU_10) {
        return "BBU->PAAU";
    }
    return "UNKNOWN";
}

/**
 * 解析透传消息
 */
int transparent_msg_parse(uint16_t msg_id, const uint8_t *payload,
                         uint32_t payload_len, transparent_parse_result_t *result)
{
    if (!payload || !result) {
        LOG_ERROR("Invalid parameters for transparent message parse");
        return ERROR_INVALID_PARAM;
    }

    /* 初始化结果 */
    memset(result, 0, sizeof(*result));
    result->msg_id = msg_id;

    /* 获取当前时间戳 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(result->timestamp, sizeof(result->timestamp),
             "%Y-%m-%d %H:%M:%S", tm_info);

    /* 检查最小长度：至少包含目标ID (1字节) */
    if (payload_len < 1) {
        LOG_ERROR("Transparent message payload too short: %u bytes", payload_len);
        result->parse_status = ERROR_INVALID_PARAM;
        return ERROR_INVALID_PARAM;
    }

    /* 解析透传目标 */
    result->data.target_id = payload[0];

    /* 解析透传内容（如果有） */
    if (payload_len > 1) {
        result->data.content_len = (payload_len - 1 > TRANSPARENT_CONTENT_MAX_LEN) ?
                                   TRANSPARENT_CONTENT_MAX_LEN : (payload_len - 1);
        memcpy(result->data.content, payload + 1, result->data.content_len);
    } else {
        result->data.content_len = 0;
    }

    result->parse_status = SUCCESS;
    LOG_DEBUG("Transparent message parsed successfully: target=%u, content_len=%u",
             result->data.target_id, result->data.content_len);

    return SUCCESS;
}

/**
 * 记录结构化审计日志
 */
static void log_transparent_audit(const transparent_parse_result_t *result)
{
    LOG_INFO("========================================");
    LOG_INFO("Transparent Message Audit Log");
    LOG_INFO("========================================");
    LOG_INFO("Message ID:    %u (%s)", result->msg_id,
             get_message_direction(result->msg_id));
    LOG_INFO("Timestamp:     %s", result->timestamp);
    LOG_INFO("Source IP:     %s", result->source_ip[0] ? result->source_ip : "N/A");
    LOG_INFO("Parse Status:  %d %s", result->parse_status,
             result->parse_status == SUCCESS ? "(SUCCESS)" : "(FAILED)");
    LOG_INFO("----------------------------------------");
    LOG_INFO("Target ID:     %u (%s)", result->data.target_id,
             get_transparent_target_name(result->data.target_id));
    LOG_INFO("Content Length: %u bytes", result->data.content_len);

    /* 打印内容的十六进制dump（前64字节） */
    if (result->data.content_len > 0) {
        LOG_INFO("Content (hex, first 64 bytes):");
        char hex_buf[256];
        int dump_len = (result->data.content_len > 64) ? 64 : result->data.content_len;
        for (int i = 0; i < dump_len; i += 16) {
            int line_len = (dump_len - i > 16) ? 16 : (dump_len - i);
            char *p = hex_buf;
            p += sprintf(p, "  %04X: ", i);
            for (int j = 0; j < line_len; j++) {
                p += sprintf(p, "%02X ", result->data.content[i + j]);
            }
            LOG_INFO("%s", hex_buf);
        }
        if (result->data.content_len > 64) {
            LOG_INFO("  ... (%u more bytes)", result->data.content_len - 64);
        }
    }
    LOG_INFO("========================================");
}

/**
 * 处理BBU到PAAU的透传消息
 */
int handle_transparent_bbu_to_paau(uint16_t msg_id,
                                   const transparent_message_t *trans_msg,
                                   const char *source_ip)
{
    if (!g_transparent_initialized) {
        LOG_ERROR("Transparent message module not initialized");
        return ERROR_NOT_INITIALIZED;
    }

    if (!trans_msg) {
        LOG_ERROR("Invalid transparent message pointer");
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("========================================");
    LOG_INFO("TRANSPARENT MESSAGE RECEIVED (BBU->PAAU)");
    LOG_INFO("========================================");
    LOG_INFO("Message ID:    %u", msg_id);
    LOG_INFO("Target:        %s", get_transparent_target_name(trans_msg->target_id));
    LOG_INFO("Content Length: %u bytes", trans_msg->content_len);
    LOG_INFO("Source IP:     %s", source_ip ? source_ip : "N/A");
    LOG_INFO("========================================");

    /* 根据目标标识进行不同处理 */
    switch (trans_msg->target_id) {
        case TRANSPARENT_TARGET_OMC:
            LOG_INFO("Processing OMC transparent message...");
            /* 这里可以添加OMC相关的处理逻辑 */
            /* 例如：转发给OMC接口、记录到特定日志等 */
            LOG_INFO("OMC message logged (no further action in current implementation)");
            break;

        case TRANSPARENT_TARGET_BBU_DEBUG:
            LOG_INFO("Processing BBU debug transparent message...");
            /* 这里可以添加调试相关的处理逻辑 */
            /* 例如：执行调试命令、输出调试信息等 */
            LOG_INFO("Debug message logged (no further action in current implementation)");
            break;

        default:
            LOG_WARN("Unknown transparent target: %u", trans_msg->target_id);
            break;
    }

    /* 打印内容摘要 */
    if (trans_msg->content_len > 0) {
        LOG_INFO("Content preview (first 32 bytes):");
        char preview[128];
        int preview_len = (trans_msg->content_len > 32) ? 32 : trans_msg->content_len;
        char *p = preview;
        for (int i = 0; i < preview_len; i++) {
            p += sprintf(p, "%02X ", trans_msg->content[i]);
        }
        LOG_INFO("  %s", preview);
        if (trans_msg->content_len > 32) {
            LOG_INFO("  ... (%u more bytes)", trans_msg->content_len - 32);
        }
    }

    LOG_INFO("========================================");
    LOG_INFO("Transparent message processing completed");
    LOG_INFO("========================================");

    /* 自动回复透传消息（原样返回） */
    /* 收到 BBU->PAAU (231-240)，回复 PAAU->BBU (221-230) */
    uint16_t reply_msg_id = MSG_TRANSPARENT_PAAU_TO_BBU_BASE +
                            (msg_id - MSG_TRANSPARENT_BBU_TO_PAAU_BASE);

    LOG_INFO("Auto-replying transparent message: received msgid=%u, reply msgid=%u",
             msg_id, reply_msg_id);

    int ret = send_transparent_paau_to_bbu(reply_msg_id,
                                           trans_msg->target_id,
                                           trans_msg->content,
                                           trans_msg->content_len);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to send transparent reply: ret=%d", ret);
        return ret;
    }

    LOG_INFO("Transparent reply sent successfully");
    return SUCCESS;
}

/**
 * 发送PAAU到BBU的透传消息
 */
int send_transparent_paau_to_bbu(uint16_t msg_id, uint8_t target_id,
                                 const uint8_t *content, uint16_t content_len)
{
    if (!g_transparent_initialized) {
        LOG_ERROR("Transparent message module not initialized");
        return ERROR_NOT_INITIALIZED;
    }

    /* 验证消息ID范围 */
    if (msg_id < MSG_TRANSPARENT_PAAU_TO_BBU_BASE ||
        msg_id > MSG_TRANSPARENT_PAAU_TO_BBU_10) {
        LOG_ERROR("Invalid transparent message ID for PAAU->BBU: %u", msg_id);
        return ERROR_INVALID_PARAM;
    }

    /* 验证内容长度 */
    if (content_len > TRANSPARENT_CONTENT_MAX_LEN) {
        LOG_ERROR("Transparent content too large: %u (max: %u)",
                 content_len, TRANSPARENT_CONTENT_MAX_LEN);
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("Sending transparent message PAAU->BBU: msgid=%u, target=%s, len=%u",
             msg_id, get_transparent_target_name(target_id), content_len);

    /* 使用栈上静态缓冲区，避免动态分配 */
    uint32_t payload_len = 1 + content_len;
    uint8_t payload[TRANSPARENT_CONTENT_MAX_LEN + 1];  /* 最大2049字节 */

    payload[0] = target_id;
    if (content_len > 0 && content) {
        memcpy(payload + 1, content, content_len);
    }

    /* 构造CPRI消息 */
    cpri_message_t message;
    memset(&message, 0, sizeof(message));

    message.header.msg_id = msg_id;
    message.payload = payload;
    message.payload_len = payload_len;

    /* 编码并发送 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&message, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Transparent message sent successfully: %d bytes", len);
        return SUCCESS;
    } else {
        LOG_ERROR("Failed to encode transparent message");
        return ERROR_GENERAL;
    }
}

/**
 * 处理透传消息的通用入口（从msg_handler调用）
 */
int transparent_msg_handle(const cpri_message_t *msg, const char *source_ip)
{
    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid transparent message");
        return ERROR_INVALID_PARAM;
    }

    /* 解析透传消息 */
    transparent_parse_result_t result;
    int ret = transparent_msg_parse(msg->header.msg_id, msg->payload,
                                    msg->payload_len, &result);

    /* 复制来源IP */
    if (source_ip) {
        strncpy(result.source_ip, source_ip, sizeof(result.source_ip) - 1);
    }

    /* 记录审计日志 */
    log_transparent_audit(&result);

    if (ret != SUCCESS) {
        LOG_ERROR("Failed to parse transparent message");
        return ret;
    }

    /* 根据消息方向进行处理 */
    if (msg->header.msg_id >= MSG_TRANSPARENT_BBU_TO_PAAU_BASE &&
        msg->header.msg_id <= MSG_TRANSPARENT_BBU_TO_PAAU_10) {
        /* BBU -> PAAU 方向 */
        return handle_transparent_bbu_to_paau(msg->header.msg_id,
                                             &result.data, source_ip);
    } else {
        /* PAAU -> BBU 方向（不应该接收到，记录警告） */
        LOG_WARN("Received PAAU->BBU transparent message (unexpected direction)");
        return SUCCESS;
    }
}
