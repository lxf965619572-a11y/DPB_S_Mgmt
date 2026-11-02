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

/* 软件与固件分开存放：
 *   软件: VERSION_BASE_DIR/<版本>                      （/opt/vendor/current 指向这里）
 *   固件: FIRMWARE_BASE_DIR/<v7a|v7b|subarray>/<版本>  （按 FPGA 种类分目录）
 * 原先固件也解压到 VERSION_BASE_DIR/<版本>，与软件、以及不同 FPGA 的固件共用一个
 * 命名空间，版本号相同就会互相覆盖（下载时会先 rm -rf 同名目录）。 */
#define FIRMWARE_BASE_DIR           "/opt/vendor/firmware"
#define FIRMWARE_STAGING_DIR        "/opt/vendor/firmware/.staging"   /* 解压后、判定 FPGA 种类前的暂存区 */
#define VERSION_DOWNLOAD_SW_DIR     VERSION_DOWNLOAD_DIR "/software"
#define VERSION_DOWNLOAD_FW_DIR     VERSION_DOWNLOAD_DIR "/firmware"
#define VERSION_LOG_FILE            "/var/log/vendor_version.log"

/* ==================== 软件升级试运行与自动回退 ====================
 * BBU 激活新软件版本时，先写试运行记录（新版本、上一个版本、启动次数），再切换
 * /opt/vendor/current。之后：
 *   - systemd 每次启动服务前执行 scripts/upgrade-guard.sh（ExecStartPre），
 *     启动次数 +1；超过 UPGRADE_TRIAL_MAX_STARTS 次仍未被确认，就把 current
 *     切回上一个版本，并写回退通知文件；
 *   - 新版本与 BBU 完成通道建立 → 确认健康，删除试运行记录；
 *   - 新版本连续运行 UPGRADE_TRIAL_STABLE_SEC 秒 → 启动次数清零（说明进程能
 *     稳定运行，此后的重启视为正常重启/断电，不计入失败），记录保留到 BBU 确认。
 * 文件格式均为 key=value 文本，C 与 shell 两侧都要能解析，改格式须同步改脚本。 */
#define VERSION_TRIAL_FILE          VERSION_ROLLBACK_DIR "/trial"
#define VERSION_AUTO_ROLLBACK_NOTICE VERSION_ROLLBACK_DIR "/auto_rollback.last"

/* 最大版本号长度 */
#define MAX_VERSION_LEN             40
#define MAX_FILENAME_LEN            16
#define MAX_FILEPATH_LEN            200
#define MAX_TIMESTAMP_LEN           20

/* 单个版本镜像的合理上限。北向报文的 file_len 是 uint32_t，超过这个量级的
 * 值不可能是真实镜像（且会影响落地文件大小的比对），在下发请求处直接拒绝。 */
#define MAX_IMAGE_BYTES             (256u * 1024u * 1024u)

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

/* 回滚到指定版本（人工回退：直接切换，不进入试运行） */
int version_rollback(const char *version, const char *reason);

/* 与 BBU 完成通道建立时调用：确认当前软件版本健康，结束试运行；
 * 若本次启动前发生过自动回退，通知已随告警上报，一并清掉通知文件。 */
void version_trial_on_channel_established(void);

/* 主循环每秒调用一次，传入进程已运行秒数：达到 UPGRADE_TRIAL_STABLE_SEC
 * 时把试运行的启动计数清零。 */
void version_trial_on_uptime(uint32_t uptime_sec);

/* 版本管理器销毁 */
void version_manager_destroy(void);

#endif /* VERSION_MANAGER_H */
