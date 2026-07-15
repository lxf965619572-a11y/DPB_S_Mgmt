#include <string.h>
#include <time.h>
#include "reset.h"
#include "msg_handler.h"
#include "logger.h"
#include "alarm_manager.h"
#include "fpga_handler.h"

/* 模块初始化标志 */
static int g_reset_initialized = 0;

/**
 * 初始化复位模块
 */
int reset_init(void)
{
    if (g_reset_initialized) {
        LOG_WARN("Reset module already initialized");
        return SUCCESS;
    }

    LOG_INFO("Initializing reset module...");
    g_reset_initialized = 1;
    LOG_INFO("Reset module initialized successfully");

    return SUCCESS;
}

/**
 * 清理复位模块
 */
void reset_cleanup(void)
{
    if (!g_reset_initialized) {
        return;
    }

    LOG_INFO("Cleaning up reset module...");
    g_reset_initialized = 0;
    LOG_INFO("Reset module cleaned up");
}

/**
 * 获取复位类型字符串
 */
static const char* get_reset_type_str(uint32_t type)
{
    switch (type) {
        case RESET_TYPE_SOFT: return "SOFT_RESET (Software Restart)";
        case RESET_TYPE_HARD: return "HARD_RESET (Hardware Power Cycle)";
        default:              return "UNKNOWN";
    }
}

/**
 * 验证复位指示合法性
 */
int reset_validate_indication(const ie_reset_ind_t *ind)
{
    if (!ind) {
        LOG_ERROR("Invalid indication pointer");
        return RESET_ERR_FORMAT;
    }

    uint32_t reset_type = le32toh(ind->reset_type);

    /* 验证复位类型 */
    if (reset_type > RESET_TYPE_HARD) {
        LOG_ERROR("Invalid reset type: %u", reset_type);
        return RESET_ERR_INVALID_TYPE;
    }

    return SUCCESS;
}

/**
 * 解析复位指示消息
 */
int reset_parse_indication(const uint8_t *data, size_t len,
                           reset_parse_result_t *result)
{
    if (!data || !result) {
        LOG_ERROR("Invalid parameters for reset parse");
        return RESET_ERR_FORMAT;
    }

    /* 检查数据长度 */
    if (len < sizeof(ie_reset_ind_t)) {
        LOG_ERROR("Reset indication data too short: %zu bytes (expected: %zu)",
                 len, sizeof(ie_reset_ind_t));
        return RESET_ERR_FORMAT;
    }

    /* 初始化结果 */
    memset(result, 0, sizeof(*result));
    result->msg_id = MSG_RESET_IND;

    /* 获取当前时间戳 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(result->timestamp, sizeof(result->timestamp),
             "%Y-%m-%d %H:%M:%S", tm_info);

    /* 解析IE数据 */
    const ie_reset_ind_t *ind = (const ie_reset_ind_t *)data;
    memcpy(&result->request, ind, sizeof(ie_reset_ind_t));

    /* 字节序转换 */
    result->request.reset_type = le32toh(result->request.reset_type);

    /* 验证指示 */
    int ret = reset_validate_indication(ind);
    result->parse_status = ret;

    if (ret != SUCCESS) {
        LOG_ERROR("Reset indication validation failed: error=%d", ret);
    } else {
        LOG_DEBUG("Reset indication parsed successfully");
    }

    return ret;
}

/**
 * 记录结构化审计日志
 */
static void log_reset_audit(const reset_parse_result_t *result)
{
    LOG_INFO("========================================");
    LOG_INFO("Reset Message Audit Log");
    LOG_INFO("========================================");
    LOG_INFO("Message ID:    %u", result->msg_id);
    LOG_INFO("Timestamp:     %s", result->timestamp);
    LOG_INFO("Source IP:     %s", result->source_ip[0] ? result->source_ip : "N/A");
    LOG_INFO("Parse Status:  %d %s", result->parse_status,
             result->parse_status == SUCCESS ? "(SUCCESS)" : "(FAILED)");
    LOG_INFO("----------------------------------------");
    LOG_INFO("Reset Type:    %u (%s)", result->request.reset_type,
             get_reset_type_str(result->request.reset_type));
    LOG_INFO("========================================");
}

/**
 * 触发解析失败告警
 */
