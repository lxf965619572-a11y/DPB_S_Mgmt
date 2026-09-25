#include <string.h>
#include <time.h>
#include "transparent_msg.h"
#include "tcp_client.h"
#include "cpri_protocol.h"
#include "fpga_handler.h"
#include "config.h"
#include "logger.h"

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;

/* 模块初始化标志 */
static int g_transparent_initialized = 0;

/* ==================== 透传通道开关（可配置） ====================
 * 透传通道把 BBU 送来的内容原样写到 FPGA：不过帧封装、无命令码校验、无参数范围检查，
 * 是全项目权限最高的入站路径，本应只在调试/产测时启用。
 *
 * 配置项（config/antenna_mgmt.conf）：
 *   TRANSPARENT_ENABLE     1=启用（默认，保持现有行为）  0=直接拒绝该通道
 *   TRANSPARENT_AUTO_REPLY 1=收到 BBU->PAAU 后自动回发（默认）  0=只收不回
 * ============================================================== */
static int  g_transparent_enable      = 1;
static int  g_transparent_auto_reply  = 1;
static bool g_transparent_cfg_loaded  = false;

static void transparent_cfg_load(void)
{
    if (g_transparent_cfg_loaded) {
        return;
    }
    g_transparent_enable     = config_get_int("TRANSPARENT_ENABLE", 1);
    g_transparent_auto_reply = config_get_int("TRANSPARENT_AUTO_REPLY", 1);
    g_transparent_cfg_loaded = true;

    LOG_INFO("Transparent channel config: enable=%d (0=拒绝该通道), auto_reply=%d",
             g_transparent_enable, g_transparent_auto_reply);
}

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
bool is_transparent_message(uint32_t msg_id)
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
static const char* get_message_direction(uint32_t msg_id)
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
 * 计算内容有效长度：去除尾部补零填充
 * 透传内容（如完整FPGA帧 EB90...5555）在IE中按1024字节定长发送，
 * 实际有效数据之后的字节均为0x00填充，转发前需要去掉。
 */
static uint16_t calc_effective_len(const uint8_t *content, uint16_t content_len)
{
    uint16_t len = content_len;
    while (len > 0 && content[len - 1] == 0x00) {
        len--;
    }
    return len;
}

/**
 * 解析透传消息
 *
 * payload 为 IE 结构（小端）：
 *   IE 1601 透传目标: IE头(4字节) + 目标ID(1字节)
 *   IE 1602 透传内容: IE头(4字节) + 内容(IE长度-4字节，定长1024字节，尾部补零)
 */
