/**
 * @file param_query.c
 * @brief 参数查询模块实现 - 完整的参数查询处理功能
 */

#include "param_query.h"
#include "param_config.h"
#include "fpga_handler.h"
#include "logger.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>

/* CPU统计信息 */
static cpu_stats_t g_cpu_stats = {
    .cpu_usage = 0,
    .stat_period = 5,  /* 默认5秒统计周期 */
    .last_update = 0
};

/* 外部全局变量 */
extern uint32_t g_serial_num;

/**
 * @brief 读取CPU占用率
 * @return CPU占用率 (0-100)
 */
static uint32_t read_cpu_usage(void)
{
    static unsigned long long prev_total = 0;
    static unsigned long long prev_idle = 0;

    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        LOG_ERROR("Failed to open /proc/stat");
        return 0;
    }

    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    /* 解析CPU时间: cpu user nice system idle iowait irq softirq */
    unsigned long long user, nice, system, idle, iowait, irq, softirq;
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq) != 7) {
        return 0;
    }

    unsigned long long total = user + nice + system + idle + iowait + irq + softirq;

    if (prev_total == 0) {
        /* 第一次读取,保存基准值 */
        prev_total = total;
        prev_idle = idle;
        return 0;
    }

    /* 计算CPU占用率 */
    unsigned long long total_diff = total - prev_total;
    unsigned long long idle_diff = idle - prev_idle;

    uint32_t usage = 0;
    if (total_diff > 0) {
        usage = (uint32_t)(100 * (total_diff - idle_diff) / total_diff);
    }

    prev_total = total;
    prev_idle = idle;

    return usage;
}

/**
 * @brief 初始化参数查询模块
 */
int param_query_init(void)
{
    /* 初始化CPU统计 */
    g_cpu_stats.cpu_usage = 0;
    g_cpu_stats.stat_period = 5;
    g_cpu_stats.last_update = time(NULL);

    /* 读取初始CPU状态 */
    read_cpu_usage();

    LOG_INFO("Parameter query module initialized");
    return SUCCESS;
}

/**
 * @brief 清理参数查询模块
 */
void param_query_cleanup(void)
{
    LOG_INFO("Parameter query module cleaned up");
}

/**
 * @brief 获取CPU占用率
 */
uint32_t param_query_get_cpu_usage(void)
{
    time_t now = time(NULL);

    /* 如果距离上次更新超过统计周期,重新读取 */
    if (now - g_cpu_stats.last_update >= g_cpu_stats.stat_period) {
        g_cpu_stats.cpu_usage = read_cpu_usage();
        g_cpu_stats.last_update = now;
    }

    return g_cpu_stats.cpu_usage;
}

/**
 * @brief 获取CPU统计周期
 */
uint32_t param_query_get_cpu_period(void)
{
    /* 从param_config模块获取配置的周期 */
    return param_config_get_cpu_period();
}

/**
 * @brief 更新CPU统计信息
 */
void param_query_update_cpu_stats(uint32_t usage)
{
    g_cpu_stats.cpu_usage = usage;
    g_cpu_stats.last_update = time(NULL);
}

/* 预分配缓冲区大小 */
#define MAX_RESPONSE_PAYLOAD_SIZE 4096

/**
 * @brief 添加响应IE到消息
 */
static int add_response_ie(cpri_message_t *msg, uint16_t ie_type,
                          const void *ie_data, uint16_t ie_data_len)
{
    if (!msg || !ie_data) {
        return ERROR_INVALID_PARAM;
    }

    /* 首次分配时创建缓冲区 */
    if (msg->payload == NULL) {
        msg->payload = (uint8_t*)malloc(MAX_RESPONSE_PAYLOAD_SIZE);
        if (!msg->payload) {
            LOG_ERROR("Failed to allocate response payload buffer");
            return ERROR_MEMORY;
        }
        msg->payload_len = 0;
    }

    /* 计算新的payload长度 */
    uint16_t ie_total_len = 4 + ie_data_len;  /* IE头(4) + 数据 */
    uint32_t new_payload_len = msg->payload_len + ie_total_len;

    /* 检查缓冲区是否足够 */
    if (new_payload_len > MAX_RESPONSE_PAYLOAD_SIZE) {
        LOG_ERROR("Response payload buffer overflow: need %u, max %u",
                  new_payload_len, MAX_RESPONSE_PAYLOAD_SIZE);
        return ERROR_MEMORY;
    }

    /* 添加IE头 */
    uint32_t offset = msg->payload_len;
    memcpy(msg->payload + offset, &ie_type, 2);
    offset += 2;
    memcpy(msg->payload + offset, &ie_total_len, 2);
    offset += 2;

    /* 添加IE数据 */
    memcpy(msg->payload + offset, ie_data, ie_data_len);

    msg->payload_len = new_payload_len;

    return SUCCESS;
}

