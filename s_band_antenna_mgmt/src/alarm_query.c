#include <string.h>
#include <time.h>
#include "alarm_query.h"
#include "alarm_manager.h"
#include "tcp_client.h"
#include "cpri_protocol.h"
#include "logger.h"

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;

/* 模块初始化标志 */
static int g_alarm_query_initialized = 0;

/**
 * 初始化告警查询模块
 */
int alarm_query_init(void)
{
    if (g_alarm_query_initialized) {
        LOG_WARN("Alarm query module already initialized");
        return SUCCESS;
    }

    LOG_INFO("Initializing alarm query module...");
    g_alarm_query_initialized = 1;
    LOG_INFO("Alarm query module initialized successfully");

    return SUCCESS;
}

/**
 * 清理告警查询模块
 */
void alarm_query_cleanup(void)
{
    if (!g_alarm_query_initialized) {
        return;
    }

    LOG_INFO("Cleaning up alarm query module...");
    g_alarm_query_initialized = 0;
    LOG_INFO("Alarm query module cleaned up");
}

/**
 * 检查告警是否匹配查询条件
 */
static int alarm_matches_query(const alarm_record_t *record,
                                uint32_t query_code,
                                uint32_t query_subcode)
{
    /* 查询所有告警 */
    if (query_code == ALARM_QUERY_ALL && query_subcode == ALARM_QUERY_ALL) {
        return 1;
    }

    /* 查询特定告警码的所有子码 */
    if (query_code != ALARM_QUERY_ALL && query_subcode == ALARM_QUERY_ALL) {
        return (record->alarm_code == query_code);
    }

    /* 查询特定告警码和子码 */
    if (query_code != ALARM_QUERY_ALL && query_subcode != ALARM_QUERY_ALL) {
        return (record->alarm_code == query_code && record->sub_code == query_subcode);
    }

    return 0;
}

/**
 * 发送告警上报消息
 */
