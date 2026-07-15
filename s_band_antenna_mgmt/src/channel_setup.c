#include "channel_setup.h"
#include "config.h"
#include "logger.h"
#include "fpga_handler.h"
#include <time.h>
#include <errno.h>
#include <string.h>

/* IE类型定义 */
#define IE_TYPE_PRODUCT_ID          1
#define IE_TYPE_SETUP_REASON        2
#define IE_TYPE_PHASED_ARRAY_CAP    3
#define IE_TYPE_HARDWARE_INFO       5
#define IE_TYPE_SOFTWARE_VERSION    6
#define IE_TYPE_FREQ_BAND_CAP       7
#define IE_TYPE_SYSTEM_TIME         11
#define IE_TYPE_OPERATION_MODE      13
#define IE_TYPE_VERSION_CHECK       14
#define IE_TYPE_PHASED_ARRAY_BEAM   15
#define IE_TYPE_CPU_STAT_PERIOD     502
#define IE_TYPE_CPRI_WORK_MODE      504
#define IE_TYPE_SETUP_RESPONSE      21

/* 预分配缓冲区大小 */
#define MAX_PAYLOAD_SIZE 4096

/* 添加IE到载荷 */
static int add_ie(uint8_t **payload, uint32_t *offset, uint16_t ie_type,
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

/* 生成通道建立请求消息 */
int channel_setup_create_request(cpri_message_t *msg, channel_setup_reason_t reason)
{
    if (!msg) {
        return ERROR_INVALID_PARAM;
    }

    memset(msg, 0, sizeof(cpri_message_t));

    /* 填充消息头 */
    msg->header.msg_id = MSG_CHANNEL_SETUP_REQ;
    msg->header.paau_id = config_get_int("PAAU_ID", 0);
    msg->header.bbu_id = 0;
    msg->header.port_num = 0;
    msg->header.serial_num = 0;

    uint8_t *payload = NULL;
    uint32_t offset = 0;

    /* IE 1: 相控阵产品标识 (96字节) */
    uint8_t product_id[96];
    memset(product_id, 0, sizeof(product_id));

    const char *manufacturer = config_get_string("PRODUCT_MANUFACTURER", "StarNet");
    const char *product_name = config_get_string("PRODUCT_NAME", "S-Band PAAU");
    const char *serial = config_get_string("PRODUCT_SERIAL", "SN00000000");
    const char *date = config_get_string("PRODUCT_DATE", "2026-01-01");
    const char *service_date = config_get_string("PRODUCT_SERVICE_DATE", "2026-01-01");
    const char *info = config_get_string("PRODUCT_INFO", "");

    strncpy((char*)product_id, manufacturer, 16);
    strncpy((char*)product_id + 16, product_name, 16);
    strncpy((char*)product_id + 32, serial, 16);
    strncpy((char*)product_id + 48, date, 16);
    strncpy((char*)product_id + 64, service_date, 16);
    strncpy((char*)product_id + 80, info, 16);

    add_ie(&payload, &offset, IE_TYPE_PRODUCT_ID, product_id, sizeof(product_id));

    /* IE 2: 通道建立原因 (9字节) */
    uint8_t setup_reason[5];
    memset(setup_reason, 0, sizeof(setup_reason));
    setup_reason[0] = (uint8_t)reason;
    /* alarm_code (4 bytes) = 0 */
    /* reserved (4 bytes) = 0 */
    add_ie(&payload, &offset, IE_TYPE_SETUP_REASON, setup_reason, sizeof(setup_reason));

    /* IE 3: 相控阵能力 (31字节) - 从FPGA状态获取 */
    uint8_t phased_cap[27];
    memset(phased_cap, 0, sizeof(phased_cap));

    /* IE 5: 硬件信息 (48字节) - 从FPGA状态获取 */
    uint8_t hw_info[48];
    memset(hw_info, 0, sizeof(hw_info));

    /* 一次性获取FPGA状态，避免重复调用 */
    fpga_status_frame_t fpga_status;
    memset(&fpga_status, 0, sizeof(fpga_status));
    int fpga_status_ret = fpga_handler_get_status(&fpga_status);

    if (fpga_status_ret == SUCCESS) {
        /* 填充IE 3: 相控阵能力 (27字节数据部分)
         * 协议定义：IE长度=31BYTE (包含IE头4字节)，数据部分=27字节
         * Byte 0-3:   支持的NR波束个数 (Unsigned long, 4BYTE)
         * Byte 4-7:   Reserve (Unsigned long, 4BYTE)
         * Byte 8:     Reserve (Unsigned char, 1BYTE)
         * Byte 9-10:  最大发射功率 (Unsigned short, 2BYTE, 1/256dBm)
         * Byte 11:    Reserve (Unsigned char, 1BYTE)
         * Byte 12-15: Reserve (Unsigned long, 4BYTE)
         * Byte 16-19: Reserve (Unsigned long, 4BYTE)
         * Byte 20-21: 支持的模式 (Unsigned short, 2BYTE)
         * Byte 22:    Reserve (Unsigned char, 1BYTE)
         * Byte 23:    Reserve (Unsigned char, 1BYTE)
         * Byte 24-26: Reserve (3BYTE，补齐到27字节)
         */
        memcpy(phased_cap + 0, &fpga_status.data.nr_beam_count, 4);
        /* Byte 4-8: Reserve = 0 (已经memset清零) */
        memcpy(phased_cap + 9, &fpga_status.data.max_tx_power, 2);
        /* Byte 11-19: Reserve = 0 (已经memset清零) */
        memcpy(phased_cap + 20, &fpga_status.data.supported_modes, 2);
        /* Byte 22-26: Reserve = 0 (已经memset清零) */

        /* 填充IE 5: 硬件信息 (48字节)
         * Byte 0-31: 相控阵硬件类型 (32BYTE)
         * Byte 32-47: 相控阵硬件版本号 (16BYTE)
         */
        memcpy(hw_info, fpga_status.data.hw_type, 32);
        /* 硬件版本：fpga_status.data.hw_version 是4字节，需要格式化为16字节字符串 */
        snprintf((char*)hw_info + 32, 16, "HW_v%u", fpga_status.data.hw_version);
    } else {
        /* FPGA状态未就绪，使用默认值 */
        uint32_t default_beam_count = 16;
        uint16_t default_max_power = 46 * 256;  /* 46 dBm */
        uint16_t default_modes = 0x03;  /* Bit0: 支持TDD-LTE, Bit1: 支持TDD-NR */

        memcpy(phased_cap + 0, &default_beam_count, 4);
        memcpy(phased_cap + 9, &default_max_power, 2);
        memcpy(phased_cap + 20, &default_modes, 2);

        strncpy((char*)hw_info, "S-Band Phased Array", 32);
        strncpy((char*)hw_info + 32, "HW_v1.0", 16);
    }

    add_ie(&payload, &offset, IE_TYPE_PHASED_ARRAY_CAP, phased_cap, sizeof(phased_cap));
    add_ie(&payload, &offset, IE_TYPE_HARDWARE_INFO, hw_info, sizeof(hw_info));

    /* IE 6: 软件版本信息 (80字节) */
    uint8_t sw_version[80];
    memset(sw_version, 0, sizeof(sw_version));

    const char *sw_ver = config_get_string("SOFTWARE_VERSION", "v1.0.0");
    const char *fw_ver = config_get_string("FIRMWARE_VERSION", "FPGA_v1.0.0");

    strncpy((char*)sw_version, sw_ver, 40);
    strncpy((char*)sw_version + 40, fw_ver, 40);
    add_ie(&payload, &offset, IE_TYPE_SOFTWARE_VERSION, sw_version, sizeof(sw_version));

    /* IE 7: 频段能力 (11字节) */
    uint8_t freq_cap[11];
    memset(freq_cap, 0, sizeof(freq_cap));

    /* S频段: 2.0-2.3 GHz (发送), 2.0-2.3 GHz (接收) */
    /* 频率单位: 100kHz */
    uint16_t tx_start = 21200;  /* 发射2.12 GHz */
    uint16_t tx_end = 21900;    /* 发射2.19 GHz */
    uint16_t rx_start = 19300;  /* 接收1.93 GHz*/
    uint16_t rx_end = 20100;    /* 接收2.01 GHz*/
    uint16_t bandwidth = 100;   /* 20 MHz (单位200kHz) */
    uint8_t wave_count = 16;

    memcpy(freq_cap, &tx_start, 2);
    memcpy(freq_cap + 2, &tx_end, 2);
    memcpy(freq_cap + 4, &rx_start, 2);
    memcpy(freq_cap + 6, &rx_end, 2);
    memcpy(freq_cap + 8, &bandwidth, 2);
    freq_cap[10] = wave_count;
    add_ie(&payload, &offset, IE_TYPE_FREQ_BAND_CAP, freq_cap, sizeof(freq_cap));

    msg->payload = payload;
    msg->payload_len = offset;

    LOG_INFO("Created channel setup request: reason=%u, payload_len=%u", reason, offset);
    return SUCCESS;
}

/* 处理通道建立配置消息 */
int channel_setup_handle_config(const cpri_message_t *msg)
{
    if (!msg || !msg->payload) {
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("Handling channel setup config message");

    uint32_t offset = 0;
    while (offset + 4 <= msg->payload_len) {
        /* 解析IE头 */
        uint16_t ie_type, ie_len;
        memcpy(&ie_type, msg->payload + offset, 2);
        memcpy(&ie_len, msg->payload + offset + 2, 2);

        if (offset + ie_len > msg->payload_len) {
            LOG_WARN("Invalid IE length: type=%u, len=%u", ie_type, ie_len);
            break;
        }

        uint8_t *ie_data = msg->payload + offset + 4;
        uint16_t ie_data_len = ie_len - 4;

        // LOG_DEBUG("Processing IE: type=%u, len=%u", ie_type, ie_len);

        switch (ie_type) {
            case IE_TYPE_SYSTEM_TIME: {
                /* 系统时间配置 (IE 11) - 格式: 秒/分/时/日/月/年(2字节) */
                if (ie_data_len >= 7) {
                    uint8_t sec = ie_data[0];
                    uint8_t min = ie_data[1];
                    uint8_t hour = ie_data[2];
                    uint8_t day = ie_data[3];
                    uint8_t month = ie_data[4];
                    uint16_t year;
                    memcpy(&year, ie_data + 5, 2);

                    LOG_INFO("System time IE received: %04d-%02d-%02d %02d:%02d:%02d",
                             year, month, day, hour, min, sec);

                    /* 设置Linux系统时间 */
                    struct tm tm_time = {0};
                    tm_time.tm_sec = sec;
                    tm_time.tm_min = min;
                    tm_time.tm_hour = hour;
                    tm_time.tm_mday = day;
                    tm_time.tm_mon = month - 1;  /* tm_mon: 0-11 */
                    tm_time.tm_year = year - 1900;  /* tm_year: years since 1900 */

                    time_t new_time = mktime(&tm_time);
                    if (new_time != -1) {
                        struct timespec ts;
                        ts.tv_sec = new_time;
                        ts.tv_nsec = 0;

                        if (clock_settime(CLOCK_REALTIME, &ts) == 0) {
                            LOG_INFO("System time set successfully");
                        } else {
                            LOG_ERROR("Failed to set system time: %s", strerror(errno));
                        }
                    } else {
                        LOG_ERROR("Invalid time format");
                    }
                }
                break;
            }

            case IE_TYPE_OPERATION_MODE: {
                /* 相控阵操作模式配置 (IE 13) - 状态值(4字节) */
                if (ie_data_len >= 4) {
                    uint32_t mode;
                    memcpy(&mode, ie_data, 4);
                    LOG_INFO("Operation mode configured: %u (0=业务, 1=频谱监测, 2=待机, 3=自校准)", mode);
                    /* 发送操作模式到FPGA */
                    fpga_send_cpri_config(mode);
                }
                break;
            }

            case IE_TYPE_VERSION_CHECK: {
                /* 软件版本核对结果 (IE 14) - 类型(1字节) + 其他字段 */
                if (ie_data_len >= 1) {
                    uint8_t version_type = ie_data[0];
                    LOG_INFO("Version check IE received: type=%u (0=软件, 1=固件)", version_type);

                    /* 如果是固件版本核对结果，且长度足够，解析详细信息 */
                    if (ie_data_len >= 285 && version_type == 1) {
                        /* 返回结果(4字节) */
                        uint32_t result;
                        memcpy(&result, ie_data + 1, 4);

                        /* 文件路径(200字节) */
                        char file_path[201] = {0};
                        memcpy(file_path, ie_data + 5, 200);

                        /* 文件名(16字节) */
                        char file_name[17] = {0};
                        memcpy(file_name, ie_data + 205, 16);

                        /* 文件长度(4字节) */
                        uint32_t file_len;
                        memcpy(&file_len, ie_data + 221, 4);

                        /* 文件时间信息(20字节) */
                        char file_time[21] = {0};
                        memcpy(file_time, ie_data + 225, 20);

                        /* 文件版本(40字节) */
                        char file_version[41] = {0};
                        memcpy(file_version, ie_data + 245, 40);

                        LOG_INFO("Firmware version check: result=%u, file=%s, version=%s",
                                 result, file_name, file_version);

                        if (result == 0) {
                            LOG_INFO("Firmware version matches");
                        } else {
                            LOG_WARN("Firmware version mismatch, may need update");
                        }
                    }
                }
                break;
            }

            case IE_TYPE_PHASED_ARRAY_BEAM: {
                /* 相控阵波束配置 (IE 15) - n1(1字节) + n2(1字节) */
                if (ie_data_len >= 2) {
                    uint8_t cmd_beam_num = ie_data[0];  /* 信令波束个数 n1 */
                    uint8_t service_beam_num = ie_data[1];  /* 业务波束个数 n2 */

                    LOG_INFO("Phased array beam config: cmd_beams=%u, service_beams=%u",
                             cmd_beam_num, service_beam_num);
                    LOG_INFO("  Command beam IDs: 0 to %u", cmd_beam_num > 0 ? cmd_beam_num - 1 : 0);
                    LOG_INFO("  Service beam IDs: %u to %u",
                             cmd_beam_num, cmd_beam_num + service_beam_num - 1);

                    /* 发送波束数配置到FPGA */
                    fpga_send_beam_num(cmd_beam_num, service_beam_num);
                } else {
                    LOG_WARN("Invalid phased array beam IE length: %u (expected >= 2)", ie_data_len);
                }
                break;
            }

            case IE_TYPE_CPU_STAT_PERIOD: {
                /* CPU占用率统计周期配置 (IE 502) */
                if (ie_data_len >= 4) {
                    uint32_t period;
                    memcpy(&period, ie_data, 4);
                    LOG_INFO("CPU stat period configured: %u seconds", period);
                    /* TODO: 实现CPU统计周期配置 */
                }
                break;
            }

            case IE_TYPE_CPRI_WORK_MODE: {
                /* CPRI口工作模式配置 (IE 504) */
                if (ie_data_len >= 4) {
                    uint32_t work_mode;
                    memcpy(&work_mode, ie_data, 4);

                    LOG_INFO("CPRI work mode configured: mode=%u (1=普通, 2=级连, 3=主备, 4=负荷分担)",
                             work_mode);

                    /* 发送CPRI工作模式到FPGA */
                    fpga_send_cpri_mode(work_mode);
                }
                break;
            }

            default:
                LOG_DEBUG("Unhandled IE type: %u", ie_type);
                break;
        }

        offset += ie_len;
    }

    return SUCCESS;
}

/* 生成通道建立响应消息 */
int channel_setup_create_response(cpri_message_t *msg, uint32_t result)
{
    if (!msg) {
        return ERROR_INVALID_PARAM;
    }

    memset(msg, 0, sizeof(cpri_message_t));

    /* 填充消息头 */
    msg->header.msg_id = MSG_CHANNEL_SETUP_CFG_ACK;
    msg->header.paau_id = config_get_int("PAAU_ID", 0);
    msg->header.bbu_id = 0;
    msg->header.port_num = 0;
    msg->header.serial_num = 0;

    uint8_t *payload = NULL;
    uint32_t offset = 0;

    /* IE 21: 通道建立响应 (4字节) */
    uint8_t response[4];
    memcpy(response, &result, 4);
    add_ie(&payload, &offset, IE_TYPE_SETUP_RESPONSE, response, sizeof(response));

    msg->payload = payload;
    msg->payload_len = offset;

    LOG_INFO("Created channel setup response: result=%u", result);
    return SUCCESS;
}