/**
 * @brief 处理系统时间查询
 */
int param_query_handle_system_time(cpri_message_t *response)
{
    system_time_resp_ie_t ie_data;

    time_t currentTime = time(NULL);
    struct tm *timeInfo = localtime(&currentTime);

    ie_data.second = timeInfo->tm_sec;         // 秒: 0-59
    ie_data.minute = timeInfo->tm_min;         // 分: 0-59
    ie_data.hour = timeInfo->tm_hour;          // 时: 0-23
    ie_data.day = timeInfo->tm_mday;           // 日: 1-31
    ie_data.month = timeInfo->tm_mon + 1;      // 月: 1-12 (tm_mon范围是0-11)
    ie_data.year = timeInfo->tm_year + 1900;   // 年: 1970-2099 (tm_year是从1900年开始的年数)
    
    int ret = add_response_ie(response, IE_TYPE_SYSTEM_TIME_RESP,
                              &ie_data, sizeof(ie_data));
    if (ret == SUCCESS) {
        LOG_DEBUG("Added system time response IE");
    }

    return ret;
}

/**
 * @brief 处理CPU占用率查询
 */
int param_query_handle_cpu_usage(cpri_message_t *response)
{
    cpu_usage_resp_ie_t ie_data;
    ie_data.cpu_usage = param_query_get_cpu_usage();

    int ret = add_response_ie(response, IE_TYPE_CPU_USAGE_RESP,
                              &ie_data, sizeof(ie_data));
    if (ret == SUCCESS) {
        LOG_DEBUG("Added CPU usage response IE: %u%%", ie_data.cpu_usage);
    }

    return ret;
}

/**
 * @brief 处理CPU统计周期查询
 */
int param_query_handle_cpu_period(cpri_message_t *response)
{
    cpu_period_resp_ie_t ie_data;
    ie_data.period = param_query_get_cpu_period();

    int ret = add_response_ie(response, IE_TYPE_CPU_PERIOD_RESP,
                              &ie_data, sizeof(ie_data));
    if (ret == SUCCESS) {
        LOG_DEBUG("Added CPU period response IE: %u seconds", ie_data.period);
    }

    return ret;
}

/**
 * @brief 处理相控阵温度查询
 */
int param_query_handle_temperature(uint8_t temp_point, cpri_message_t *response)
{
    temp_resp_ie_t ie_data;
    ie_data.temp_point = temp_point;

    /* 从FPGA状态获取温度 */
    fpga_status_frame_t fpga_status;
    if (fpga_handler_get_status(&fpga_status) != SUCCESS) {
        LOG_WARN("FPGA status not available for temperature query, using default");
        ie_data.temperature = 0;
    } else {
        /* 根据测温点索引获取温度 */
        switch (temp_point) {
            case 0:
                ie_data.temperature = fpga_status.data.temp1;
                break;
            case 1:
                ie_data.temperature = fpga_status.data.temp2;
                break;
            case 2:
                ie_data.temperature = fpga_status.data.temp3;
                break;
            default:
                LOG_WARN("Invalid temperature point: %u", temp_point);
                ie_data.temperature = 0;
                break;
        }
    }

    int ret = add_response_ie(response, IE_TYPE_TEMP_RESP,
                              &ie_data, sizeof(ie_data));
    if (ret == SUCCESS) {
        LOG_DEBUG("Added temperature response IE: point=%u, temp=%d°C",
                  ie_data.temp_point, ie_data.temperature);
    }

    return ret;
}

/**
 * @brief 处理过温门限查询
 */