int transparent_msg_parse(uint32_t msg_id, const uint8_t *payload,
                         uint32_t payload_len, transparent_parse_result_t *result)
{
    if (!payload || !result) {
        LOG_ERROR("Invalid parameters for transparent message parse");
        return ERROR_INVALID_PARAM;
    }

    /* 初始化结果 */
    memset(result, 0, sizeof(*result));
    result->msg_id = msg_id;
    result->data.target_id = TRANSPARENT_TARGET_OMC;

    /* 获取当前时间戳 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(result->timestamp, sizeof(result->timestamp),
             "%Y-%m-%d %H:%M:%S", tm_info);

    /* 至少包含一个IE头（4字节） */
    if (payload_len < sizeof(cpri_ie_header_t)) {
        LOG_ERROR("Transparent message payload too short for IE header: %u bytes",
                  payload_len);
        result->parse_status = ERROR_INVALID_PARAM;
        return ERROR_INVALID_PARAM;
    }

    bool has_target_ie = false;
    bool has_content_ie = false;
    uint32_t offset = 0;

    /* 遍历所有IE */
    while (offset + sizeof(cpri_ie_header_t) <= payload_len) {
        cpri_ie_header_t ie_hdr;
        memcpy(&ie_hdr, payload + offset, sizeof(cpri_ie_header_t));

        /* IE长度包含IE头本身，且不能小于IE头长度 */
        if (ie_hdr.ie_length < sizeof(cpri_ie_header_t)) {
            LOG_ERROR("Invalid transparent IE length: type=%u, len=%u at offset=%u",
                      ie_hdr.ie_type, ie_hdr.ie_length, offset);
            result->parse_status = ERROR_INVALID_PARAM;
            return ERROR_INVALID_PARAM;
        }
        if (offset + ie_hdr.ie_length > payload_len) {
            LOG_ERROR("Transparent IE exceeds payload: type=%u, len=%u, offset=%u, payload_len=%u",
                      ie_hdr.ie_type, ie_hdr.ie_length, offset, payload_len);
            result->parse_status = ERROR_INVALID_PARAM;
            return ERROR_INVALID_PARAM;
        }

        const uint8_t *ie_data = payload + offset + sizeof(cpri_ie_header_t);
        uint32_t ie_data_len = ie_hdr.ie_length - sizeof(cpri_ie_header_t);

        switch (ie_hdr.ie_type) {
            case IE_TRANSPARENT_TARGET:
                if (ie_data_len >= 1) {
                    result->data.target_id = ie_data[0];
                    has_target_ie = true;
                }
                break;

            case IE_TRANSPARENT_CONTENT: {
                uint32_t copy_len = (ie_data_len > TRANSPARENT_CONTENT_MAX_LEN) ?
                                    TRANSPARENT_CONTENT_MAX_LEN : ie_data_len;
                memcpy(result->data.content, ie_data, copy_len);
                result->data.content_len = (uint16_t)copy_len;
                has_content_ie = true;
                break;
            }

            default:
                LOG_WARN("Unknown transparent IE type: %u (len=%u), skipped",
                         ie_hdr.ie_type, ie_hdr.ie_length);
                break;
        }

        offset += ie_hdr.ie_length;
    }

    if (!has_content_ie) {
        LOG_ERROR("Transparent message missing content IE (%u)", IE_TRANSPARENT_CONTENT);
        result->parse_status = ERROR_INVALID_PARAM;
        return ERROR_INVALID_PARAM;
    }

    if (!has_target_ie) {
        LOG_WARN("Transparent message missing target IE (%u), use default target=%u",
                 IE_TRANSPARENT_TARGET, result->data.target_id);
    }

    /* 去除尾部补零，得到有效内容长度 */
    result->data.effective_len = calc_effective_len(result->data.content,
                                                    result->data.content_len);

    result->parse_status = SUCCESS;
    LOG_DEBUG("Transparent message parsed: target=%u, content_len=%u, effective_len=%u",
             result->data.target_id, result->data.content_len,
             result->data.effective_len);

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
    LOG_INFO("Content Length: %u bytes (IE), effective: %u bytes (zero padding removed)",
             result->data.content_len, result->data.effective_len);

    /* 打印有效内容的十六进制dump（前64字节） */
    if (result->data.effective_len > 0) {
        LOG_INFO("Effective Content (hex, first 64 bytes):");
        char hex_buf[256];
        int dump_len = (result->data.effective_len > 64) ? 64 : result->data.effective_len;
        for (int i = 0; i < dump_len; i += 16) {
            int line_len = (dump_len - i > 16) ? 16 : (dump_len - i);
            char *p = hex_buf;
            p += sprintf(p, "  %04X: ", i);
            for (int j = 0; j < line_len; j++) {
                p += sprintf(p, "%02X ", result->data.content[i + j]);
            }
            LOG_INFO("%s", hex_buf);
        }
        if (result->data.effective_len > 64) {
            LOG_INFO("  ... (%u more bytes)", result->data.effective_len - 64);
        }
    }
    LOG_INFO("========================================");
}

/**
 * 处理BBU到PAAU的透传消息
 *
 * 数据流：BBU(CPRI 231-240, IE1602) -> 提取有效内容(去尾部补零) -> FPGA(RS422原样发送)
 * 透传内容本身即为完整的FPGA帧（如 EB90 ... 5555），原样转发，不再二次封装。
 */
