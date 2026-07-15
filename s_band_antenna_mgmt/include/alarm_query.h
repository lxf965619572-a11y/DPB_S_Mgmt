#ifndef ALARM_QUERY_H
#define ALARM_QUERY_H

#include <stdint.h>
#include "common.h"
#include "alarm_manager.h"
#include "cpri_protocol.h"

/* 消息ID定义 (已在cpri_protocol.h中定义) */
/* MSG_ALARM_QUERY_REQ = 121 */
/* MSG_ALARM_REPORT_REQ = 111 */

/* IE ID定义 */
#define IE_ALARM_QUERY_REQ          0x044D  /* 1101 */
#define IE_ALARM_REPORT             0x03E9  /* 1001 */

/* 特殊查询码 */
#define ALARM_QUERY_ALL             0xFFFFFFFF  /* 查询所有告警 */

/* 告警有效性 */
#define ALARM_VALIDITY_VALID        0  /* 有效 */
#define ALARM_VALIDITY_INVALID      1  /* 无效/不存在 */

/* 告警清除标志 */
#define ALARM_CLEAR_FLAG_ACTIVE     0  /* 告警产生 */
#define ALARM_CLEAR_FLAG_CLEARED    1  /* 告警清除 */

/**
 * 告警查询请求 IE (IE ID: 1101)
 * 包含在消息 [121] 中
 */
typedef struct {
    uint32_t alarm_code;    /* 告警码，0xFFFFFFFF表示查询所有 */
    uint32_t sub_code;      /* 告警子码，0xFFFFFFFF表示不限子码 */
} __attribute__((packed)) ie_alarm_query_req_t;

/**
 * 告警上报 IE (IE ID: 1001)
 * 包含在消息 [111] 中
 */
typedef struct {
    uint16_t validity;          /* 告警有效性: 0=有效, 1=无效 */
    uint32_t alarm_code;        /* 告警码 */
    uint32_t sub_code;          /* 告警子码 */
    uint32_t clear_flag;        /* 清除标志: 0=产生, 1=清除 */
    char timestamp[20];         /* 时间戳: yyyy-mm-dd hh:mm:ss */
    char additional_info[100];  /* 附加信息 */
} __attribute__((packed)) ie_alarm_report_t;

/**
 * 初始化告警查询模块
 */
int alarm_query_init(void);

/**
 * 清理告警查询模块
 */
void alarm_query_cleanup(void);

/**
 * 处理告警查询请求
 * @param req_header 请求消息头
 * @param req_ie 查询请求IE
 * @return 成功返回SUCCESS，失败返回错误码
 */
int handle_alarm_query_request(const cpri_msg_header_t *req_header,
                                const ie_alarm_query_req_t *req_ie);

/**
 * 发送告警上报消息
 * @param req_header 请求消息头
 * @param record 告警记录
 * @return 成功返回SUCCESS，失败返回错误码
 */
int send_alarm_report_from_record(const cpri_msg_header_t *req_header,
                                   const alarm_record_t *record);

#endif /* ALARM_QUERY_H */
