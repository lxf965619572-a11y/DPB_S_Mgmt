#include "fpga_to_cpri.h"
#include "logger.h"

/* 预分配缓冲区大小 */
#define MAX_PAYLOAD_SIZE 4096

/* IE类型定义（根据CPRI协议） */
#define IE_TYPE_BEAM_STATUS         353
#define IE_TYPE_LO_STATUS           354
#define IE_TYPE_CLOCK_STATUS        355
#define IE_TYPE_RUN_STATUS          356
#define IE_TYPE_CPRI_MODE           357
#define IE_TYPE_TEMP                453
#define IE_TYPE_POWER               457
#define IE_TYPE_TOFFSET             458
#define IE_TYPE_T2A                 459
#define IE_TYPE_TA3                 460

/* 添加IE到载荷 */
static int add_ie_to_payload(uint8_t **payload, uint32_t *offset, uint16_t ie_type,
                             const uint8_t *ie_data, uint16_t ie_data_len)
{
    uint16_t ie_len = 4 + ie_data_len;  /* IE头(4字节) + 数据 */

    /* 首次分配时创建缓冲区 */
    if (*payload == NULL) {
        *payload = (uint8_t*)malloc(MAX_PAYLOAD_SIZE);
        if (!*payload) {
            LOG_ERROR("Failed to allocate payload buffer");
            return ERROR_MEMORY;
        }
        *offset = 0;
    }

    /* 检查缓冲区是否足够 */
    if (*offset + ie_len > MAX_PAYLOAD_SIZE) {
        LOG_ERROR("Payload buffer overflow: need %u, max %u",
                  *offset + ie_len, MAX_PAYLOAD_SIZE);
        return ERROR_MEMORY;
    }

    /* 写入IE头（小端序） */
    memcpy(*payload + *offset, &ie_type, 2);
    *offset += 2;
    memcpy(*payload + *offset, &ie_len, 2);
    *offset += 2;

    /* 写入IE数据 */
    if (ie_data_len > 0 && ie_data) {
        memcpy(*payload + *offset, ie_data, ie_data_len);
        *offset += ie_data_len;
    }

    return SUCCESS;
}

int fpga_to_cpri_status_response(const fpga_status_frame_t *fpga_status, cpri_message_t *cpri_msg)
{
    if (!fpga_status || !cpri_msg) {
        return ERROR_INVALID_PARAM;
    }

    memset(cpri_msg, 0, sizeof(cpri_message_t));

    /* 填充CPRI消息头 */
    cpri_msg->header.msg_id = MSG_PAAU_STATUS_QUERY_RSP;
    cpri_msg->header.paau_id = 0;
    cpri_msg->header.bbu_id = 0;
    cpri_msg->header.port_num = fpga_status->data.main_fiber_num;
    cpri_msg->header.serial_num = 0;  /* 需要从请求中获取 */

    /* 构建载荷（包含多个IE） */
    uint8_t *payload = NULL;
    uint32_t offset = 0;

    /* IE: 波束状态 */
    uint8_t beam_status_data[6];
    beam_status_data[0] = 0;  /* 保留 */
    beam_status_data[1] = 0;  /* 波束号 */
    uint32_t beam_status = 0;  /* 从fpga_status->data.beam_status提取 */
    memcpy(beam_status_data + 2, &beam_status, 4);
    add_ie_to_payload(&payload, &offset, IE_TYPE_BEAM_STATUS, beam_status_data, sizeof(beam_status_data));

    /* IE: 本振状态 */
    uint8_t lo_status_data[8];
    uint32_t lo_freq_khz = fpga_decode_frequency(fpga_status->data.lo_frequency);
    uint32_t lo_freq_100khz = lo_freq_khz / 100;  /* 转换为100kHz单位 */
    memcpy(lo_status_data, &lo_freq_100khz, 4);
    uint32_t lo_status = fpga_status->data.lo_lock_status;
    memcpy(lo_status_data + 4, &lo_status, 4);
    add_ie_to_payload(&payload, &offset, IE_TYPE_LO_STATUS, lo_status_data, sizeof(lo_status_data));

    /* IE: 时钟状态 */
    uint32_t clock_status = fpga_status->data.clock_sync_status;
    add_ie_to_payload(&payload, &offset, IE_TYPE_CLOCK_STATUS, (uint8_t*)&clock_status, 4);

    /* IE: 运行状态 */
    uint32_t run_status = fpga_status->data.phased_array_work_mode;
    add_ie_to_payload(&payload, &offset, IE_TYPE_RUN_STATUS, (uint8_t*)&run_status, 4);

    /* IE: CPRI工作模式 */
    uint32_t cpri_mode = fpga_status->data.cpri_work_mode;
    add_ie_to_payload(&payload, &offset, IE_TYPE_CPRI_MODE, (uint8_t*)&cpri_mode, 4);

    cpri_msg->payload = payload;
    cpri_msg->payload_len = offset;

    LOG_DEBUG("Converted FPGA status to CPRI status response (%u bytes)", offset);
    return SUCCESS;
}

