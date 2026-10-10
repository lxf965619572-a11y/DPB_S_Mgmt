#include "log_upload.h"
#include "logger.h"
#include "tcp_client.h"
#include "config.h"
#include "shell_util.h"
#include "telemetry_log.h"
#include <sys/stat.h>
#include <libgen.h>
#include <pthread.h>

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;

/* 日志上传任务结构 */
typedef struct {
    char store_path[200];
    char log_file_path[256];    /* 文本日志（主件：不存在即判失败） */
    char tel_log_path[256];     /* 遥测日志（可选件：缺失或为空只降级，不判失败） */
    char ftp_server[64];
    uint16_t ftp_port;
    cpri_msg_header_t req_header;  /* 保存请求消息头用于响应 */
} log_upload_task_t;

/* 快照与打包用的临时目录（日志同分区，见 log_upload_init）。
 *
 * 【刻意不放进 log_upload_task_t】：handle_request 开头会对该结构整体 memset 清空，
 * 而这个目录是在 log_upload_init 里建好、只设一次的。放进结构体就会被那次 memset 抹掉，
 * 快照路径退化成 "/antenna_mgmt.log"（去写文件系统根目录，EACCES），
 * 表现为每次上传都 "Failed to create snapshot" 且结果码恒为 2 —— 已由
 * verify_log_upload.py 实测抓到过。 */
static char g_upload_tmp_dir[256];

static pthread_t g_upload_thread;
static log_upload_task_t g_upload_task;
static bool g_upload_in_progress = false;
static pthread_mutex_t g_upload_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 进程是否正在退出。上传线程每次发 TCP 报文前检查，置位后跳过发送 ——
 * 详见 log_upload_destroy()。 */
static volatile bool g_upload_stopping = false;

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

/* 使用curl执行匿名FTP上传。
 *
 * allow_resume 控制是否带 "-C -"（断点续传）。续传只对"同名即同内容前缀"的文件
 * 安全，也就是追加型的文本日志；压缩包必须传 false —— 同一天内多次请求的包内容
 * 不同，若远端残留半截同名文件，-C - 会从断点续写、把两次不同内容拼成一个无法
 * 解压的归档，而 curl 仍返回 0。静默产出坏数据比上传失败更糟。 */
static int ftp_upload_file_with_curl(const char *local_file,
                                      const char *ftp_server,
                                      uint16_t ftp_port,
                                      const char *remote_path,
                                      const char *file_name,
                                      bool allow_resume)
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
     * 使用匿名登录，带超时控制。 */
    char *curl_argv[24];
    int n = 0;

    curl_argv[n++] = (char *)"curl";
    curl_argv[n++] = (char *)"-S";
    curl_argv[n++] = (char *)"--ftp-create-dirs";
    if (allow_resume) {
        curl_argv[n++] = (char *)"-C";
        curl_argv[n++] = (char *)"-";
    }
    curl_argv[n++] = (char *)"-T";
    curl_argv[n++] = (char *)local_file;
    curl_argv[n++] = (char *)"--user";
    curl_argv[n++] = (char *)"anonymous:";
    curl_argv[n++] = (char *)"--connect-timeout";
    curl_argv[n++] = (char *)"30";
    curl_argv[n++] = (char *)"--max-time";
    curl_argv[n++] = (char *)"7200";
    curl_argv[n++] = (char *)"--speed-limit";
    curl_argv[n++] = (char *)"512";
    curl_argv[n++] = (char *)"--speed-time";
    curl_argv[n++] = (char *)"120";
    curl_argv[n++] = (char *)"--retry";
    curl_argv[n++] = (char *)"3";
    curl_argv[n++] = (char *)"--retry-delay";
    curl_argv[n++] = (char *)"5";
    curl_argv[n++] = full_url;
    curl_argv[n] = NULL;

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

/* 取路径的文件名部分，用作归档内的成员名。
 * libgen 的 basename() 会修改入参，所以先 strdup 再调（与 init 里用 dirname 同样处理）。
 * 路径异常（NULL / 空 / 以 '/' 结尾 / "." / ".."）时退回 fallback，
 * 避免归档里出现空名或怪名。 */
static void path_basename_or(const char *path, const char *fallback,
                             char *out, size_t out_size)
{
    char *copy;
    const char *base;

    if (!out || out_size == 0) {
        return;
    }

    copy = path ? strdup(path) : NULL;
    if (!copy) {
        snprintf(out, out_size, "%s", fallback ? fallback : "");
        return;
    }

    base = basename(copy);
    if (!base || base[0] == '\0' || strcmp(base, "/") == 0 ||
        strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        snprintf(out, out_size, "%s", fallback ? fallback : "");
    } else {
        snprintf(out, out_size, "%s", base);
    }

    free(copy);
}