int param_query_handle_temp_threshold(uint8_t temp_point, cpri_message_t *response)
{
    temp_threshold_resp_ie_t ie_data;
    ie_data.temp_point = temp_point;

    /* 从FPGA状态获取温度门限 */
    fpga_status_frame_t fpga_status;
    if (fpga_handler_get_status(&fpga_status) != SUCCESS) {
        LOG_ERROR("Failed to get FPGA status for threshold query");
        ie_data.up_threshold = 85;
        ie_data.low_threshold = -40;
    } else {
        /* 根据测温点索引获取门限 */
        switch (temp_point) {
            case 0:
                ie_data.up_threshold = fpga_status.data.temp1_high;
                ie_data.low_threshold = fpga_status.data.temp1_low;
                break;
            case 1:
                ie_data.up_threshold = fpga_status.data.temp2_high;
                ie_data.low_threshold = fpga_status.data.temp2_low;
                break;
            case 2:
                ie_data.up_threshold = fpga_status.data.temp3_high;
                ie_data.low_threshold = fpga_status.data.temp3_low;
                break;
            default:
                LOG_WARN("Invalid temperature point: %u", temp_point);
                ie_data.up_threshold = 85;
                ie_data.low_threshold = -40;
                break;
        }
    }

    int ret = add_response_ie(response, IE_TYPE_TEMP_THRESHOLD_RESP,
                              &ie_data, sizeof(ie_data));
    if (ret == SUCCESS) {
        LOG_DEBUG("Added temp threshold response IE: point=%u, high=%d°C, low=%d°C",
                  ie_data.temp_point, ie_data.up_threshold, ie_data.low_threshold);
    }

    return ret;
}

/**
 * @brief 处理输出功率查询
 */
int param_query_handle_output_power(uint16_t rf_channel, cpri_message_t *response)
{
    output_power_resp_ie_t ie_data;
    ie_data.reserved = 0;

    /* 从FPGA状态获取输出功率 */
    fpga_status_frame_t fpga_status;
    if (fpga_handler_get_status(&fpga_status) != SUCCESS) {
        LOG_ERROR("Failed to get FPGA status for power query");
        ie_data.power = 0;
    } else {
        /* 获取当前输出功率 (1/256 dBm) */
        ie_data.power = fpga_status.data.current_output_power;
    }

    int ret = add_response_ie(response, IE_TYPE_OUTPUT_POWER_RESP,
                              &ie_data, sizeof(ie_data));
    if (ret == SUCCESS) {
        LOG_DEBUG("Added output power response IE: channel=%u, power=%u (1/256 dBm)",
                  rf_channel, ie_data.power);
    }

    return ret;
}

/**
 * @brief 处理Toffset查询
 */
int param_query_handle_toffset(cpri_message_t *response)
{
    toffset_resp_ie_t ie_data;

    /* 从FPGA状态获取Toffset */
    fpga_status_frame_t fpga_status;
    if (fpga_handler_get_status(&fpga_status) != SUCCESS) {
        LOG_ERROR("Failed to get FPGA status for Toffset query");
        ie_data.delay = 0;
    } else {
        ie_data.delay = fpga_status.data.toffset;
    }

    int ret = add_response_ie(response, IE_TYPE_TOFFSET_RESP,
                              &ie_data, sizeof(ie_data));
    if (ret == SUCCESS) {
        LOG_DEBUG("Added Toffset response IE: delay=%u", ie_data.delay);
    }

    return ret;
}

/**
 * @brief 处理T2a查询
 */
int param_query_handle_t2a(cpri_message_t *response)
{
    t2a_resp_ie_t ie_data;

    /* 从FPGA状态获取T2a */
    fpga_status_frame_t fpga_status;
    if (fpga_handler_get_status(&fpga_status) != SUCCESS) {
        LOG_ERROR("Failed to get FPGA status for T2a query");
        ie_data.delay = 0;
    } else {
        ie_data.delay = fpga_status.data.t2a;
    }

    int ret = add_response_ie(response, IE_TYPE_T2A_RESP,
                              &ie_data, sizeof(ie_data));
    if (ret == SUCCESS) {
        LOG_DEBUG("Added T2a response IE: delay=%u", ie_data.delay);
    }

    return ret;
}