int fpga_to_cpri_param_response(const fpga_status_frame_t *fpga_status, cpri_message_t *cpri_msg)
{
    if (!fpga_status || !cpri_msg) {
        return ERROR_INVALID_PARAM;
    }

    memset(cpri_msg, 0, sizeof(cpri_message_t));

    /* 填充CPRI消息头 */
    cpri_msg->header.msg_id = MSG_PAAU_PARAM_QUERY_RSP;
    cpri_msg->header.paau_id = 0;
    cpri_msg->header.bbu_id = 0;
    cpri_msg->header.port_num = fpga_status->data.main_fiber_num;
    cpri_msg->header.serial_num = 0;

    /* 构建载荷 */
    uint8_t *payload = NULL;
    uint32_t offset = 0;

    /* IE: 温度（3个温度点） */
    for (int i = 0; i < 3; i++) {
        uint8_t temp_data[2];
        temp_data[0] = i + 1;  /* 温度点编号 */

        if (i == 0) temp_data[1] = fpga_status->data.temp1;
        else if (i == 1) temp_data[1] = fpga_status->data.temp2;
        else temp_data[1] = fpga_status->data.temp3;

        add_ie_to_payload(&payload, &offset, IE_TYPE_TEMP, temp_data, sizeof(temp_data));
    }

    /* IE: 输出功率 */
    uint8_t power_data[3];
    power_data[0] = 0;  /* 保留 */
    memcpy(power_data + 1, &fpga_status->data.current_output_power, 2);
    add_ie_to_payload(&payload, &offset, IE_TYPE_POWER, power_data, sizeof(power_data));

    /* IE: Toffset */
    add_ie_to_payload(&payload, &offset, IE_TYPE_TOFFSET, (uint8_t*)&fpga_status->data.toffset, 4);

    /* IE: T2a */
    add_ie_to_payload(&payload, &offset, IE_TYPE_T2A, (uint8_t*)&fpga_status->data.t2a, 4);

    /* IE: Ta3 */
    add_ie_to_payload(&payload, &offset, IE_TYPE_TA3, (uint8_t*)&fpga_status->data.ta3, 4);

    cpri_msg->payload = payload;
    cpri_msg->payload_len = offset;

    LOG_DEBUG("Converted FPGA status to CPRI param response (%u bytes)", offset);
    return SUCCESS;
}

int fpga_to_cpri_alarm_report(const fpga_status_frame_t *fpga_status, cpri_message_t *cpri_msg)
{
    if (!fpga_status || !cpri_msg) {
        return ERROR_INVALID_PARAM;
    }

    /* 检查是否有告警 */
    bool has_alarm = false;

    /* 检查温度告警 */
    if (fpga_status->data.temp1 > fpga_status->data.temp1_high || fpga_status->data.temp1 < fpga_status->data.temp1_low) {
        has_alarm = true;
    }
    if (fpga_status->data.temp2 > fpga_status->data.temp2_high || fpga_status->data.temp2 < fpga_status->data.temp2_low) {
        has_alarm = true;
    }
    if (fpga_status->data.temp3 > fpga_status->data.temp3_high || fpga_status->data.temp3 < fpga_status->data.temp3_low) {
        has_alarm = true;
    }

    /* 检查本振失锁告警 */
    if (fpga_status->data.lo_lock_status != 0) {
        has_alarm = true;
    }

    /* 检查时钟失步告警 */
    if (fpga_status->data.clock_sync_status != 0) {
        has_alarm = true;
    }

    if (!has_alarm) {
        return ERROR_GENERAL;  /* 无告警 */
    }

    memset(cpri_msg, 0, sizeof(cpri_message_t));

    /* 填充CPRI消息头 */
    cpri_msg->header.msg_id = MSG_ALARM_REPORT_REQ;
    cpri_msg->header.paau_id = 0;
    cpri_msg->header.bbu_id = 0;
    cpri_msg->header.port_num = fpga_status->data.main_fiber_num;
    cpri_msg->header.serial_num = 0;

    /* 构建告警IE（简化版本） */
    uint8_t *payload = NULL;
    uint32_t offset = 0;

    /* IE: 告警上报 (IE type 1001) */
    uint8_t alarm_data[134];
    memset(alarm_data, 0, sizeof(alarm_data));

    uint16_t alarm_valid = 1;
    memcpy(alarm_data, &alarm_valid, 2);

    uint32_t alarm_code = 0x1001;  /* 示例告警码 */
    memcpy(alarm_data + 2, &alarm_code, 4);

    uint32_t alarm_subcode = 0;
    memcpy(alarm_data + 6, &alarm_subcode, 4);

    uint32_t alarm_clear = 0;  /* 0=产生告警 */
    memcpy(alarm_data + 10, &alarm_clear, 4);

    /* 时间戳 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[20];
    snprintf(timestamp, sizeof(timestamp), "%04d%02d%02d%02d%02d%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    memcpy(alarm_data + 14, timestamp, 20);

    /* 附加信息 */
    snprintf((char*)alarm_data + 34, 100, "FPGA Status Alarm");

    add_ie_to_payload(&payload, &offset, 1001, alarm_data, sizeof(alarm_data));

    cpri_msg->payload = payload;
    cpri_msg->payload_len = offset;

    LOG_INFO("Generated CPRI alarm report from FPGA status");
    return SUCCESS;
}
