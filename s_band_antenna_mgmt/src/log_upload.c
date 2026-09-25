#include "log_upload.h"
#include "logger.h"
#include "tcp_client.h"
#include "config.h"
#include "shell_util.h"
#include <sys/stat.h>
#include <pthread.h>

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;

/* 日志上传任务结构 */
typedef struct {
    char store_path[200];
    char log_file_path[256];
    char ftp_server[64];
    uint16_t ftp_port;
    cpri_msg_header_t req_header;  /* 保存请求消息头用于响应 */
} log_upload_task_t;

static pthread_t g_upload_thread;
static log_upload_task_t g_upload_task;
static bool g_upload_in_progress = false;
static pthread_mutex_t g_upload_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 发送日志上传应答 (MsgID: 132) */
static int send_log_upload_ack(const cpri_msg_header_t *req_header, uint8_t result)
{
    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    /* 设置消息头 */
    response.header.msg_id = MSG_LOG_UPLOAD_REQ_ACK;
    response.header.paau_id = req_header->paau_id;
    response.header.bbu_id = req_header->bbu_id;
    response.header.port_num = req_header->port_num;
    response.header.serial_num = req_header->serial_num;

    /* 构造IE 1205 */
    ie_log_upload_ack_t ie_ack;
    ie_ack.header.ie_type = (IE_LOG_UPLOAD_ACK);
    ie_ack.header.ie_length = (sizeof(ie_log_upload_ack_t));
    ie_ack.result = result;

    /* 设置payload - 使用栈缓冲区 */
    uint8_t payload_buffer[256];
    response.payload_len = sizeof(ie_log_upload_ack_t);
    response.payload = payload_buffer;
    memcpy(response.payload, &ie_ack, sizeof(ie_log_upload_ack_t));

    /* 编码并发送 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent log upload ack (serial=%u, result=%u)",
                 response.header.serial_num, result);
    }

    /* 无需cpri_free_message，使用的是栈缓冲区 */
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

/* 发送日志上传结果指示 (MsgID: 133) */
static int send_log_upload_result(const cpri_msg_header_t *req_header,
                                   uint8_t result,
                                   const char *store_path,
                                   const char *file_name)
{
    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    /* 设置消息头 */
    response.header.msg_id = MSG_LOG_UPLOAD_RESULT_IND;
    response.header.paau_id = req_header->paau_id;
    response.header.bbu_id = req_header->bbu_id;
    response.header.port_num = req_header->port_num;
    response.header.serial_num = req_header->serial_num;

    /* 构造IE 1211 */
    ie_log_upload_result_ind_t ie_result;
    memset(&ie_result, 0, sizeof(ie_result));
    ie_result.header.ie_type = (IE_LOG_UPLOAD_RESULT_IND);
    ie_result.header.ie_length = (sizeof(ie_log_upload_result_ind_t));
    ie_result.result = result;
    strncpy(ie_result.store_path, store_path, sizeof(ie_result.store_path) - 1);
    strncpy(ie_result.file_name, file_name, sizeof(ie_result.file_name) - 1);

    /* 设置payload - 使用栈缓冲区 */
    uint8_t payload_buffer[512];
    response.payload_len = sizeof(ie_log_upload_result_ind_t);
    response.payload = payload_buffer;
    memcpy(response.payload, &ie_result, sizeof(ie_log_upload_result_ind_t));

    /* 编码并发送 */
    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent log upload result (serial=%u, result=%u, file=%s)",
                 response.header.serial_num, result, file_name);
    }

    /* 无需cpri_free_message，使用的是栈缓冲区 */
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

