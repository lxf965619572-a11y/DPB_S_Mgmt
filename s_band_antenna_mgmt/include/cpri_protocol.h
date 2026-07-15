#ifndef CPRI_PROTOCOL_H
#define CPRI_PROTOCOL_H

#include "common.h"

/* CPRI消息头长度 */
#define CPRI_HEADER_LEN     15

/* CPRI消息类型定义 */
typedef enum {
    MSG_CHANNEL_SETUP_REQ = 1,
    MSG_CHANNEL_SETUP_CFG = 2,
    MSG_CHANNEL_SETUP_CFG_ACK = 3,
    MSG_VERSION_UPDATE_RESULT_IND = 4,
    MSG_VERSION_UPDATE_RESULT_IND_ACK = 5,
    MSG_PAAU_VERSION_QUERY = 11,
    MSG_PAAU_VERSION_QUERY_ACK = 12,
    MSG_VERSION_DOWNLOAD_REQ = 21,
    MSG_VERSION_DOWNLOAD_ACK = 22,
    MSG_VERSION_DOWNLOAD_RESULT_IND = 23,
    MSG_PAAU_VERSION_ACTIVATE_IND = 31,
    MSG_PAAU_VERSION_ACTIVATE_ACK = 32,
    MSG_PAAU_STATUS_QUERY = 41,
    MSG_PAAU_STATUS_QUERY_RSP = 42,
    MSG_PAAU_PARAM_QUERY = 51,
    MSG_PAAU_PARAM_QUERY_RSP = 52,
    MSG_PAAU_PARAM_CONFIG = 61,
    MSG_PAAU_PARAM_CONFIG_RSP = 62,
    MSG_PHASED_ARRAY_CALIB_IND = 71,
    MSG_PHASED_ARRAY_CALIB_IND_ACK = 72,
    MSG_LOOPBACK_REQ = 81,
    MSG_LOOPBACK_REQ_RSP = 82,
    MSG_ALARM_REPORT_REQ = 111,
    MSG_ALARM_QUERY_REQ = 121,
    MSG_ALARM_REPORT_REQ_ACK = 122,
    MSG_LOG_UPLOAD_REQ = 131,
    MSG_LOG_UPLOAD_REQ_ACK = 132,
    MSG_LOG_UPLOAD_RESULT_IND = 133,
    MSG_RESET_IND = 141,
    MSG_REMOTE_PAAU_RESET_IND = 151,
    MSG_PAAU_HEARTBEAT = 171,
    MSG_BBU_HEARTBEAT = 181,
    MSG_NR_CELL_CONFIG = 195,
    MSG_NR_CELL_CONFIG_RSP = 196,
} cpri_msg_type_t;

/* CPRI消息头结构 */
typedef struct __attribute__((packed)) {
    uint32_t msg_id;        /* 消息编号 */
    uint32_t msg_length;    /* 消息长度（包含头部） */
    uint8_t  paau_id;       /* PAAU ID */
    uint8_t  bbu_id;        /* BBU ID */
    uint8_t  port_num;      /* 光纤端口号 */
    uint32_t serial_num;    /* 流水号 */
} cpri_msg_header_t;

/* IE头结构 */
typedef struct __attribute__((packed)) {
    uint16_t ie_type;       /* IE类型 */
    uint16_t ie_length;     /* IE长度（包含IE头） */
} cpri_ie_header_t;

/* 完整CPRI消息结构 */
typedef struct {
    cpri_msg_header_t header;
    uint8_t *payload;
    uint32_t payload_len;
} cpri_message_t;

/* 函数声明 */
int cpri_encode_message(const cpri_message_t *msg, uint8_t *buffer, uint32_t buffer_size);
int cpri_decode_message(const uint8_t *buffer, uint32_t buffer_len, cpri_message_t *msg);
void cpri_free_message(cpri_message_t *msg);
const char* cpri_get_msg_type_name(uint32_t msg_id);

#endif /* CPRI_PROTOCOL_H */