int handle_transparent_bbu_to_paau(uint32_t msg_id,
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
    LOG_INFO("Content Length: %u bytes (IE), effective: %u bytes",
             trans_msg->content_len, trans_msg->effective_len);
    LOG_INFO("Source IP:     %s", source_ip ? source_ip : "N/A");
    LOG_INFO("========================================");

    /* 将透传有效内容原样转发给FPGA（RS422） */
    if (trans_msg->effective_len > 0) {
        LOG_INFO("Forwarding transparent content (%u bytes) to FPGA via RS422...",
                 trans_msg->effective_len);
        int fpga_ret = fpga_send_raw(trans_msg->content, trans_msg->effective_len);
        if (fpga_ret != SUCCESS) {
            LOG_ERROR("Failed to forward transparent content to FPGA: ret=%d", fpga_ret);
            return fpga_ret;
        }
        LOG_INFO("Transparent content forwarded to FPGA successfully");
    } else {
        LOG_WARN("Transparent content is empty (all zero padding), nothing to forward");
    }

    /* 自动回复开关：该回显构成一个反射原语（收到什么就弹回什么），
     * 不需要时可用 TRANSPARENT_AUTO_REPLY=0 关掉。 */
    if (!g_transparent_auto_reply) {
        LOG_INFO("Transparent auto-reply disabled by config, skipping reply");
        return SUCCESS;
    }

    /* 自动回复透传消息：收到 BBU->PAAU (231-240)，回复 PAAU->BBU (221-230) */
    uint16_t reply_msg_id = MSG_TRANSPARENT_PAAU_TO_BBU_BASE +
                            (msg_id - MSG_TRANSPARENT_BBU_TO_PAAU_BASE);

    LOG_INFO("Auto-replying transparent message: received msgid=%u, reply msgid=%u",
             msg_id, reply_msg_id);

    /* 回复仅携带有效内容（不含补零填充） */
    int ret = send_transparent_paau_to_bbu(reply_msg_id,
                                           trans_msg->target_id,
                                           trans_msg->content,
                                           trans_msg->effective_len);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to send transparent reply: ret=%d", ret);
        return ret;
    }

    LOG_INFO("Transparent reply sent successfully");
    return SUCCESS;
}

/**
 * 发送PAAU到BBU的透传消息（IE格式）
 *
 * payload 组装为：
 *   IE 1601 透传目标: IE头(4) + 目标ID(1)
 *   IE 1602 透传内容: IE头(4) + 内容(content_len)
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

    if (content_len > 0 && !content) {
        LOG_ERROR("Transparent content pointer is NULL with len=%u", content_len);
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("Sending transparent message PAAU->BBU: msgid=%u, target=%s, len=%u",
             msg_id, get_transparent_target_name(target_id), content_len);

    /* payload = IE1601(4+1) + IE1602(4+content_len)，最大 5+4+1024=1033 字节 */
    uint8_t payload[TRANSPARENT_CONTENT_MAX_LEN + 16];
    uint32_t payload_len = 0;

    /* IE 1601: 透传目标 */
    uint16_t ie_type = IE_TRANSPARENT_TARGET;
    uint16_t ie_len = (uint16_t)(sizeof(cpri_ie_header_t) + 1);
    memcpy(payload + payload_len, &ie_type, 2);
    payload_len += 2;
    memcpy(payload + payload_len, &ie_len, 2);
    payload_len += 2;
    payload[payload_len++] = target_id;

    /* IE 1602: 透传内容 */
    ie_type = IE_TRANSPARENT_CONTENT;
    ie_len = (uint16_t)(sizeof(cpri_ie_header_t) + content_len);
    memcpy(payload + payload_len, &ie_type, 2);
    payload_len += 2;
    memcpy(payload + payload_len, &ie_len, 2);
    payload_len += 2;
    if (content_len > 0) {
        memcpy(payload + payload_len, content, content_len);
        payload_len += content_len;
    }

    /* 构造CPRI消息 */
    cpri_message_t message;
    memset(&message, 0, sizeof(message));

    message.header.msg_id = msg_id;
    message.payload = payload;
    message.payload_len = payload_len;

    /* 编码并发送 */
    uint8_t buffer[TRANSPARENT_CONTENT_MAX_LEN + 64];
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

    /* 通道开关：调试/产测结束的部署应把 TRANSPARENT_ENABLE 置 0，
     * 直接封掉这条权限最高的入站路径。 */
    transparent_cfg_load();
    if (!g_transparent_enable) {
        LOG_WARN("Transparent channel disabled by config, message rejected (msg_id=%u)",
                 msg->header.msg_id);
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
