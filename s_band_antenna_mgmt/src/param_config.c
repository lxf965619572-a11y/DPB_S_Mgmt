#include "param_config.h"
#include "fpga_handler.h"
#include "logger.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

/* CPU统计周期全局变量 */
static uint32_t g_cpu_stat_period = 60;  /* 默认60秒 */
static pthread_mutex_t g_period_mutex = PTHREAD_MUTEX_INITIALIZER;

int param_config_init(void)
{
    LOG_INFO("Parameter config module initialized");
    return SUCCESS;
}

/**
 * @brief 配置Linux系统时间
 * 使用settimeofday()系统调用设置系统时间
 */
int param_config_set_system_time(const system_time_config_ie_t *config_ie)
{
    if (!config_ie) {
        LOG_ERROR("Invalid parameter: config_ie is NULL");
        return ERROR_INVALID_PARAM;
    }

    /* 验证时间参数有效性 */
    if (config_ie->second > 59 || config_ie->minute > 59 ||
        config_ie->hour > 23 || config_ie->day < 1 || config_ie->day > 31 ||
        config_ie->month < 1 || config_ie->month > 12) {
        LOG_ERROR("Invalid time parameters: %04u-%02u-%02u %02u:%02u:%02u",
                  config_ie->year, config_ie->month, config_ie->day,
                  config_ie->hour, config_ie->minute, config_ie->second);
        return ERROR_INVALID_PARAM;
    }

    /* 构造tm结构 */
    struct tm tm_time = {0};
    tm_time.tm_year = config_ie->year - 1900;  /* tm_year是从1900年开始 */
    tm_time.tm_mon = config_ie->month - 1;     /* tm_mon是0-11 */
    tm_time.tm_mday = config_ie->day;
    tm_time.tm_hour = config_ie->hour;
    tm_time.tm_min = config_ie->minute;
    tm_time.tm_sec = config_ie->second;

    /* 转换为time_t */
    time_t new_time = mktime(&tm_time);
    if (new_time == -1) {
        LOG_ERROR("mktime() failed for time: %04u-%02u-%02u %02u:%02u:%02u",
                  config_ie->year, config_ie->month, config_ie->day,
                  config_ie->hour, config_ie->minute, config_ie->second);
        return ERROR_GENERAL;
    }

    /* 设置系统时间 */
    struct timeval tv;
    tv.tv_sec = new_time;
    tv.tv_usec = 0;

    if (settimeofday(&tv, NULL) != 0) {
        LOG_ERROR("settimeofday() failed: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    LOG_INFO("System time configured: %04u-%02u-%02u %02u:%02u:%02u",
             config_ie->year, config_ie->month, config_ie->day,
             config_ie->hour, config_ie->minute, config_ie->second);

    return SUCCESS;
}

/**
 * @brief 配置CPU占用率统计周期
 *
 * 思路说明：
 * CPU占用率统计周期用于控制param_query.c中read_cpu_usage()函数的采样间隔。
 * 该周期决定了两次/proc/stat读取之间的时间间隔，从而影响CPU使用率计算的精度。
 *
 * 实现方式：
 * 1. 将周期值保存到全局变量g_cpu_stat_period
 * 2. param_query.c中的read_cpu_usage()函数会调用param_config_get_cpu_period()获取周期
 * 3. read_cpu_usage()根据周期值决定采样间隔
 *
 * 合理范围：
 * - 最小值：1秒（太小会增加系统开销）
 * - 最大值：3600秒（1小时，太大会导致统计不及时）
 * - 推荐值：60秒（平衡精度和开销）
 */
int param_config_set_cpu_period(const cpu_period_config_ie_t *config_ie)
{
    if (!config_ie) {
        LOG_ERROR("Invalid parameter: config_ie is NULL");
        return ERROR_INVALID_PARAM;
    }

    /* 验证周期范围 */
    if (config_ie->period < 1 || config_ie->period > 3600) {
        LOG_ERROR("Invalid CPU stat period: %u (valid range: 1-3600 seconds)",
                  config_ie->period);
        return ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_period_mutex);
    g_cpu_stat_period = config_ie->period;
    pthread_mutex_unlock(&g_period_mutex);

    LOG_INFO("CPU stat period configured: %u seconds", config_ie->period);

    return SUCCESS;
}

/**
 * @brief 配置CPRI工作模式
 * 将工作模式发送给FPGA
 */
int param_config_set_cpri_mode(const cpri_work_mode_config_ie_t *config_ie)
{
    if (!config_ie) {
        LOG_ERROR("Invalid parameter: config_ie is NULL");
        return ERROR_INVALID_PARAM;
    }

    /* 验证工作模式 */
    if (config_ie->work_mode > CPRI_MODE_LOAD_SHARING) {
        LOG_ERROR("Invalid CPRI work mode: %u", config_ie->work_mode);
        return ERROR_INVALID_PARAM;
    }

    /* 发送给FPGA */
    int ret = fpga_handler_set_work_mode(config_ie->work_mode);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to set FPGA work mode: %d", ret);
        return ret;
    }

    const char *mode_str[] = {"Normal", "Cascade", "Master-Slave", "Load-Sharing"};
    LOG_INFO("CPRI work mode configured: %s (%u)",
             mode_str[config_ie->work_mode], config_ie->work_mode);

    return SUCCESS;
}

uint32_t param_config_get_cpu_period(void)
{
    uint32_t period;
    pthread_mutex_lock(&g_period_mutex);
    period = g_cpu_stat_period;
    pthread_mutex_unlock(&g_period_mutex);
    return period;
}

/**
 * @brief 从消息中提取IE并处理
 */