/* 复制文件。argv 数组传参，不经过 shell（路径来自配置，非北向可控，仍统一走无 shell 路径）。
 * 文本日志轮转时 rename 与 fopen 之间有微秒级窗口，此刻 cp 会拿到 ENOENT，
 * 调用方重试一次即可。 */
static int copy_file_quiet(const char *src, const char *dst)
{
    char *cp_argv[] = { (char *)"cp", (char *)src, (char *)dst, NULL };
    char cp_out[256] = {0};
    return shell_run(cp_argv, cp_out, sizeof(cp_out));
}

/* 用 tar 把若干文件打成一个包。
 *
 * 直接用 -czf（gzip 压缩），不做"gzip 不可用就退到不压缩"的降级：本仓库
 * version_manager.c 解包下载的版本包用的就是 `tar -xzf`，gzip 已经是这个项目的
 * 硬依赖。何况降级会产出一个名为 .tgz、内容却没压缩的文件，对端 `tar xzf` 会直接
 * 报 "not in gzip format" —— 名字说谎比失败更糟。
 *
 * 用 -C <dir> + 纯文件名而不是绝对路径：tar 对绝对路径会打 "Removing leading '/'"
 * 警告，且归档里留下绝对路径名，对端解包时可能写到意外位置。 */
static int run_tar_package(const char *pkg_path, const char *dir,
                           char **names, int name_count)
{
    char *argv[10];
    char out[256] = {0};
    int n = 0;
    int i;

    argv[n++] = (char *)"tar";
    argv[n++] = (char *)"-czf";
    argv[n++] = (char *)pkg_path;
    argv[n++] = (char *)"-C";
    argv[n++] = (char *)dir;
    for (i = 0; i < name_count; i++) {
        argv[n++] = names[i];
    }
    argv[n] = NULL;

    if (shell_run(argv, out, sizeof(out)) != 0) {
        LOG_ERROR("tar failed to create package: %s", out);
        return ERROR_GENERAL;
    }
    return SUCCESS;
}

/* 日志上传线程函数。
 *
 * 上传内容是【文本日志 + 遥测日志打包成一个压缩包】，一次 FTP 传走。
 * 之所以打包而不是分两次传：IE 1211（上传结果指示）里只能装一个文件名和一个结果，
 * 传两个文件就必须发两条携带同一 serial_num 的结果指示，基带若按 serial_num 做
 * 请求-应答匹配，第二条会被当成重复报文。打包后"一次请求 → 一条结果指示"的契约不变。 */
