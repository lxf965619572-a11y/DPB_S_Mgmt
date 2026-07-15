#ifndef RESET_H
#define RESET_H

#include <stdint.h>
#include "common.h"

/* 消息ID定义 (已在cpri_protocol.h中定义) */
/* MSG_RESET_IND = 141 */
/* MSG_REMOTE_PAAU_RESET_IND = 151 */
#define MSG_REMOTE_RESET_IND MSG_REMOTE_PAAU_RESET_IND

/* IE ID定义 */
#define IE_RESET_IND                0x0515  /* 1301 */

/* 复位类型 */
#define RESET_TYPE_SOFT             0  /* 热复位(软件重启) */
#define RESET_TYPE_HARD             1  /* 冷复位(硬件掉电重启) */

/* 错误码定义 */
#define RESET_ERR_FORMAT            -2001  /* 格式错误 */
#define RESET_ERR_PERMISSION        -2002  /* 权限不足 */
#define RESET_ERR_OUT_OF_RANGE      -2003  /* 字段超限 */
#define RESET_ERR_INVALID_TYPE      -2004  /* 无效复位类型 */

/**
 * 复位指示 IE (IE ID: 1301)
 * 包含在消息 [141] 和 [151] 中
 */
typedef struct {
    uint32_t reset_type;  /* 复位类型: 0=热复位, 1=冷复位 */
} __attribute__((packed)) ie_reset_ind_t;

/**
 * 复位消息解析结果（用于日志审计）
 */
typedef struct {
    uint16_t msg_id;         /* 消息ID */
    char timestamp[32];      /* 时间戳 */
    char source_ip[16];      /* 来源IP */
    int parse_status;        /* 解析状态码: 0=成功, <0=错误码 */
    ie_reset_ind_t request;  /* 解析后的请求内容 */
} reset_parse_result_t;

/**
 * 初始化复位模块
 */
int reset_init(void);

/**
 * 清理复位模块
 */
void reset_cleanup(void);

/**
 * 解析复位指示消息
 * @param data 原始消息数据
 * @param len 数据长度
 * @param result 解析结果输出
 * @return 成功返回SUCCESS，失败返回错误码
 */
int reset_parse_indication(const uint8_t *data, size_t len, reset_parse_result_t *result);

/**
 * 验证复位指示合法性
 * @param ind 复位指示IE
 * @return 成功返回SUCCESS，失败返回错误码
 */
int reset_validate_indication(const ie_reset_ind_t *ind);

/**
 * 处理复位指示
 * @param ind_ie 复位指示IE
 * @param source_ip 来源IP地址
 * @param msg_id 消息ID (141或151)
 * @return 成功返回SUCCESS，失败返回错误码
 */
int handle_reset_indication(const ie_reset_ind_t *ind_ie, const char *source_ip, uint16_t msg_id);

#endif /* RESET_H */
