#ifndef PHASED_ARRAY_CALIB_H
#define PHASED_ARRAY_CALIB_H

#include <stdint.h>
#include "common.h"
#include "cpri_protocol.h"

/* IE 类型定义 */
#define IE_PHASED_ARRAY_CALIB_IND       0x02EF  /* 751: 相控阵校准指示 */
#define IE_PHASED_ARRAY_CALIB_ACK       0x02F9  /* 761: 相控阵校准指示应答 */

/* 校准结果定义 */
#define CALIB_RESULT_SUCCESS            0       /* 成功 */
#define CALIB_RESULT_FAILURE            1       /* 失败 */

/**
 * 相控阵校准指示 IE (IE ID: 751)
 * 包含在消息 [71] 中
 */
typedef struct __attribute__((packed)) {
    /* IE 头部已经在 payload 中，这里只定义数据部分 */
    /* 根据协议文档，IE 长度为 2BYTE，但没有具体数据字段说明 */
    /* 暂时定义为空结构，如果有数据字段再补充 */
} ie_phased_array_calib_ind_t;

/**
 * 相控阵校准指示应答 IE (IE ID: 761)
 * 包含在消息 [72] 中
 */
typedef struct __attribute__((packed)) {
    uint8_t result;     /* 返回结果: 0=成功, 非0=失败 */
} ie_phased_array_calib_ack_t;

/**
 * 初始化相控阵校准模块
 */
int phased_array_calib_init(void);

/**
 * 清理相控阵校准模块
 */
void phased_array_calib_cleanup(void);

/**
 * 处理相控阵校准指示请求
 * @param req_header 请求消息头
 * @param req_ie 校准指示IE（可能为NULL，如果没有数据部分）
 * @return 成功返回SUCCESS，失败返回错误码
 */
int handle_phased_array_calib_request(const cpri_msg_header_t *req_header,
                                      const ie_phased_array_calib_ind_t *req_ie);

#endif /* PHASED_ARRAY_CALIB_H */
