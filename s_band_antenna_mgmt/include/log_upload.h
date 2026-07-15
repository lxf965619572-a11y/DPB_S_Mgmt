#ifndef LOG_UPLOAD_H
#define LOG_UPLOAD_H

#include "common.h"
#include "cpri_protocol.h"

/* IE类型定义 */
#define IE_LOG_UPLOAD_REQ           0x04B1  /* 1201: 日志上传请求 */
#define IE_LOG_UPLOAD_ACK           0x04B5  /* 1205: 日志上传应答 */
#define IE_LOG_UPLOAD_RESULT_IND    0x04BB  /* 1211: 日志上传结果指示 */

/* 日志上传结果定义 */
#define LOG_UPLOAD_SUCCESS          0       /* 上传成功 */
#define LOG_UPLOAD_FILE_NOT_EXIST   1       /* 日志文件不存在 */
#define LOG_UPLOAD_FAILED           2       /* 日志上传失败 */

/* 日志上传应答结果定义 */
#define LOG_UPLOAD_ACK_ACCEPT       0       /* 接受请求 */
#define LOG_UPLOAD_ACK_REJECT       1       /* 拒绝请求 */

/* IE 1201: 日志上传请求 */
typedef struct __attribute__((packed)) {
    cpri_ie_header_t header;
    char store_path[200];       /* FTP存储路径 */
} ie_log_upload_req_t;

/* IE 1205: 日志上传应答 */
typedef struct __attribute__((packed)) {
    cpri_ie_header_t header;
    uint8_t result;             /* 应答结果 */
} ie_log_upload_ack_t;

/* IE 1211: 日志上传结果指示 */
typedef struct __attribute__((packed)) {
    cpri_ie_header_t header;
    uint8_t result;             /* 上传结果 */
    char store_path[200];       /* 存储路径 */
    char file_name[16];         /* 文件名 */
} ie_log_upload_result_ind_t;

/* 日志上传模块初始化 */
int log_upload_init(void);

/* 处理日志上传请求 (MsgID: 131) */
int log_upload_handle_request(const cpri_message_t *msg);

/* 日志上传模块销毁 */
void log_upload_destroy(void);

#endif /* LOG_UPLOAD_H */
