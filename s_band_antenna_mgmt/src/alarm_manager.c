/**
 * @file alarm_manager.c
 * @brief 告警管理模块实现 - 完整的告警检测、存储和上报功能
 */

#include "alarm_manager.h"
#include "tcp_client.h"
#include "logger.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

/* 全局告警管理器 */
static alarm_manager_t g_alarm_mgr;
static alarm_config_t g_alarm_config;

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;
extern uint32_t g_serial_num;
extern pthread_mutex_t g_serial_num_mutex;  /* 流水号互斥锁 */

/* 获取下一个流水号（线程安全） */
static inline uint32_t get_next_serial_num_safe(void)
{
    extern pthread_mutex_t g_serial_num_mutex;
    pthread_mutex_lock(&g_serial_num_mutex);
    uint32_t serial = ++g_serial_num;
    pthread_mutex_unlock(&g_serial_num_mutex);
    return serial;
}

/* 告警信息表 */
typedef struct {
    uint32_t alarm_code;
    const char *name;
    const char *description;
    alarm_attribute_t attribute;
} alarm_info_t;

static const alarm_info_t g_alarm_info[] = {
    {ALARM_CODE_LO_UNLOCK, "本振失锁告警", "相控阵自行关闭所有通道", ALARM_ATTR_FAULT},
    {ALARM_CODE_OPTICAL_SYNC_LOSS, "光链路同步码流丢失", "检查光纤连接", ALARM_ATTR_FAULT},
    {ALARM_CODE_OVER_TEMPERATURE, "过温告警", "监测点温度超过过温门限", ALARM_ATTR_FAULT},
    {ALARM_CODE_VERSION_ACTIVATE_FAIL, "版本激活失败", "BBU指定版本激活失败", ALARM_ATTR_FAULT},
    {ALARM_CODE_CHANNEL_FAULT_EXCEED, "通道故障过多", "通道故障数超过门限", ALARM_ATTR_FAULT},
    {ALARM_CODE_TCP_DISCONNECT, "TCP连接断开", "与BBU的TCP连接断开", ALARM_ATTR_FAULT},
    {0, NULL, NULL, ALARM_ATTR_FAULT}
};

/* 获取告警名称 */
const char* alarm_get_name(uint32_t alarm_code)
{
    for (int i = 0; g_alarm_info[i].name != NULL; i++) {
        if (g_alarm_info[i].alarm_code == alarm_code) {
            return g_alarm_info[i].name;
        }
    }
    return "未知告警";
}

/* 获取告警属性 */
alarm_attribute_t alarm_get_attribute(uint32_t alarm_code)
{
    for (int i = 0; g_alarm_info[i].name != NULL; i++) {
        if (g_alarm_info[i].alarm_code == alarm_code) {
            return g_alarm_info[i].attribute;
        }
    }
    return ALARM_ATTR_FAULT;
}


/* 初始化告警管理器 */
int alarm_manager_init(const alarm_config_t *config)
{
    memset(&g_alarm_mgr, 0, sizeof(g_alarm_mgr));

    if (config) {
        memcpy(&g_alarm_config, config, sizeof(alarm_config_t));
    } else {
        /* 默认配置 */
        g_alarm_config.channel_fault_threshold = 10;
        g_alarm_config.over_temp_threshold_high = 85;
        g_alarm_config.over_temp_threshold_low = -40;
    }

    if (pthread_mutex_init(&g_alarm_mgr.mutex, NULL) != 0) {
        LOG_ERROR("Failed to init alarm manager mutex");
        return ERROR_GENERAL;
    }

    g_alarm_mgr.tcp_connected = false;
    g_alarm_mgr.pending_tcp_alarm = false;

    LOG_INFO("Alarm manager initialized (fault_threshold=%u, temp_high=%d, temp_low=%d)",
             g_alarm_config.channel_fault_threshold,
             g_alarm_config.over_temp_threshold_high,
             g_alarm_config.over_temp_threshold_low);
    return SUCCESS;
}

/* 销毁告警管理器 */
void alarm_manager_destroy(void)
{
    pthread_mutex_destroy(&g_alarm_mgr.mutex);
    LOG_INFO("Alarm manager destroyed");
}