int send_alarm_report_from_record(const cpri_msg_header_t *req_header,
                                   const alarm_record_t *record)
{
    if (!req_header || !record) {
        LOG_ERROR("Invalid parameter");
        return ERROR_INVALID_PARAM;
    }

    /* 构造告警上报IE */
    ie_alarm_report_t report_ie;
    memset(&report_ie, 0, sizeof(report_ie));

    report_ie.validity = (ALARM_VALIDITY_VALID);
    report_ie.alarm_code = (record->alarm_code);
    report_ie.sub_code = (record->sub_code);
    report_ie.clear_flag = (record->active ? ALARM_CLEAR_FLAG_ACTIVE : ALARM_CLEAR_FLAG_CLEARED);

    /* 格式化时间戳 */
    struct tm *tm_info = localtime(&record->start_time);
    strftime(report_ie.timestamp, sizeof(report_ie.timestamp),
             "%Y-%m-%d %H:%M:%S", tm_info);

    /* 复制附加信息 */
    strncpy(report_ie.additional_info, record->additional_info,
            sizeof(report_ie.additional_info) - 1);

    /* 构造CPRI消息 */
    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    response.header.msg_id = MSG_ALARM_REPORT_REQ_ACK;
    response.header.paau_id = req_header->paau_id;
    response.header.bbu_id = req_header->bbu_id;
    response.header.port_num = req_header->port_num;
    response.header.serial_num = req_header->serial_num;

    /* 使用栈上静态缓冲区，避免动态分配 */
    uint16_t ie_type = 1001;
    uint16_t ie_len = 4 + sizeof(ie_alarm_report_t);  /* IE头 + 数据 */
    response.payload_len = ie_len;

    uint8_t payload_buffer[256];  /* 足够容纳IE头(4) + ie_alarm_report_t(134) */
    response.payload = payload_buffer;

    /* 写入IE头（小端序） */
    memcpy(response.payload, &ie_type, 2);
    memcpy(response.payload + 2, &ie_len, 2);

    /* 写入IE数据 */
    memcpy(response.payload + 4, &report_ie, sizeof(ie_alarm_report_t));

    /* 调试：打印前16字节 */
    LOG_DEBUG("Alarm IE header: type=%u(0x%04X), len=%u(0x%04X)",
             ie_type, ie_type, ie_len, ie_len);
    LOG_DEBUG("Alarm IE bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
             response.payload[0], response.payload[1], response.payload[2], response.payload[3],
             response.payload[4], response.payload[5], response.payload[6], response.payload[7]);

    /* 编码并发送 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Alarm report sent: code=%u, subcode=%u, active=%d, time=%s",
                 record->alarm_code, record->sub_code, record->active,
                 report_ie.timestamp);
    } else {
        LOG_ERROR("Failed to encode alarm report");
    }

    /* 无需free，使用的是栈缓冲区 */
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

/**
 * 发送无效告警响应
 */
static int send_invalid_alarm_response(const cpri_msg_header_t *req_header,
                                       uint32_t alarm_code,
                                       uint32_t sub_code)
{
    ie_alarm_report_t report_ie;
    memset(&report_ie, 0, sizeof(report_ie));

    report_ie.validity = (ALARM_VALIDITY_INVALID);
    report_ie.alarm_code = (alarm_code);
    report_ie.sub_code = (sub_code);
    report_ie.clear_flag = (ALARM_CLEAR_FLAG_CLEARED);

    /* 当前时间 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(report_ie.timestamp, sizeof(report_ie.timestamp),
             "%Y-%m-%d %H:%M:%S", tm_info);

    strcpy(report_ie.additional_info, "Alarm not found or already cleared");

    /* 构造CPRI消息 */
    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    response.header.msg_id = MSG_ALARM_REPORT_REQ_ACK;
    response.header.paau_id = req_header->paau_id;
    response.header.bbu_id = req_header->bbu_id;
    response.header.port_num = req_header->port_num;
    response.header.serial_num = req_header->serial_num;

    /* 使用栈上静态缓冲区，避免动态分配 */
    uint16_t ie_type = 1001;
    uint16_t ie_len = 4 + sizeof(ie_alarm_report_t);  /* IE头 + 数据 */
    response.payload_len = ie_len;

    uint8_t payload_buffer[256];  /* 足够容纳IE头(4) + ie_alarm_report_t(134) */
    response.payload = payload_buffer;

    /* 写入IE头（小端序） */
    memcpy(response.payload, &ie_type, 2);
    memcpy(response.payload + 2, &ie_len, 2);

    /* 写入IE数据 */
    memcpy(response.payload + 4, &report_ie, sizeof(ie_alarm_report_t));

    /* 调试：打印前16字节 */
    LOG_DEBUG("Invalid alarm IE header: type=%u(0x%04X), len=%u(0x%04X)",
             ie_type, ie_type, ie_len, ie_len);
    LOG_DEBUG("Invalid alarm IE bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
             response.payload[0], response.payload[1], response.payload[2], response.payload[3],
             response.payload[4], response.payload[5], response.payload[6], response.payload[7]);

    /* 编码并发送 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Invalid alarm response sent: code=%u, subcode=%u",
                 alarm_code, sub_code);
    }

    /* 无需free，使用的是栈缓冲区 */
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

/**
 * 处理告警查询请求
 */
int handle_alarm_query_request(const cpri_msg_header_t *req_header,
                                const ie_alarm_query_req_t *req_ie)
{
    if (!g_alarm_query_initialized) {
        LOG_ERROR("Alarm query module not initialized");
        return ERROR_NOT_INITIALIZED;
    }

    if (!req_header || !req_ie) {
        LOG_ERROR("Invalid parameter");
        return ERROR_INVALID_PARAM;
    }

    uint32_t query_code = (req_ie->alarm_code);
    uint32_t query_subcode = (req_ie->sub_code);

    LOG_INFO("========================================");
    LOG_INFO("Alarm Query Request Received");
    LOG_INFO("========================================");
    LOG_INFO("Query Code:    0x%08X %s", query_code,
             query_code == ALARM_QUERY_ALL ? "(ALL)" : "");
    LOG_INFO("Query Subcode: 0x%08X %s", query_subcode,
             query_subcode == ALARM_QUERY_ALL ? "(ALL)" : "");

    /* 获取所有活跃告警 */
    alarm_record_t alarms[MAX_ALARM_RECORDS];
    int alarm_count = 0;
    int ret = alarm_get_active_alarms(alarms, MAX_ALARM_RECORDS, &alarm_count);

    if (ret != SUCCESS) {
        LOG_ERROR("Failed to get active alarms");
        return ret;
    }

    LOG_INFO("Total active alarms: %d", alarm_count);

    /* 过滤并上报匹配的告警 */
    int reported_count = 0;
    int skipped_count = 0;

    for (int i = 0; i < alarm_count; i++) {
        alarm_record_t *alarm = &alarms[i];

        /* 跳过已清除的告警（双重保险） */
        if (!alarm->active) {
            skipped_count++;
            LOG_DEBUG("Skipping inactive alarm: code=%u, subcode=%u",
                     alarm->alarm_code, alarm->sub_code);
            continue;
        }

        /* 检查是否匹配查询条件 */
        if (!alarm_matches_query(alarm, query_code, query_subcode)) {
            LOG_DEBUG("Alarm does not match query: code=%u, subcode=%u",
                     alarm->alarm_code, alarm->sub_code);
            continue;
        }

        /* 时间戳校验：只上报最近24小时内的告警 */
        time_t now = time(NULL);
        double diff_seconds = difftime(now, alarm->start_time);
        if (diff_seconds > 86400) {  /* 24小时 = 86400秒 */
            LOG_WARN("Alarm too old, skipping: code=%u, age=%.0f seconds",
                    alarm->alarm_code, diff_seconds);
            skipped_count++;
            continue;
        }

        /* 发送告警上报 */
        ret = send_alarm_report_from_record(req_header, alarm);
        if (ret == SUCCESS) {
            reported_count++;
        } else {
            LOG_ERROR("Failed to send alarm report: code=%u, subcode=%u",
                     alarm->alarm_code, alarm->sub_code);
        }
    }

    LOG_INFO("========================================");
    LOG_INFO("Alarm Query Summary");
    LOG_INFO("========================================");
    LOG_INFO("Total alarms:    %d", alarm_count);
    LOG_INFO("Reported:        %d", reported_count);
    LOG_INFO("Skipped:         %d", skipped_count);
    LOG_INFO("========================================");

    /* 如果没有匹配的告警，必须发送无效响应（包括查询所有告警的情况） */
    if (reported_count == 0) {
        LOG_INFO("No matching alarms found, sending invalid response");
        send_invalid_alarm_response(req_header, query_code, query_subcode);
    }

    return SUCCESS;
}