/* 使用curl执行匿名FTP上传 */
static int ftp_upload_file_with_curl(const char *local_file,
                                      const char *ftp_server,
                                      uint16_t ftp_port,
                                      const char *remote_path,
                                      const char *file_name)
{
    char full_url[512];

    /* 构造FTP URL: ftp://server:port/remote_path/file_name */
    if (remote_path && remote_path[0] != '\0') {
        /* 确保路径格式正确 */
        if (remote_path[0] == '/') {
            snprintf(full_url, sizeof(full_url), "ftp://%s:%u%s/%s",
                    ftp_server, ftp_port, remote_path, file_name);
        } else {
            snprintf(full_url, sizeof(full_url), "ftp://%s:%u/%s/%s",
                    ftp_server, ftp_port, remote_path, file_name);
        }
    } else {
        snprintf(full_url, sizeof(full_url), "ftp://%s:%u/%s",
                ftp_server, ftp_port, file_name);
    }

    /* argv 数组传参，不经过 shell：
     * full_url 含北向可控的 store_path（IE 1201），走 shell 会被 $() 与反引号注入。
     * 使用匿名登录，支持断点续传和超时控制。 */
    char *curl_argv[] = { (char *)"curl", (char *)"-S", (char *)"--ftp-create-dirs",
                          (char *)"-C", (char *)"-", (char *)"-T", (char *)local_file,
                          (char *)"--user", (char *)"anonymous:",
                          (char *)"--connect-timeout", (char *)"30",
                          (char *)"--max-time", (char *)"7200",
                          (char *)"--speed-limit", (char *)"512",
                          (char *)"--speed-time", (char *)"120",
                          (char *)"--retry", (char *)"3",
                          (char *)"--retry-delay", (char *)"5",
                          full_url, NULL };

    LOG_INFO("Executing FTP upload: %s -> %s", local_file, full_url);

    /* 读取curl输出（shell_run 返回 waitpid 原始状态，下方 WEXITSTATUS 仍适用） */
    char output[4096] = {0};
    int status = shell_run(curl_argv, output, sizeof(output));
    if (status < 0) {
        LOG_ERROR("Failed to execute curl command");
        return ERROR_GENERAL;
    }

    if (strlen(output) > 0) {
        LOG_WARN("curl output: %s", output);
    }

    /* 检查curl返回状态 */
    if (status != 0) {
        int exit_code = WEXITSTATUS(status);
        const char *error_msg = "Unknown error";

        switch (exit_code) {
            case 6:  error_msg = "Couldn't resolve host"; break;
            case 7:  error_msg = "Failed to connect to host"; break;
            case 9:  error_msg = "FTP access denied"; break;
            case 18: error_msg = "Partial file transfer"; break;
            case 28: error_msg = "Operation timeout"; break;
            case 56: error_msg = "Network receive error"; break;
            case 67: error_msg = "Login denied"; break;
            default: error_msg = "Transfer failed"; break;
        }

        LOG_ERROR("curl command failed with exit code %d: %s", exit_code, error_msg);
        return ERROR_GENERAL;
    }

    LOG_INFO("FTP upload completed successfully");
    return SUCCESS;
}

/* 日志上传线程函数 */
static void* log_upload_thread_func(void *arg)
{
    log_upload_task_t *task = (log_upload_task_t *)arg;
    uint8_t result = LOG_UPLOAD_SUCCESS;
    char file_name[16] = {0};
    char temp_file[512] = {0};
    bool temp_file_created = false;

    LOG_INFO("Log upload thread started");

    /* 检查日志文件是否存在 */
    struct stat st;
    if (stat(task->log_file_path, &st) != 0) {
        LOG_ERROR("Log file does not exist: %s", task->log_file_path);
        result = LOG_UPLOAD_FILE_NOT_EXIST;
        goto send_result;
    }

    /* 记录文件大小 */
    LOG_INFO("Log file size: %.2f MB (%ld bytes)",
             st.st_size / (1024.0 * 1024.0), st.st_size);

    /* 根据文件大小给出警告 */
    if (st.st_size > 100 * 1024 * 1024) {  /* 100MB */
        LOG_WARN("Large log file detected (%.2f MB), upload may take several minutes",
                 st.st_size / (1024.0 * 1024.0));
    }

    /* 生成临时文件路径 */
    snprintf(temp_file, sizeof(temp_file), "%s.upload", task->log_file_path);

    /* 复制日志文件到临时文件（避免上传过程中文件被修改） */
    LOG_INFO("Creating snapshot of log file: %s", temp_file);
    /* argv 数组传参，不经过 shell（路径来自配置，非北向可控，仍统一走无 shell 路径） */
    char *cp_argv[] = { (char *)"cp", task->log_file_path, temp_file, NULL };
    char cp_out[256] = {0};
    int cp_ret = shell_run(cp_argv, cp_out, sizeof(cp_out));
    if (cp_ret != 0) {
        LOG_ERROR("Failed to create log file snapshot (exit code: %d)", cp_ret);
        result = LOG_UPLOAD_FAILED;
        goto send_result;
    }

    temp_file_created = true;
    LOG_INFO("Log file snapshot created successfully");

    /* 生成文件名: PAAU_YYYYMMDD_HHMMSS.log */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    // snprintf(file_name, sizeof(file_name), "PAAU%04d%02d%02d%02d%02d%02d.log",
    //         tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
    //         tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    snprintf(file_name, sizeof(file_name), "PAAU%02d%02d.log",
            tm_info->tm_mon + 1, tm_info->tm_mday);

    LOG_INFO("Uploading log file snapshot: %s -> %s", temp_file, file_name);

    /* 执行FTP上传（使用临时文件） */
    int ret = ftp_upload_file_with_curl(temp_file,
                                        task->ftp_server,
                                        task->ftp_port,
                                        task->store_path,
                                        file_name);
    if (ret != SUCCESS) {
        LOG_ERROR("FTP upload failed");
        result = LOG_UPLOAD_FAILED;
    } else {
        LOG_INFO("Log file uploaded successfully: %s", file_name);
    }

send_result:
    /* 清理临时文件 */
    if (temp_file_created) {
        LOG_INFO("Cleaning up temporary file: %s", temp_file);
        if (unlink(temp_file) != 0) {
            LOG_WARN("Failed to delete temporary file: %s", temp_file);
        } else {
            LOG_INFO("Temporary file deleted successfully");
        }
    }

    /* 发送上传结果指示 */
    send_log_upload_result(&task->req_header, result, task->store_path, file_name);

    /* 清除上传中标志 */
    pthread_mutex_lock(&g_upload_mutex);
    g_upload_in_progress = false;
    pthread_mutex_unlock(&g_upload_mutex);

    LOG_INFO("Log upload thread finished");
    return NULL;
}