/* 设置TCP连接状态 */
void alarm_set_tcp_status(bool connected)
{
    pthread_mutex_lock(&g_alarm_mgr.mutex);

    bool prev_status = g_alarm_mgr.tcp_connected;
    g_alarm_mgr.tcp_connected = connected;

    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    if (connected && !prev_status) {
        /* TCP连接建立,上报所有待发送的告警 */
        LOG_INFO("TCP connected, reporting pending alarms");
        alarm_report_all_pending();
    }
}

/* 查找告警记录 */
static int find_alarm_record(uint32_t alarm_code, uint32_t sub_code)
{
    for (int i = 0; i < MAX_ALARM_RECORDS; i++) {
        if (g_alarm_mgr.records[i].active &&
            g_alarm_mgr.records[i].alarm_code == alarm_code &&
            g_alarm_mgr.records[i].sub_code == sub_code) {
            return i;
        }
    }
    return -1;
}

/* 查找空闲记录位置 */
static int find_free_record(void)
{
    for (int i = 0; i < MAX_ALARM_RECORDS; i++) {
        if (!g_alarm_mgr.records[i].active) {
            return i;
        }
    }
    return -1;
}


/* 触发告警 */
int alarm_trigger(uint32_t alarm_code, uint32_t sub_code, const char *additional_info)
{
    pthread_mutex_lock(&g_alarm_mgr.mutex);

    /* 检查告警是否已存在 */
    int idx = find_alarm_record(alarm_code, sub_code);
    if (idx >= 0) {
        /* 告警已存在,不重复触发 */
        LOG_DEBUG("Alarm already active: %s (code=%u, sub=%u)",
                  alarm_get_name(alarm_code), alarm_code, sub_code);
        pthread_mutex_unlock(&g_alarm_mgr.mutex);
        return SUCCESS;
    }

    /* 查找空闲位置 */
    idx = find_free_record();
    if (idx < 0) {
        LOG_ERROR("Alarm record table full");
        pthread_mutex_unlock(&g_alarm_mgr.mutex);
        return ERROR_GENERAL;
    }

    /* 记录告警 */
    alarm_record_t *record = &g_alarm_mgr.records[idx];
    record->active = true;
    record->alarm_code = alarm_code;
    record->sub_code = sub_code;
    record->start_time = time(NULL);
    record->clear_time = 0;
    record->reported = false;
    record->clear_reported = false;
    record->attribute = alarm_get_attribute(alarm_code);

    if (additional_info && strlen(additional_info) > 0) {
        strncpy(record->additional_info, additional_info, 99);
        record->additional_info[99] = '\0';
    } else {
        record->additional_info[0] = '\0';
    }

    g_alarm_mgr.alarm_count++;
    g_alarm_mgr.total_count++;

    LOG_WARN("Alarm triggered: %s (code=%u, sub=%u) - %s",
             alarm_get_name(alarm_code), alarm_code, sub_code,
             additional_info ? additional_info : "");

    bool tcp_connected = g_alarm_mgr.tcp_connected;
    int record_idx = idx;

    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    /* 暂时禁用告警上报 */
    if (tcp_connected) {
        alarm_report_to_bbu(alarm_code, sub_code, ALARM_CLEAR_FLAG_ACTIVE, additional_info);

        pthread_mutex_lock(&g_alarm_mgr.mutex);
        if (record_idx >= 0 && record_idx < MAX_ALARM_RECORDS) {
            g_alarm_mgr.records[record_idx].reported = true;
        }
        pthread_mutex_unlock(&g_alarm_mgr.mutex);
    } else {
        LOG_DEBUG("TCP not connected, alarm will be reported later");
        if (alarm_code == ALARM_CODE_TCP_DISCONNECT) {
            pthread_mutex_lock(&g_alarm_mgr.mutex);
            g_alarm_mgr.pending_tcp_alarm = true;
            pthread_mutex_unlock(&g_alarm_mgr.mutex);
        }
    }
    

    return SUCCESS;
}


