#ifndef VERSION_MANAGER_H
#define VERSION_MANAGER_H

#include "common.h"
#include "cpri_protocol.h"

/* IE类型定义 */
#define IE_VERSION_CHECK_RESULT     0x000E  /* 14: 软件版本核对结果（用于下载请求） */
#define IE_VERSION_DOWNLOAD_ACK     0x0065  /* 101: 版本下载响应 */
#define IE_VERSION_DOWNLOAD_RESULT  0x006F  /* 111: 版本下载传输完成指示 */
#define IE_VERSION_ACTIVATE_IND     0x0137  /* 311: 版本激活指示 */
#define IE_VERSION_ACTIVATE_ACK     0x0141  /* 321: 版本激活应答 */

/* 版本类型定义 */
#define VERSION_TYPE_SOFTWARE       0       /* 软件版本 */
#define VERSION_TYPE_FIRMWARE       1       /* 固件版本 */

/* 下载应答结果 */
#define DOWNLOAD_ACK_ACCEPT         0       /* 接受下载 */
#define DOWNLOAD_ACK_REJECT_BUSY    1       /* 拒绝：正在下载 */
#define DOWNLOAD_ACK_REJECT_SPACE   2       /* 拒绝：空间不足 */
#define DOWNLOAD_ACK_REJECT_OTHER   3       /* 拒绝：其他原因 */

/* 下载结果定义 */
#define DOWNLOAD_RESULT_SUCCESS     0       /* 下载成功 */
#define DOWNLOAD_RESULT_NOT_EXIST   1       /* 文件不存在 */
#define DOWNLOAD_RESULT_TIMEOUT     2       /* 超时 */
#define DOWNLOAD_RESULT_TOO_LARGE   3       /* 文件过大 */
#define DOWNLOAD_RESULT_CHECKSUM    4       /* 校验失败 */
#define DOWNLOAD_RESULT_EXTRACT     5       /* 解压失败 */
#define DOWNLOAD_RESULT_OTHER       6       /* 其他失败 */

/* 激活结果定义 */
#define ACTIVATE_RESULT_SUCCESS     0       /* 可以激活 */
#define ACTIVATE_RESULT_NOT_EXIST   1       /* 版本不存在 */
#define ACTIVATE_RESULT_CORRUPTED   2       /* 文件损坏 */
#define ACTIVATE_RESULT_MISMATCH    3       /* 版本不匹配 */
#define ACTIVATE_RESULT_OTHER       4       /* 其他原因 */

/* 版本管理路径定义 */
#define VERSION_BASE_DIR            "/opt/vendor/versions"
#define VERSION_CURRENT_LINK        "/opt/vendor/current"
#define VERSION_ROLLBACK_DIR        "/opt/vendor/versions/.rollback"
#define VERSION_DOWNLOAD_DIR        "/opt/vendor/downloads"
#define VERSION_LOG_FILE            "/var/log/vendor_version.log"

/* 最大版本号长度 */
#define MAX_VERSION_LEN             40
#define MAX_FILENAME_LEN            16
#define MAX_FILEPATH_LEN            200
#define MAX_TIMESTAMP_LEN           20

/* IE 14: 软件版本核对结果（用于下载请求） */
typedef struct __attribute__((packed)) {
    cpri_ie_header_t header;
    uint8_t ver_type;           /* 版本类型 */
    uint32_t result;            /* 返回结果 */
    char file_path[200];        /* FTP文件路径 */
    char file_name[16];         /* 文件名 */
    uint32_t file_len;          /* 文件大小 */
    char file_time[20];         /* 文件时间戳 */
    char file_ver[40];          /* 目标版本号 */
} ie_version_download_req_t;

/* IE 101: 版本下载响应 */
typedef struct __attribute__((packed)) {
    cpri_ie_header_t header;
    uint8_t ver_type;           /* 版本类型 */
    uint32_t result;            /* 应答结果 */
} ie_version_download_ack_t;

/* IE 111: 版本下载传输完成指示 */
typedef struct __attribute__((packed)) {
    cpri_ie_header_t header;
    uint8_t ver_type;           /* 版本类型 */
    uint32_t result;            /* 最终结果 */
} ie_version_download_result_t;

/* IE 311: 版本激活指示 */
typedef struct __attribute__((packed)) {
    cpri_ie_header_t header;
    uint8_t ver_type;           /* 版本类型 */
    char version_num[40];       /* 目标版本号 */
} ie_version_activate_ind_t;

/* IE 321: 版本激活应答 */
typedef struct __attribute__((packed)) {
    cpri_ie_header_t header;
    uint8_t ver_type;           /* 版本类型 */
    uint8_t result;             /* 激活结果 */
} ie_version_activate_ack_t;

/* 版本元数据结构 */
typedef struct {
    char version[MAX_VERSION_LEN];
    char build_time[MAX_TIMESTAMP_LEN];
    char checksum[65];          /* SHA-256 hex string */
    uint32_t file_size;
    uint8_t ver_type;
    char install_path[256];
} version_metadata_t;

/* 下载任务结构 */
typedef struct {
    char version[MAX_VERSION_LEN];
    char file_path[MAX_FILEPATH_LEN];
    char file_name[MAX_FILENAME_LEN];
    char file_time[MAX_TIMESTAMP_LEN];
    uint32_t file_len;
    uint8_t ver_type;
    char ftp_server[64];
    uint16_t ftp_port;
    cpri_msg_header_t req_header;
    bool in_progress;
} version_download_task_t;

/* 版本管理器初始化 */
int version_manager_init(void);

/* 处理版本下载请求 (MsgID: 21) */
int version_handle_download_request(const cpri_message_t *msg);

/* 处理版本激活指示 (MsgID: 31) */
int version_handle_activate_request(const cpri_message_t *msg);

/* 获取当前运行版本 */
int version_get_current(char *version, size_t len);

/* 列出所有已安装版本 */
int version_list_installed(char versions[][MAX_VERSION_LEN], int max_count);

/* 回滚到指定版本 */
int version_rollback(const char *version, const char *reason);

/* 版本管理器销毁 */
void version_manager_destroy(void);

#endif /* VERSION_MANAGER_H */
