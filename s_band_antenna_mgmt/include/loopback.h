#ifndef LOOPBACK_H
#define LOOPBACK_H

#include <stdint.h>
#include "common.h"
#include "cpri_protocol.h"

/* 消息ID定义 (已在cpri_protocol.h中定义) */
/* MSG_LOOPBACK_REQ = 81 */
/* MSG_LOOPBACK_REQ_RSP = 82 */

/* IE ID定义 */
#define IE_LOOPBACK_REQ             0x0579  /* 1401 */
#define IE_LOOPBACK_RSP             0x0583  /* 1411 */

/* 环回类型 */
#define LOOPBACK_TYPE_OTHER         0  /* 其他 */
#define LOOPBACK_TYPE_PHY_PORT      1  /* 物理端口环回使能 */
#define LOOPBACK_TYPE_BEAM          2  /* 波束环回使能 */
#define LOOPBACK_TYPE_DISABLE       3  /* 环回去使能 */

/* 环回结果 */
#define LOOPBACK_RESULT_SUCCESS     0  /* 成功 */
#define LOOPBACK_RESULT_NOT_SUPPORT 1  /* 失败-不支持 */

/* 错误码定义 */
#define LOOPBACK_ERR_FORMAT         -1001  /* 格式错误 */
#define LOOPBACK_ERR_PERMISSION     -1002  /* 权限不足 */
#define LOOPBACK_ERR_OUT_OF_RANGE   -1003  /* 字段超限 */
#define LOOPBACK_ERR_INVALID_TYPE   -1004  /* 无效类型 */
#define LOOPBACK_ERR_INVALID_PORT   -1005  /* 无效端口号 */
#define LOOPBACK_ERR_INVALID_BEAM   -1006  /* 无效波束号 */

/**
 * 环回请求 IE (IE ID: 1401)
 * 包含在消息 [81] 中
 */
typedef struct {
    uint8_t loopback_type;   /* 环回类型 */
    uint16_t test_period;    /* 检测周期定时器(ms): 0=单次, >0=持续时间 */
    uint8_t port_number;     /* 端口号 (0-7) */
    uint8_t beam_number;     /* 波束号 */
} __attribute__((packed)) ie_loopback_req_t;

/**
 * 环回响应 IE (IE ID: 1411)
 * 包含在消息 [82] 中
 */
typedef struct {
    uint8_t loopback_type;   /* 环回类型(回填) */
    uint8_t port_number;     /* 端口号(回填) */
    uint8_t beam_number;     /* 波束号(回填) */
    uint8_t result;          /* 返回结果: 0=成功, 1=失败 */
} __attribute__((packed)) ie_loopback_rsp_t;

/**
 * 环回消息解析结果（用于日志审计）
 */
typedef struct {
    uint16_t msg_id;            /* 消息ID */
    char timestamp[32];         /* 时间戳 */
    char source_ip[16];         /* 来源IP */
    int parse_status;           /* 解析状态码: 0=成功, <0=错误码 */
    ie_loopback_req_t request;  /* 解析后的请求内容 */
} loopback_parse_result_t;

/**
 * 初始化环回模块
 */
int loopback_init(void);

/**
 * 清理环回模块
 */
void loopback_cleanup(void);

/**
 * 解析环回请求消息
 * @param data 原始消息数据
 * @param len 数据长度
 * @param result 解析结果输出
 * @return 成功返回SUCCESS，失败返回错误码
 */
int loopback_parse_request(const uint8_t *data, size_t len, loopback_parse_result_t *result);

/**
 * 验证环回请求合法性
 * @param req 环回请求IE
 * @return 成功返回SUCCESS，失败返回错误码
 */
int loopback_validate_request(const ie_loopback_req_t *req);

/**
 * 处理环回请求
 * @param req_header 请求消息头
 * @param req_ie 环回请求IE
 * @param source_ip 来源IP地址
 * @return 成功返回SUCCESS，失败返回错误码
 */
int handle_loopback_request(const cpri_msg_header_t *req_header,
                            const ie_loopback_req_t *req_ie,
                            const char *source_ip);

/**
 * 发送环回响应
 * @param req_header 原始请求消息头
 * @param req_ie 原始请求IE
 * @param result 处理结果
 * @return 成功返回SUCCESS，失败返回错误码
 */
int send_loopback_response(const cpri_msg_header_t *req_header,
                           const ie_loopback_req_t *req_ie,
                           uint8_t result);

#endif /* LOOPBACK_H */