/* 清除告警 */
int alarm_clear(uint32_t alarm_code, uint32_t sub_code)
{
    pthread_mutex_lock(&g_alarm_mgr.mutex);

    int idx = find_alarm_record(alarm_code, sub_code);
    if (idx < 0) {
        /* 告警不存在,无需清除 */
        LOG_DEBUG("Alarm not found for clearing: code=%u, sub=%u", alarm_code, sub_code);
        pthread_mutex_unlock(&g_alarm_mgr.mutex);
        return SUCCESS;
    }

    alarm_record_t *record = &g_alarm_mgr.records[idx];

    /* 记录清除时间 */
    record->clear_time = time(NULL);

    LOG_INFO("Alarm cleared: %s (code=%u, sub=%u)",
             alarm_get_name(alarm_code), alarm_code, sub_code);

    char additional_info[100];
    strncpy(additional_info, record->additional_info, 99);
    additional_info[99] = '\0';

    bool tcp_connected = g_alarm_mgr.tcp_connected;

    /* 清除告警记录 */
    record->active = false;
    g_alarm_mgr.alarm_count--;

    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    /* 上报告警清除 */
    if (tcp_connected) {
        alarm_report_to_bbu(alarm_code, sub_code, ALARM_CLEAR_FLAG_CLEARED, additional_info);
    }

    return SUCCESS;
}

/* 检查告警是否激活 */
bool alarm_is_active(uint32_t alarm_code, uint32_t sub_code)
{
    pthread_mutex_lock(&g_alarm_mgr.mutex);
    int idx = find_alarm_record(alarm_code, sub_code);
    pthread_mutex_unlock(&g_alarm_mgr.mutex);
    return (idx >= 0);
}


/* 上报告警到BBU */
int alarm_report_to_bbu(uint32_t alarm_code, uint32_t sub_code,
                        uint32_t clear_flag, const char *additional_info)
{
    LOG_DEBUG("[TRACE] alarm_report_to_bbu: ENTER (code=%u, sub=%u)", alarm_code, sub_code);

    /* 构造告警上报消息 */
    cpri_message_t msg;
    memset(&msg, 0, sizeof(msg));

    /* 填充消息头 */
    msg.header.msg_id = MSG_ALARM_REPORT_REQ;
    msg.header.paau_id = 0;
    msg.header.bbu_id = 0;
    msg.header.port_num = 0;
    msg.header.serial_num = get_next_serial_num_safe();

    LOG_DEBUG("[TRACE] alarm_report_to_bbu: got serial_num=%u", msg.header.serial_num);

    /* 构造告警上报IE (IE 1001, 134字节payload) */
    alarm_report_ie_t ie_data;
    memset(&ie_data, 0, sizeof(ie_data));

    ie_data.validity = ALARM_VALIDITY_VALID;
    ie_data.alarm_code = alarm_code;
    ie_data.sub_code = sub_code;
    ie_data.clear_flag = clear_flag;

    /* 生成时间戳 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    snprintf(ie_data.timestamp, sizeof(ie_data.timestamp),
             "%04d-%02d-%02d %02d:%02d:%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    /* 填充附加信息 */
    if (additional_info && strlen(additional_info) > 0) {
        strncpy(ie_data.additional_info, additional_info, 99);
        ie_data.additional_info[99] = '\0';
    }

    /* 构造IE头和数据 */
    uint16_t ie_type = IE_TYPE_ALARM_REPORT;
    uint16_t ie_len = 4 + sizeof(alarm_report_ie_t);  /* IE头(4) + 数据(134) */

    /* 使用栈上静态缓冲区，避免动态分配 */
    uint8_t payload_buffer[256];  /* 足够容纳IE头(4) + alarm_report_ie_t(134) */
    msg.payload_len = ie_len;
    msg.payload = payload_buffer;

    /* 填充payload */
    uint32_t offset = 0;
    memcpy(msg.payload + offset, &ie_type, 2);
    offset += 2;
    memcpy(msg.payload + offset, &ie_len, 2);
    offset += 2;
    memcpy(msg.payload + offset, &ie_data, sizeof(alarm_report_ie_t));

    /* 编码并发送 */
    uint8_t buffer[512];
    int len = cpri_encode_message(&msg, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent alarm report: %s (code=%u, sub=%u, clear=%u, serial=%u)",
                 alarm_get_name(alarm_code), alarm_code, sub_code, clear_flag, msg.header.serial_num);
    } else {
        LOG_ERROR("Failed to encode alarm report message");
    }

    /* 无需cpri_free_message，使用的是栈缓冲区 */
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