static void trigger_reset_parse_alarm(int error_code, const char *source_ip)
{
    const char *error_desc = "Unknown error";
    uint32_t alarm_code = 70001;  /* 复位解析失败基础告警码 */

    switch (error_code) {
        case RESET_ERR_FORMAT:
            error_desc = "Reset message format error";
            alarm_code = 70001;
            break;
        case RESET_ERR_PERMISSION:
            error_desc = "Reset permission denied";
            alarm_code = 70002;
            break;
        case RESET_ERR_OUT_OF_RANGE:
            error_desc = "Reset field out of range";
            alarm_code = 70003;
            break;
        case RESET_ERR_INVALID_TYPE:
            error_desc = "Invalid reset type";
            alarm_code = 70004;
            break;
    }

    char alarm_info[256];
    snprintf(alarm_info, sizeof(alarm_info),
             "Reset parse failed from %s: %s (error=%d)",
             source_ip ? source_ip : "unknown", error_desc, error_code);

    LOG_ERROR("%s", alarm_info);

    /* 触发告警 */
    alarm_trigger(alarm_code, 0, error_desc);
}

/**
 * 处理复位指示
 */
int handle_reset_indication(const ie_reset_ind_t *ind_ie,
                           const char *source_ip,
                           uint16_t msg_id)
{
    if (!g_reset_initialized) {
        LOG_ERROR("Reset module not initialized");
        return ERROR_NOT_INITIALIZED;
    }

    if (!ind_ie) {
        LOG_ERROR("Invalid indication IE pointer");
        return ERROR_INVALID_PARAM;
    }

    /* 解析和验证指示 */
    reset_parse_result_t parse_result;
    memset(&parse_result, 0, sizeof(parse_result));
    parse_result.msg_id = msg_id;

    /* 复制来源IP */
    if (source_ip) {
        strncpy(parse_result.source_ip, source_ip,
                sizeof(parse_result.source_ip) - 1);
    }

    /* 解析指示 */
    int ret = reset_parse_indication((const uint8_t *)ind_ie,
                                     sizeof(*ind_ie), &parse_result);

    /* 记录审计日志 */
    log_reset_audit(&parse_result);

    /* 如果解析失败，触发告警 */
    if (ret != SUCCESS) {
        trigger_reset_parse_alarm(ret, source_ip);
        return ret;
    }

    uint32_t reset_type = le32toh(ind_ie->reset_type);

    /* 现阶段仅记录日志，不执行实际业务 */
    LOG_INFO("========================================");
    LOG_INFO("RESET RECEIVED - Echo Mode");
    LOG_INFO("========================================");
    LOG_INFO("Message ID:  %u %s", msg_id,
             msg_id == MSG_RESET_IND ? "(RESET_IND)" : "(REMOTE_RESET_IND)");
    LOG_INFO("Reset Type:  %s", get_reset_type_str(reset_type));
    LOG_INFO("Source IP:   %s", source_ip ? source_ip : "N/A");
    LOG_INFO("========================================");
    LOG_INFO("Payload Echo:");
    LOG_INFO("  reset_type = 0x%08X (%u)", ind_ie->reset_type, reset_type);
    LOG_INFO("========================================");

    /* 记录警告：实际环境中应执行复位操作 */
    if (reset_type == RESET_TYPE_SOFT) {
        // LOG_WARN("Soft reset requested but not executed (echo mode)");
        // LOG_WARN("In production: systemctl restart antenna-mgmt.service");
        LOG_INFO("Executing soft reset (相控阵软复位)");

        /* 发送相控阵软复位命令到FPGA */
        int fpga_ret = fpga_send_phase_restore(reset_type);  /* 0表示软复位 */
        if (fpga_ret != SUCCESS) {
            LOG_ERROR("Failed to send soft reset command to FPGA");
        } else {
            LOG_INFO("Soft reset command sent to FPGA successfully");
        }

        /* 延迟2秒后重启服务 */
        LOG_INFO("Service will restart in 2 seconds...");
        sleep(2);
        // system("systemctl restart antenna-mgmt.service &");

    } else if (reset_type == RESET_TYPE_HARD) {
        // LOG_WARN("Hard reset requested but not executed (echo mode)");
        // LOG_WARN("In production: system reboot or hardware power cycle");
        LOG_INFO("Executing hard reset (相控阵硬复位 + 系统重启)");

        /* 发送相控阵硬复位命令到FPGA */
        int fpga_ret = fpga_send_phase_restore(reset_type);  /* 1表示硬复位 */
        if (fpga_ret != SUCCESS) {
            LOG_ERROR("Failed to send hard reset command to FPGA");
        } else {
            LOG_INFO("Hard reset command sent to FPGA successfully");
        }

        /* 延迟2秒后重启系统 */
        LOG_INFO("System will reboot in 2 seconds...");
        sleep(2);
        // system("shutdown -r now &");
    }

    return SUCCESS;
}
