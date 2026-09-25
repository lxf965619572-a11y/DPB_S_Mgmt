#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include "reset.h"
#include "msg_handler.h"
#include "logger.h"
#include "alarm_manager.h"
#include "fpga_handler.h"
#include "config.h"

/* 模块初始化标志 */
static int g_reset_initialized = 0;

/* ==================== 复位动作的落地开关（可配置） ====================
 * 改造前的状态：软/硬复位的 system() 都被注释掉，但日志仍打印
 *   "Service will restart in 2 seconds..." / "System will reboot in 2 seconds..."
 * 并且真的 sleep(2)，函数最后无条件 return SUCCESS —— BBU 以为已重启并继续
 * 下发配置，实际什么也没发生，且现场无从察觉。
 *
 * 配置项（config/antenna_mgmt.conf）：
 *   RESET_RESTART_SERVICE  1=软复位后重启本服务（默认）   0=不重启
 *   RESET_REBOOT_SYSTEM    1=硬复位后重启整个系统         0=不重启（默认）
 * 整机重启影响面大，默认关闭；需要时按现场约定打开。
 * ================================================================== */
static int  g_reset_restart_service = 1;
static int  g_reset_reboot_system   = 0;
static bool g_reset_cfg_loaded      = false;

static void reset_cfg_load(void)
{
    if (g_reset_cfg_loaded) {
        return;
    }
    g_reset_restart_service = config_get_int("RESET_RESTART_SERVICE", 1);
    g_reset_reboot_system   = config_get_int("RESET_REBOOT_SYSTEM", 0);
    g_reset_cfg_loaded = true;

    LOG_INFO("Reset actions config: restart_service=%d, reboot_system=%d",
             g_reset_restart_service, g_reset_reboot_system);
}

/* 异步执行重启动作：fork 一个脱离会话的子进程，先睡 2 秒（给北向应答留发送时间），
 * 再 execvp 执行。不用 shell，也不在消息处理线程里 sleep —— 后者会阻塞 TCP
 * 接收线程，正是本项目反复踩的那个坑。 */
static void reset_schedule_action(char *const argv[], const char *what)
{
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("Failed to fork for %s: %s", what, strerror(errno));
        return;
    }

    if (pid == 0) {
        setsid();           /* 脱离会话，父进程重启/退出不影响它 */
        sleep(2);
        execvp(argv[0], argv);
        _exit(127);         /* exec 失败 */
    }

    LOG_INFO("Scheduled %s in 2 seconds (helper pid=%d)", what, (int)pid);
}

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

    reset_cfg_load();

    LOG_INFO("========================================");
    LOG_INFO("RESET RECEIVED");
    LOG_INFO("========================================");
    LOG_INFO("Message ID:  %u %s", msg_id,
             msg_id == MSG_RESET_IND ? "(RESET_IND)" : "(REMOTE_RESET_IND)");
    LOG_INFO("Reset Type:  %s", get_reset_type_str(reset_type));
    LOG_INFO("Source IP:   %s", source_ip ? source_ip : "N/A");
    LOG_INFO("========================================");
    LOG_INFO("Payload Echo:");
    LOG_INFO("  reset_type = 0x%08X (%u)", ind_ie->reset_type, reset_type);
    LOG_INFO("========================================");

    if (reset_type == RESET_TYPE_SOFT) {
        LOG_INFO("Executing soft reset (相控阵软复位)");

        /* 发送相控阵软复位命令到FPGA */
        int fpga_ret = fpga_send_phase_restore(reset_type);  /* 0表示软复位 */
        if (fpga_ret != SUCCESS) {
            /* 关键：FPGA 侧没收到命令就必须如实返回失败，
             * 否则 BBU 会以为复位已生效并继续下发配置。 */
            LOG_ERROR("Failed to send soft reset command to FPGA, reset NOT performed");
            return ERROR_GENERAL;
        }
        LOG_INFO("Soft reset command sent to FPGA successfully");

        /* 重启本服务（FPGA 命令已确认下发后才做） */
        if (g_reset_restart_service) {
            char *argv[] = { (char *)"systemctl", (char *)"restart",
                             (char *)"antenna-mgmt.service", NULL };
            reset_schedule_action(argv, "service restart");
        } else {
            LOG_INFO("Service restart disabled by config (RESET_RESTART_SERVICE=0); "
                     "FPGA soft reset done, management service NOT restarted");
        }

    } else if (reset_type == RESET_TYPE_HARD) {
        LOG_INFO("Executing hard reset (相控阵硬复位)");

        /* 发送相控阵硬复位命令到FPGA */
        int fpga_ret = fpga_send_phase_restore(reset_type);  /* 1表示硬复位 */
        if (fpga_ret != SUCCESS) {
            LOG_ERROR("Failed to send hard reset command to FPGA, reset NOT performed");
            return ERROR_GENERAL;
        }
        LOG_INFO("Hard reset command sent to FPGA successfully");

        /* 重启整个系统（默认关闭：整机重启影响面大） */
        if (g_reset_reboot_system) {
            char *argv[] = { (char *)"shutdown", (char *)"-r", (char *)"now", NULL };
            reset_schedule_action(argv, "system reboot");
        } else {
            LOG_INFO("System reboot disabled by config (RESET_REBOOT_SYSTEM=0); "
                     "FPGA hard reset done, system NOT rebooted");
        }

    } else {
        LOG_WARN("Unknown reset type %u, no action taken", reset_type);
    }

    return SUCCESS;
}