/* 上报所有未上报的告警 */
int alarm_report_all_pending(void)
{
    int count = 0;

    pthread_mutex_lock(&g_alarm_mgr.mutex);

    for (int i = 0; i < MAX_ALARM_RECORDS; i++) {
        alarm_record_t *record = &g_alarm_mgr.records[i];

        if (record->active && !record->reported) {
            /* 上报激活的告警 */
            pthread_mutex_unlock(&g_alarm_mgr.mutex);

            int ret = alarm_report_to_bbu(record->alarm_code, record->sub_code,
                                          ALARM_CLEAR_FLAG_ACTIVE, record->additional_info);

            pthread_mutex_lock(&g_alarm_mgr.mutex);

            if (ret == SUCCESS) {
                record->reported = true;
                count++;
            }
        }
    }

    /* 清除TCP断开告警的待发送标志 */
    g_alarm_mgr.pending_tcp_alarm = false;

    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    if (count > 0) {
        LOG_INFO("Reported %d pending alarms", count);
    }

    return count;
}


/* ========== 特定告警检测函数 ========== */

/* 检测本振失锁告警 */
void alarm_check_lo_unlock(uint32_t lo_lock_status)
{
    pthread_mutex_lock(&g_alarm_mgr.mutex);
    bool prev_state = g_alarm_mgr.state_tracker.lo_unlock_active;
    bool current_state = (lo_lock_status != 0);  /* FPGA: 0=锁定, 1=失锁 */
    g_alarm_mgr.state_tracker.lo_unlock_active = current_state;
    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    if (current_state && !prev_state) {
        /* 本振失锁 */
        alarm_trigger(ALARM_CODE_LO_UNLOCK, 0, "LO unlock detected");
    } else if (!current_state && prev_state) {
        /* 本振恢复锁定 */
        alarm_clear(ALARM_CODE_LO_UNLOCK, 0);
    }
}

/* 检测光链路同步丢失告警 */
void alarm_check_optical_sync_loss(uint8_t optical_port, bool sync_status)
{
    if (optical_port >= 8) {
        LOG_ERROR("Invalid optical port: %u", optical_port);
        return;
    }

    pthread_mutex_lock(&g_alarm_mgr.mutex);
    bool prev_state = g_alarm_mgr.state_tracker.optical_sync_loss[optical_port];
    bool current_state = !sync_status;  /* true表示丢失 */
    g_alarm_mgr.state_tracker.optical_sync_loss[optical_port] = current_state;
    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    if (current_state && !prev_state) {
        /* 光链路同步丢失 */
        char info[100];
        snprintf(info, sizeof(info), "Optical port %u sync lost", optical_port);
        alarm_trigger(ALARM_CODE_OPTICAL_SYNC_LOSS, optical_port, info);
    } else if (!current_state && prev_state) {
        /* 光链路同步恢复 */
        alarm_clear(ALARM_CODE_OPTICAL_SYNC_LOSS, optical_port);
    }
}

/* 检测过温告警 */
void alarm_check_over_temperature(uint8_t probe_id, int8_t temperature)
{
    if (probe_id >= 10) {
        LOG_ERROR("Invalid temperature probe ID: %u", probe_id);
        return;
    }

    pthread_mutex_lock(&g_alarm_mgr.mutex);
    bool prev_state = g_alarm_mgr.state_tracker.over_temperature[probe_id];
    bool current_state = (temperature > g_alarm_config.over_temp_threshold_high ||
                          temperature < g_alarm_config.over_temp_threshold_low);
    g_alarm_mgr.state_tracker.over_temperature[probe_id] = current_state;
    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    if (current_state && !prev_state) {
        /* 过温告警 */
        char info[100];
        snprintf(info, sizeof(info), "Probe %u temp=%d°C (threshold: %d~%d°C)",
                 probe_id, temperature,
                 g_alarm_config.over_temp_threshold_low,
                 g_alarm_config.over_temp_threshold_high);
        alarm_trigger(ALARM_CODE_OVER_TEMPERATURE, probe_id, info);
    } else if (!current_state && prev_state) {
        /* 温度恢复正常 */
        alarm_clear(ALARM_CODE_OVER_TEMPERATURE, probe_id);
    }
}

