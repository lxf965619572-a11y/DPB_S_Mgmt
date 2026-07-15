#ifndef TRANSPARENT_MSG_H
#define TRANSPARENT_MSG_H

#include <stdint.h>
#include "common.h"
#include "cpri_protocol.h"

/* 消息ID定义 */
/* PAAU -> BBU 透传消息 (221-230) */
#define MSG_TRANSPARENT_PAAU_TO_BBU_BASE    221
#define MSG_TRANSPARENT_PAAU_TO_BBU_1       221
#define MSG_TRANSPARENT_PAAU_TO_BBU_2       222
#define MSG_TRANSPARENT_PAAU_TO_BBU_3       223
#define MSG_TRANSPARENT_PAAU_TO_BBU_4       224
#define MSG_TRANSPARENT_PAAU_TO_BBU_5       225
#define MSG_TRANSPARENT_PAAU_TO_BBU_6       226
#define MSG_TRANSPARENT_PAAU_TO_BBU_7       227
#define MSG_TRANSPARENT_PAAU_TO_BBU_8       228
#define MSG_TRANSPARENT_PAAU_TO_BBU_9       229
#define MSG_TRANSPARENT_PAAU_TO_BBU_10      230

/* BBU -> PAAU 透传消息 (231-240) */
#define MSG_TRANSPARENT_BBU_TO_PAAU_BASE    231
#define MSG_TRANSPARENT_BBU_TO_PAAU_1       231
#define MSG_TRANSPARENT_BBU_TO_PAAU_2       232
#define MSG_TRANSPARENT_BBU_TO_PAAU_3       233
#define MSG_TRANSPARENT_BBU_TO_PAAU_4       234
#define MSG_TRANSPARENT_BBU_TO_PAAU_5       235
#define MSG_TRANSPARENT_BBU_TO_PAAU_6       236
#define MSG_TRANSPARENT_BBU_TO_PAAU_7       237
#define MSG_TRANSPARENT_BBU_TO_PAAU_8       238
#define MSG_TRANSPARENT_BBU_TO_PAAU_9       239
#define MSG_TRANSPARENT_BBU_TO_PAAU_10      240

/* IE ID定义 */
#define IE_TRANSPARENT_TARGET               0x0641  /* 1601: 透传目标 */
#define IE_TRANSPARENT_CONTENT              0x0642  /* 1602: 透传内容 */

/* 透传目标标识 */
#define TRANSPARENT_TARGET_OMC              0  /* OMC/来自OMC的透传消息 */
#define TRANSPARENT_TARGET_BBU_DEBUG        1  /* BBU本地调试口 */

/* 透传内容最大长度 */
#define TRANSPARENT_CONTENT_MAX_LEN         1024

/**
 * 透传目标 IE (IE ID: 1601)
 */
typedef struct {
    uint8_t target_id;  /* 透传目标标识: 0=OMC, 1=BBU本地调试口 */
} __attribute__((packed)) ie_transparent_target_t;

/**
 * 透传内容 IE (IE ID: 1602)
 */
typedef struct {
    uint8_t content[TRANSPARENT_CONTENT_MAX_LEN];  /* 透传数据内容 */
} __attribute__((packed)) ie_transparent_content_t;

/**
 * 透传消息完整结构（用于解析）
 */
typedef struct {
    uint8_t target_id;                              /* 目标标识 */
    uint8_t content[TRANSPARENT_CONTENT_MAX_LEN];   /* 内容数据 */
    uint16_t content_len;                           /* 实际内容长度 */
} transparent_message_t;

/**
 * 透传消息解析结果（用于日志审计）
 */
typedef struct {
    uint16_t msg_id;            /* 消息ID */
    char timestamp[32];         /* 时间戳 */
    char source_ip[16];         /* 来源IP */
    int parse_status;           /* 解析状态码: 0=成功, <0=错误码 */
    transparent_message_t data; /* 解析后的数据 */
} transparent_parse_result_t;

/**
 * 初始化透传消息模块
 */
int transparent_msg_init(void);

/**
 * 清理透传消息模块
 */
void transparent_msg_cleanup(void);

/**
 * 解析透传消息
 * @param msg_id 消息ID
 * @param payload 消息载荷
 * @param payload_len 载荷长度
 * @param result 解析结果输出
 * @return 成功返回SUCCESS，失败返回错误码
 */
int transparent_msg_parse(uint16_t msg_id, const uint8_t *payload,
                         uint32_t payload_len, transparent_parse_result_t *result);

/**
 * 处理BBU到PAAU的透传消息
 * @param msg_id 消息ID (231-240)
 * @param trans_msg 透传消息数据
 * @param source_ip 来源IP
 * @return 成功返回SUCCESS，失败返回错误码
 */
int handle_transparent_bbu_to_paau(uint16_t msg_id,
                                   const transparent_message_t *trans_msg,
                                   const char *source_ip);

/**
 * 发送PAAU到BBU的透传消息
 * @param msg_id 消息ID (221-230)
 * @param target_id 目标标识
 * @param content 透传内容
 * @param content_len 内容长度
 * @return 成功返回SUCCESS，失败返回错误码
 */
int send_transparent_paau_to_bbu(uint16_t msg_id, uint8_t target_id,
                                 const uint8_t *content, uint16_t content_len);

/**
 * 检查消息ID是否为透传消息
 * @param msg_id 消息ID
 * @return true=是透传消息, false=不是
 */
bool is_transparent_message(uint16_t msg_id);

/**
 * 获取透传目标名称
 * @param target_id 目标标识
 * @return 目标名称字符串
 */
const char* get_transparent_target_name(uint8_t target_id);

/**
 * 处理透传消息的通用入口（从msg_handler调用）
 * @param msg CPRI消息
 * @param source_ip 来源IP
 * @return 成功返回SUCCESS，失败返回错误码
 */
int transparent_msg_handle(const cpri_message_t *msg, const char *source_ip);

#endif /* TRANSPARENT_MSG_H */