static void* log_upload_thread_func(void *arg)
{
    log_upload_task_t *task = (log_upload_task_t *)arg;
    uint8_t result = LOG_UPLOAD_SUCCESS;
    char file_name[16] = {0};
    char text_snap[400] = {0};
    char tel_snap[400] = {0};
    char pkg_path[400] = {0};
    char text_name[64] = {0};
    char tel_name[64] = {0};
    char *names[2];
    int name_count = 0;
    bool text_created = false;
    bool tel_created = false;
    bool pkg_created = false;
    struct stat st;
    time_t now;
    struct tm tm_storage;
    struct tm *tm_info;

    LOG_INFO("Log upload thread started");

    /* 文本日志是主件：不存在就沿用原语义判"文件不存在" */
    if (stat(task->log_file_path, &st) != 0) {
        LOG_ERROR("Log file does not exist: %s", task->log_file_path);
        result = LOG_UPLOAD_FILE_NOT_EXIST;
        goto send_result;
    }

    LOG_INFO("Log file size: %.2f MB (%ld bytes)",
             st.st_size / (1024.0 * 1024.0), st.st_size);

    if (st.st_size > 100 * 1024 * 1024) {  /* 100MB */
        LOG_WARN("Large log file detected (%.2f MB), upload may take several minutes",
                 st.st_size / (1024.0 * 1024.0));
    }

    /* 归档内的成员名跟随配置的文件名（取 basename），不写死：现场改了 LOG_FILE /
     * TELEMETRY_LOG_FILE 时，包内名字与实际文件一致，排障时不会因名字对不上而误判。
     * 路径异常时退回默认名。 */
    path_basename_or(task->log_file_path, "antenna_mgmt.log", text_name, sizeof(text_name));
    path_basename_or(task->tel_log_path, "telemetry.bin", tel_name, sizeof(tel_name));

    /* 两个 basename 撞名（例如两个配置指向不同目录下的同名文件）会让快照互相覆盖、
     * 归档里出现两个同名成员（解包时后者盖前者），文本日志和遥测日志就混了。
     * 给遥测那个加后缀区分。 */
    if (strcmp(text_name, tel_name) == 0) {
        LOG_WARN("Log basenames collide (%s), renaming telemetry member", tel_name);
        snprintf(tel_name, sizeof(tel_name), "%s.tel", text_name);
    }

    /* 文本日志快照：沿用原来的"先 cp 再传"，避免上传期间文件被写而内容不一致 */
    snprintf(text_snap, sizeof(text_snap), "%s/%s", g_upload_tmp_dir, text_name);
    if (copy_file_quiet(task->log_file_path, text_snap) != 0) {
        LOG_ERROR("Failed to create snapshot of %s", task->log_file_path);
        result = LOG_UPLOAD_FAILED;
        goto send_result;
    }
    text_created = true;
    LOG_INFO("Log file snapshot created: %s", text_snap);

    /* 遥测日志是可选件：还没有 FPGA 回帧、或模块没起来时它会缺失或为空。
     * 这时只降级为"包里只有文本日志"，不让整次上传失败。 */
    if (stat(task->tel_log_path, &st) == 0 && st.st_size > 0) {
        snprintf(tel_snap, sizeof(tel_snap), "%s/%s", g_upload_tmp_dir, tel_name);
        /* 用模块内的快照而不是 cp：它持文件锁复制，与轮转互斥，且快照必然落在
         * 整帧边界上 —— cp 读的是字节流，可能把最后一帧抄一半，得到一个尾部
         * 截断的二进制记录。 */
        if (telemetry_log_snapshot(tel_snap) == SUCCESS) {
            tel_created = true;
            LOG_INFO("Telemetry log snapshot created (%ld bytes)", (long)st.st_size);
        } else {
            LOG_WARN("Telemetry log snapshot failed, packaging text log only");
        }
    } else {
        LOG_WARN("Telemetry log not available yet, packaging text log only");
    }

    /* 打包 */
    snprintf(pkg_path, sizeof(pkg_path), "%s/log_upload_pkg.tgz", g_upload_tmp_dir);
    names[name_count++] = text_name;
    if (tel_created) {
        names[name_count++] = tel_name;
    }
    if (run_tar_package(pkg_path, g_upload_tmp_dir, names, name_count) != SUCCESS) {
        result = LOG_UPLOAD_FAILED;
        goto send_result;
    }
    pkg_created = true;

    /* 远端文件名 PAAU<MMDD>.tgz = 12 字符。
     * 必须 <= 15：IE 1211 的 file_name 是 char[16]（含结尾 NUL），超了会被 snprintf
     * 静默截断，而报文里没有任何字段能暴露这件事。保持与旧版 PAAU<MMDD> 同前缀，
     * 基带侧改动最小。 */
    _Static_assert(sizeof("PAAU0000.tgz") - 1 <= 15, "remote package name too long");

    now = time(NULL);
    tm_info = localtime_r(&now, &tm_storage);
    /* localtime_r 失败是有可能的（罕见），原来直接解引用会崩；同时把月/日夹到合法范围，
     * 编译器才能证明 %02d 不会撑破缓冲区 —— 这也是 -Wformat-truncation 一直报的原因
     * （它无法推断 tm_mon/tm_mday 的取值范围）。 */
    int mon = tm_info ? (tm_info->tm_mon + 1) : 1;
    int mday = tm_info ? tm_info->tm_mday : 1;
    if (mon < 1 || mon > 12) {
        mon = 1;
    }
    if (mday < 1 || mday > 31) {
        mday = 1;
    }

    int fn_len = snprintf(file_name, sizeof(file_name), "PAAU%02d%02d.tgz", mon, mday);
    if (fn_len < 0 || (size_t)fn_len >= sizeof(file_name)) {
        /* 真被截断就明确失败，绝不把一个残缺文件名发给基带 */
        LOG_ERROR("Remote package name too long (%d), aborting upload", fn_len);
        result = LOG_UPLOAD_FAILED;
        goto send_result;
    }

    LOG_INFO("Uploading log package: %s -> %s", pkg_path, file_name);

    /* allow_resume=false：压缩包不支持断点续传，理由见 ftp_upload_file_with_curl */
    if (ftp_upload_file_with_curl(pkg_path, task->ftp_server, task->ftp_port,
                                  task->store_path, file_name, false) != SUCCESS) {
        LOG_ERROR("FTP upload of log package failed");
        result = LOG_UPLOAD_FAILED;
    } else {
        LOG_INFO("Log package uploaded successfully: %s", file_name);
    }

send_result:
    /* 清理临时文件。快照名与源文件名不同，unlink 不会误删源文件。
     * 压缩包不带续传，留着也没有复用价值，一并删掉避免占空间。 */
    if (pkg_created && unlink(pkg_path) != 0) {
        LOG_WARN("Failed to delete temporary package: %s", pkg_path);
    }
    if (tel_created && unlink(tel_snap) != 0) {
        LOG_WARN("Failed to delete telemetry snapshot: %s", tel_snap);
    }
    if (text_created && unlink(text_snap) != 0) {
        LOG_WARN("Failed to delete log snapshot: %s", text_snap);
    }

    /* 进程正在退出时不再发 TCP 报文：此刻 tcp_client 可能已销毁，会锁到已销毁的
     * send_mutex。结果指示发不出去无关紧要 —— 进程都要退了。 */
    if (!g_upload_stopping) {
        send_log_upload_result(&task->req_header, result, task->store_path, file_name);
    }

    /* 清除上传中标志 */
    pthread_mutex_lock(&g_upload_mutex);
    g_upload_in_progress = false;
    pthread_mutex_unlock(&g_upload_mutex);

    LOG_INFO("Log upload thread finished (result=%u)", result);
    return NULL;
}