int log_upload_init(void)
{
    g_upload_in_progress = false;
    LOG_INFO("Log upload module initialized");
    return SUCCESS;
}

int log_upload_handle_request(const cpri_message_t *msg)
{
    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid log upload request message");
        return ERROR_INVALID_PARAM;
    }

    /* 检查是否已有上传任务在进行 */
    pthread_mutex_lock(&g_upload_mutex);
    if (g_upload_in_progress) {
        pthread_mutex_unlock(&g_upload_mutex);
        LOG_WARN("Log upload already in progress, rejecting request");
        send_log_upload_ack(&msg->header, LOG_UPLOAD_ACK_REJECT);
        return ERROR_GENERAL;
    }
    g_upload_in_progress = true;
    pthread_mutex_unlock(&g_upload_mutex);

    /* 解析IE 1201。先校验长度：store_path 是 200 字节裸字段，
     * payload 短于结构体时读到的是内存池里上一条报文的残留字节。 */
    if (msg->payload_len < sizeof(ie_log_upload_req_t)) {
        LOG_ERROR("Log upload request too short: %u < %zu",
                  msg->payload_len, sizeof(ie_log_upload_req_t));
        pthread_mutex_lock(&g_upload_mutex);
        g_upload_in_progress = false;
        pthread_mutex_unlock(&g_upload_mutex);
        send_log_upload_ack(&msg->header, LOG_UPLOAD_ACK_REJECT);
        return ERROR_INVALID_PARAM;
    }

    ie_log_upload_req_t *ie_req = (ie_log_upload_req_t *)msg->payload;

    /* store_path 由北向报文提供并直接进 FTP URL；先校验再使用/打印：
     * 拒绝含 ".." 的写法以阻断路径穿越，同时避免对未校验数据做 %s 打印。 */
    if (!shell_safe_relpath(ie_req->store_path)) {
        LOG_ERROR("Rejected log upload: unsafe store_path");
        pthread_mutex_lock(&g_upload_mutex);
        g_upload_in_progress = false;
        pthread_mutex_unlock(&g_upload_mutex);
        send_log_upload_ack(&msg->header, LOG_UPLOAD_ACK_REJECT);
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("Received log upload request: store_path=%s", ie_req->store_path);

    /* 准备上传任务 */
    memset(&g_upload_task, 0, sizeof(g_upload_task));
    strncpy(g_upload_task.store_path, ie_req->store_path, sizeof(g_upload_task.store_path) - 1);

    /* 获取日志文件路径 */
    const char *log_file = config_get_string("LOG_FILE", "./logs/antenna_mgmt.log");
    strncpy(g_upload_task.log_file_path, log_file, sizeof(g_upload_task.log_file_path) - 1);

    /* 保存请求消息头 */
    memcpy(&g_upload_task.req_header, &msg->header, sizeof(cpri_msg_header_t));

    /* 设置FTP服务器信息（从配置文件读取，默认使用BBU的IP） */
    const char *ftp_server = config_get_string("FTP_SERVER", g_config.bbu_ip);
    strncpy(g_upload_task.ftp_server, ftp_server, sizeof(g_upload_task.ftp_server) - 1);
    g_upload_task.ftp_port = config_get_int("FTP_PORT", 21);

    /* 发送应答 - 接受请求 */
    int ret = send_log_upload_ack(&msg->header, LOG_UPLOAD_ACK_ACCEPT);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to send log upload ack");
        pthread_mutex_lock(&g_upload_mutex);
        g_upload_in_progress = false;
        pthread_mutex_unlock(&g_upload_mutex);
        return ret;
    }

    /* 创建上传线程 */
    ret = pthread_create(&g_upload_thread, NULL, log_upload_thread_func, &g_upload_task);
    if (ret != 0) {
        LOG_ERROR("Failed to create log upload thread: %d", ret);
        pthread_mutex_lock(&g_upload_mutex);
        g_upload_in_progress = false;
        pthread_mutex_unlock(&g_upload_mutex);

        /* 发送失败结果 */
        send_log_upload_result(&msg->header, LOG_UPLOAD_FAILED,
                              g_upload_task.store_path, "");
        return ERROR_GENERAL;
    }

    /* 分离线程，让其自动清理 */
    pthread_detach(g_upload_thread);

    LOG_INFO("Log upload task started");
    return SUCCESS;
}

void log_upload_destroy(void)
{
    /* 等待上传任务完成 */
    pthread_mutex_lock(&g_upload_mutex);
    if (g_upload_in_progress) {
        LOG_WARN("Log upload in progress during destroy");
    }
    pthread_mutex_unlock(&g_upload_mutex);

    LOG_INFO("Log upload module destroyed");
}