static int handle_config_ie(const cpri_message_t *request, cpri_message_t *response)
{
    const uint8_t *data = request->payload;
    uint16_t offset = 0;
    uint16_t payload_len = request->header.msg_length - sizeof(cpri_msg_header_t);

    int time_result = CONFIG_RESULT_FAILURE;
    int period_result = CONFIG_RESULT_FAILURE;
    int mode_result = CONFIG_RESULT_FAILURE;
    uint8_t main_port = INVALID_FIBER_PORT;
    uint8_t sub_port = INVALID_FIBER_PORT;

    /* 标记哪些IE被处理了 */
    bool time_processed = false;
    bool period_processed = false;
    bool mode_processed = false;

    /* 解析所有IE */
    while (offset < payload_len) {
        if (offset + 4 > payload_len) {
            break;  /* 不足一个IE头部 */
        }

        /* 安全读取IE头：IE类型(2) + IE长度(2) */
        uint16_t ie_type, ie_length;
        memcpy(&ie_type, data + offset, 2);
        memcpy(&ie_length, data + offset + 2, 2);

        LOG_DEBUG("Parsing IE: ie_type=%u, ie_length=%u at offset=%u",
                  ie_type, ie_length, offset);

        /* IE长度包含IE头本身，不能小于IE头长度(4字节)；否则 offset 不推进将导致死循环 */
        if (ie_length < 4 || offset + ie_length > payload_len) {
            LOG_ERROR("IE length exceeds payload: ie_type=%u, ie_length=%u, offset=%u, payload_len=%u",
                      ie_type, ie_length, offset, payload_len);
            break;
        }

        switch (ie_type) {
            case IE_SYSTEM_TIME_CONFIG: {
                const system_time_config_ie_t *ie = (const system_time_config_ie_t *)(data + offset);
                if (param_config_set_system_time(ie) == SUCCESS) {
                    time_result = CONFIG_RESULT_SUCCESS;
                }
                time_processed = true;
                break;
            }

            case IE_CPU_PERIOD_CONFIG: {
                const cpu_period_config_ie_t *ie = (const cpu_period_config_ie_t *)(data + offset);
                if (param_config_set_cpu_period(ie) == SUCCESS) {
                    period_result = CONFIG_RESULT_SUCCESS;
                }
                period_processed = true;
                break;
            }

            case IE_CPRI_WORK_MODE_CONFIG: {
                const cpri_work_mode_config_ie_t *ie = (const cpri_work_mode_config_ie_t *)(data + offset);
                if (param_config_set_cpri_mode(ie) == SUCCESS) {
                    mode_result = CONFIG_RESULT_SUCCESS;
                    /* 获取光纤端口信息 - 从FPGA读取实际端口状态 */
                    if (fpga_handler_get_fiber_ports(&main_port, &sub_port) != SUCCESS) {
                        LOG_WARN("Failed to get fiber ports from FPGA, using default values");
                        main_port = 0;  /* 主光口默认值 */
                        sub_port = INVALID_FIBER_PORT;  /* 备光口默认值 */
                    }
                }
                mode_processed = true;
                break;
            }

            default:
                LOG_WARN("Unknown IE type in config request: %u", ie_type);
                break;
        }

        offset += ie_length;
    }

    /* 构造响应消息 */
    response->header.msg_id = MSG_ID_PARAM_CONFIG_RESP;
    response->header.msg_length = sizeof(cpri_msg_header_t);
    response->header.serial_num = request->header.serial_num;  /* 复制请求的流水号 */

    /* 使用静态缓冲区（线程安全，因为参数配置是串行处理的） */
    static uint8_t resp_buffer[256];
    uint8_t *resp_data = resp_buffer;
    uint16_t resp_offset = 0;

    /* 只添加被请求的IE的响应 */
    if (time_processed) {
        system_time_config_resp_ie_t *time_resp = (system_time_config_resp_ie_t *)(resp_data + resp_offset);
        time_resp->ie_type = IE_SYSTEM_TIME_CONFIG_RESP;
        time_resp->ie_length = sizeof(system_time_config_resp_ie_t);
        time_resp->result = time_result;
        resp_offset += sizeof(system_time_config_resp_ie_t);
    }

    if (period_processed) {
        cpu_period_config_resp_ie_t *period_resp = (cpu_period_config_resp_ie_t *)(resp_data + resp_offset);
        period_resp->ie_type = IE_CPU_PERIOD_CONFIG_RESP;
        period_resp->ie_length = sizeof(cpu_period_config_resp_ie_t);
        period_resp->result = period_result;
        resp_offset += sizeof(cpu_period_config_resp_ie_t);
    }

    if (mode_processed) {
        cpri_work_mode_config_resp_ie_t *mode_resp = (cpri_work_mode_config_resp_ie_t *)(resp_data + resp_offset);
        mode_resp->ie_type = IE_CPRI_WORK_MODE_CONFIG_RESP;
        mode_resp->ie_length = sizeof(cpri_work_mode_config_resp_ie_t);
        mode_resp->main_fiber_port = main_port;
        mode_resp->sub_fiber_port = sub_port;
        mode_resp->result = mode_result;
        resp_offset += sizeof(cpri_work_mode_config_resp_ie_t);
    }

    /* 设置响应payload */
    response->payload = resp_buffer;
    response->payload_len = resp_offset;
    response->header.msg_length += resp_offset;

    return SUCCESS;
}

int param_config_handle_request(const cpri_message_t *request, cpri_message_t *response)
{
    if (!request || !response) {
        LOG_ERROR("Invalid parameters");
        return ERROR_INVALID_PARAM;
    }

    if (request->header.msg_id != MSG_ID_PARAM_CONFIG) {
        LOG_ERROR("Invalid message ID: %u (expected %u)",
                  request->header.msg_id, MSG_ID_PARAM_CONFIG);
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("Handling parameter config request");

    return handle_config_ie(request, response);
}