int log_upload_init(void)
{
    const char *log_file;
    char *log_copy;

    g_upload_in_progress = false;

    /* 提前把打包用的临时目录建好。放在这里而不是 handle_request 里，是为了不在 TCP
     * 接收线程里 fork 一个 mkdir——接收线程阻塞期间不读 socket，BBU 心跳会积压在
     * 内核缓冲里被误判为失联（仓库里已有这个先例，见 cell_config.c 顶部的注释）。
     *
     * 目录放在文本日志的同分区而不是 /tmp：/tmp 常常是 tmpfs，打包一份十几 MB 的
     * 日志会把内存吃掉。定长字段因此存的是路径而非拼接后的目录名。 */
    log_file = config_get_string("LOG_FILE", "./logs/antenna_mgmt.log");
    log_copy = strdup(log_file);
    if (log_copy) {
        /* dirname 会修改入参，所以必须传 strdup 出来的副本 */
        char *dir = dirname(log_copy);
        snprintf(g_upload_tmp_dir, sizeof(g_upload_tmp_dir),
                 "%s/.upload_tmp", dir);

        char *mkdir_argv[] = { (char *)"mkdir", (char *)"-p", g_upload_tmp_dir, NULL };
        char mkdir_out[256] = {0};
        if (shell_run(mkdir_argv, mkdir_out, sizeof(mkdir_out)) != 0) {
            LOG_WARN("Failed to create log upload temp dir: %s", g_upload_tmp_dir);
        }
        free(log_copy);
    } else {
        strncpy(g_upload_tmp_dir, ".upload_tmp", sizeof(g_upload_tmp_dir) - 1);
    }

    LOG_INFO("Log upload module initialized (tmp_dir=%s)", g_upload_tmp_dir);
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

    /* 遥测日志路径直接问模块要（它已按 TELEMETRY_LOG_FILE 初始化，未初始化时返回
     * 默认路径），避免在这里再读一次配置、造成两处默认值日后漂移。
     * tmp_dir 已在 log_upload_init 里建好，这里不重复计算。 */
    strncpy(g_upload_task.tel_log_path, telemetry_log_get_path(),
            sizeof(g_upload_task.tel_log_path) - 1);

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
    /* 置停止位：上传线程在发 TCP 报文前会检查它，置位后不再发送。
     *
     * 【刻意不 join 上传线程】curl 的 --max-time 是 7200 秒，join 可能把退出流程
     * 卡住两小时。因此这里只是缩小竞态窗口、并不消除它：线程若已越过那个检查、
     * 正处在 tcp_client_send 内部，仍可能撞上已被销毁的 send_mutex。
     * 彻底消除需要可中断的上传或限时 join（pthread_timedjoin_np 是 GNU 扩展），
     * 超出本次改动范围。调用方（main.c）已把本函数前移到 tcp_client_stop 之前，
     * 使置位一定发生在 TCP 拆卸之前。 */
    g_upload_stopping = true;

    pthread_mutex_lock(&g_upload_mutex);
    if (g_upload_in_progress) {
        LOG_WARN("Log upload still in progress at shutdown, not waiting for it");
    }
    pthread_mutex_unlock(&g_upload_mutex);

    LOG_INFO("Log upload module destroyed");
}