/* 检测通道故障过多告警 */
void alarm_check_channel_fault_exceed(uint8_t fault_count)
{
    pthread_mutex_lock(&g_alarm_mgr.mutex);
    bool prev_state = g_alarm_mgr.state_tracker.channel_fault_exceed_active;
    bool current_state = (fault_count > g_alarm_config.channel_fault_threshold);
    g_alarm_mgr.state_tracker.channel_fault_exceed_active = current_state;
    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    if (current_state && !prev_state) {
        /* 通道故障过多 */
        char info[100];
        snprintf(info, sizeof(info), "Channel fault count=%u (threshold: %u)",
                 fault_count, g_alarm_config.channel_fault_threshold);
        alarm_trigger(ALARM_CODE_CHANNEL_FAULT_EXCEED, 0, info);
    } else if (!current_state && prev_state) {
        /* 通道故障恢复 */
        alarm_clear(ALARM_CODE_CHANNEL_FAULT_EXCEED, 0);
    }
}

/* 触发版本激活失败告警 */
void alarm_trigger_version_activate_fail(const char *version_info)
{
    char info[100];
    snprintf(info, sizeof(info), "Version activation failed: %s",
             version_info ? version_info : "unknown");
    alarm_trigger(ALARM_CODE_VERSION_ACTIVATE_FAIL, 0, info);
}

/* 触发TCP断开告警 */
void alarm_trigger_tcp_disconnect(const char *reason)
{
    pthread_mutex_lock(&g_alarm_mgr.mutex);
    bool prev_state = g_alarm_mgr.state_tracker.tcp_disconnect_active;
    g_alarm_mgr.state_tracker.tcp_disconnect_active = true;
    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    if (!prev_state) {
        char info[100];
        snprintf(info, sizeof(info), "TCP disconnected: %s",
                 reason ? reason : "unknown reason");
        alarm_trigger(ALARM_CODE_TCP_DISCONNECT, 0, info);
    }
}

/* 清除TCP断开告警 */
void alarm_clear_tcp_disconnect(void)
{
    pthread_mutex_lock(&g_alarm_mgr.mutex);
    bool prev_state = g_alarm_mgr.state_tracker.tcp_disconnect_active;
    g_alarm_mgr.state_tracker.tcp_disconnect_active = false;
    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    if (prev_state) {
        alarm_clear(ALARM_CODE_TCP_DISCONNECT, 0);
    }
}

/* 周期性告警检测任务(从FPGA遥测数据检测) */
void alarm_periodic_check(const fpga_status_frame_t *fpga_status)
{
    if (!fpga_status) {
        return;
    }

    /* 1. 检查本振失锁告警 (告警码: 50001) */
    alarm_check_lo_unlock(fpga_status->data.lo_lock_status);

    /* 2. 检查过温告警 (告警码: 1156, 子码: 温度检测点0-9) */
    alarm_check_over_temperature(0, fpga_status->data.temp1);
    alarm_check_over_temperature(1, fpga_status->data.temp2);
    alarm_check_over_temperature(2, fpga_status->data.temp3);

    /* 3. 检查通道故障过多告警 (告警码: 1159) */
    alarm_check_channel_fault_exceed(fpga_status->data.channel_fault_count);

    /* 4. 检查光链路同步码流丢失 (告警码: 1097, 子码: 光口号0-7) */
    for (int i = 0; i < 4; i++) {
        bool main_sync = (fpga_status->data.link_success_flag & (1 << i)) != 0;
        alarm_check_optical_sync_loss(i, main_sync);

        // bool backup_sync = (fpga_status->data.link_success_flag & (1 << (i + 4))) != 0;
        // alarm_check_optical_sync_loss(i + 4, backup_sync);
    }
}

/**
 * 获取所有活跃告警列表
 */
int alarm_get_active_alarms(alarm_record_t *records, int max_count, int *actual_count)
{
    if (!records || !actual_count || max_count <= 0) {
        return ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_alarm_mgr.mutex);

    int count = 0;
    for (int i = 0; i < MAX_ALARM_RECORDS && count < max_count; i++) {
        if (g_alarm_mgr.records[i].active) {
            memcpy(&records[count], &g_alarm_mgr.records[i], sizeof(alarm_record_t));
            count++;
        }
    }

    *actual_count = count;

    pthread_mutex_unlock(&g_alarm_mgr.mutex);

    return SUCCESS;
}
