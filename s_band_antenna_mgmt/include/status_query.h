#ifndef STATUS_QUERY_H
#define STATUS_QUERY_H

#include "common.h"
#include "cpri_protocol.h"
#include "fpga_protocol.h"

/* IE类型定义 - 查询请求 */
#define IE_TYPE_BEAM_STATUS_QUERY       303
#define IE_TYPE_LO_STATUS_QUERY         304
#define IE_TYPE_CLOCK_STATUS_QUERY      305
#define IE_TYPE_RUN_STATUS_QUERY        306
#define IE_TYPE_CPRI_MODE_QUERY         307
#define IE_TYPE_CALIB_RESULT_QUERY      308

/* IE类型定义 - 查询响应 */
#define IE_TYPE_BEAM_STATUS_RESP        353
#define IE_TYPE_LO_STATUS_RESP          354
#define IE_TYPE_CLOCK_STATUS_RESP       355
#define IE_TYPE_RUN_STATUS_RESP         356
#define IE_TYPE_CPRI_MODE_RESP          357
#define IE_TYPE_CALIB_RESULT_RESP       358

/* 处理状态查询请求 */
int status_query_handle_request(const cpri_message_t *msg);

/* 生成状态查询响应 */
int status_query_create_response(cpri_message_t *response, const cpri_message_t *request);

#endif /* STATUS_QUERY_H */