/**
 * @brief 处理Ta3查询
 */
int param_query_handle_ta3(cpri_message_t *response)
{
    ta3_resp_ie_t ie_data;

    /* 从FPGA状态获取Ta3 */
    fpga_status_frame_t fpga_status;
    if (fpga_handler_get_status(&fpga_status) != SUCCESS) {
        LOG_ERROR("Failed to get FPGA status for Ta3 query");
        ie_data.delay = 0;
    } else {
        ie_data.delay = fpga_status.data.ta3;
    }

    int ret = add_response_ie(response, IE_TYPE_TA3_RESP,
                              &ie_data, sizeof(ie_data));
    if (ret == SUCCESS) {
        LOG_DEBUG("Added Ta3 response IE: delay=%u", ie_data.delay);
    }

    return ret;
}

/**
 * @brief 处理参数查询请求
 */
int param_query_handle_request(const cpri_message_t *request, cpri_message_t *response)
{
    if (!request || !response) {
        return ERROR_INVALID_PARAM;
    }

    /* 初始化响应消息 */
    memset(response, 0, sizeof(cpri_message_t));
    response->header.msg_id = MSG_PAAU_PARAM_QUERY_RSP;
    response->header.paau_id = request->header.paau_id;
    response->header.bbu_id = request->header.bbu_id;
    response->header.port_num = request->header.port_num;
    response->header.serial_num = request->header.serial_num;  /* 返回相同流水号 */
    response->payload_len = 0;
    response->payload = NULL;

    /* 解析请求中的所有IE */
    uint32_t offset = 0;
    int ie_count = 0;

    while (offset < request->payload_len) {
        if (offset + 4 > request->payload_len) {
            LOG_ERROR("Invalid IE header at offset %u", offset);
            break;
        }

        /* 读取IE头 */
        uint16_t ie_type, ie_len;
        memcpy(&ie_type, request->payload + offset, 2);
        offset += 2;
        memcpy(&ie_len, request->payload + offset, 2);
        offset += 2;

        uint16_t ie_data_len = ie_len - 4;  /* 减去IE头长度 */

        if (offset + ie_data_len > request->payload_len) {
            LOG_ERROR("Invalid IE data length at offset %u", offset);
            break;
        }

        const uint8_t *ie_data = request->payload + offset;

        /* 根据IE类型处理查询 */
        switch (ie_type) {
            case IE_TYPE_SYSTEM_TIME_QUERY:
                param_query_handle_system_time(response);
                ie_count++;
                break;

            case IE_TYPE_CPU_USAGE_QUERY:
                param_query_handle_cpu_usage(response);
                ie_count++;
                break;

            case IE_TYPE_CPU_PERIOD_QUERY:
                param_query_handle_cpu_period(response);
                ie_count++;
                break;

            case IE_TYPE_TEMP_QUERY:
                if (ie_data_len >= 1) {
                    uint8_t temp_point = ie_data[0];
                    param_query_handle_temperature(temp_point, response);
                    ie_count++;
                }
                break;

            case IE_TYPE_TEMP_THRESHOLD_QUERY:
                if (ie_data_len >= 1) {
                    uint8_t temp_point = ie_data[0];
                    param_query_handle_temp_threshold(temp_point, response);
                    ie_count++;
                }
                break;

            case IE_TYPE_OUTPUT_POWER_QUERY:
                if (ie_data_len >= 2) {
                    uint16_t rf_channel;
                    memcpy(&rf_channel, ie_data, 2);
                    param_query_handle_output_power(rf_channel, response);
                    ie_count++;
                }
                break;

            case IE_TYPE_TOFFSET_QUERY:
                param_query_handle_toffset(response);
                ie_count++;
                break;

            case IE_TYPE_T2A_QUERY:
                param_query_handle_t2a(response);
                ie_count++;
                break;

            case IE_TYPE_TA3_QUERY:
                param_query_handle_ta3(response);
                ie_count++;
                break;

            default:
                LOG_WARN("Unknown query IE type: %u", ie_type);
                break;
        }

        offset += ie_data_len;
    }

    LOG_INFO("Processed parameter query request: %d IEs", ie_count);

    return SUCCESS;
}
