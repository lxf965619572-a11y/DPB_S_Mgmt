#include "version_manager.h"
#include "logger.h"
#include "tcp_client.h"
#include "config.h"
#include "fpga_firmware_injector.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <openssl/sha.h>

/* 外部TCP客户端引用 */
extern tcp_client_t g_tcp_client;

/* 全局下载任务 */
static version_download_task_t g_download_task;
static pthread_t g_download_thread;
static pthread_mutex_t g_download_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 当前版本信息 */
static char g_current_version[MAX_VERSION_LEN] = {0};

/* 创建目录（递归） */
static int create_directory_recursive(const char *path)
{
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return ERROR_GENERAL;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return ERROR_GENERAL;
    }
    return SUCCESS;
}

/* 计算文件SHA-256校验和 */
static int calculate_sha256(const char *file_path, char *checksum_hex)
{
    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        LOG_ERROR("Failed to open file for checksum: %s", file_path);
        return ERROR_GENERAL;
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);

    unsigned char buffer[8192];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        SHA256_Update(&sha256, buffer, bytes);
    }
    SHA256_Final(hash, &sha256);
    fclose(fp);

    /* 转换为十六进制字符串 */
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(checksum_hex + (i * 2), "%02x", hash[i]);
    }
    checksum_hex[64] = '\0';

    return SUCCESS;
}

/* 解压tar文件 */
static int extract_tar_file(const char *tar_path, const char *dest_dir)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\" 2>&1", tar_path, dest_dir);

    LOG_INFO("Extracting tar file: %s", cmd);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        LOG_ERROR("Failed to execute tar command");
        return ERROR_GENERAL;
    }

    char output[1024] = {0};
    size_t bytes_read = fread(output, 1, sizeof(output) - 1, fp);
    int status = pclose(fp);

    if (bytes_read > 0) {
        output[bytes_read] = '\0';
        if (strlen(output) > 0) {
            LOG_WARN("tar output: %s", output);
        }
    }

    if (status != 0) {
        LOG_ERROR("tar command failed with status: %d", status);
        return ERROR_GENERAL;
    }

    LOG_INFO("Tar extraction completed successfully");
    return SUCCESS;
}

/* 从.txt文件读取预期校验和 */
static int read_checksum_from_txt(const char *version_dir, char *expected_checksum)
{
    DIR *dir = opendir(version_dir);
    if (!dir) {
        LOG_ERROR("Failed to open version directory: %s", version_dir);
        return ERROR_GENERAL;
    }

    /* 查找.txt文件 */
    struct dirent *entry;
    char txt_file[512] = {0};
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".txt") == 0) {
            snprintf(txt_file, sizeof(txt_file), "%s/%s", version_dir, entry->d_name);
            LOG_INFO("Found checksum file: %s", entry->d_name);
            break;
        }
    }
    closedir(dir);

    if (txt_file[0] == '\0') {
        LOG_ERROR("No .txt checksum file found in version directory");
        return ERROR_GENERAL;
    }

    /* 读取校验和 */
    FILE *fp = fopen(txt_file, "r");
    if (!fp) {
        LOG_ERROR("Failed to open checksum file: %s", txt_file);
        return ERROR_GENERAL;
    }

    char line[256];
    bool found = false;

    /* 读取第一行或查找包含校验和的行 */
    while (fgets(line, sizeof(line), fp)) {
        /* 移除空白字符 */
        char *start = line;
        while (*start == ' ' || *start == '\t') start++;

        /* 跳过空行和注释 */
        if (*start == '\0' || *start == '\n' || *start == '#') {
            continue;
        }

        /* 查找 sha256= 格式 */
        char *sha256_pos = strstr(start, "sha256=");
        if (sha256_pos) {
            sha256_pos += 7;  /* 跳过 "sha256=" */
            /* 提取64位十六进制校验和 */
            int i = 0;
            while (i < 64 && sha256_pos[i] != '\0' && sha256_pos[i] != '\n' &&
                   sha256_pos[i] != ' ' && sha256_pos[i] != '\t' && sha256_pos[i] != '\r') {
                char c = sha256_pos[i];
                if (!((c >= '0' && c <= '9') ||
                      (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F'))) {
                    break;
                }
                expected_checksum[i] = c;
                i++;
            }

            if (i == 64) {
                expected_checksum[64] = '\0';
                found = true;
                LOG_INFO("Read expected checksum from txt: %s", expected_checksum);
                break;
            }
        }

        /* 兼容旧格式：直接提取64位十六进制校验和 */
        int i = 0;
        while (i < 64 && start[i] != '\0' && start[i] != '\n' &&
               start[i] != ' ' && start[i] != '\t') {
            char c = start[i];
            if (!((c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) {
                break;
            }
            expected_checksum[i] = c;
            i++;
        }

        if (i == 64) {
            expected_checksum[64] = '\0';
            found = true;
            LOG_INFO("Read expected checksum from txt: %s", expected_checksum);
            break;
        }
    }

    fclose(fp);

    if (!found) {
        LOG_ERROR("Failed to parse checksum from txt file");
        return ERROR_GENERAL;
    }

    return SUCCESS;
}

/* 验证.bin可执行文件的校验和 */
static int verify_bin_file_checksum(const char *version_dir)
{
    DIR *dir = opendir(version_dir);
    if (!dir) {
        LOG_ERROR("Failed to open version directory: %s", version_dir);
        return DOWNLOAD_RESULT_CHECKSUM;
    }

    /* 查找.bin文件或可执行文件（无后缀） */
    struct dirent *entry;
    char bin_file[512] = {0};
    char executable_file[512] = {0};

    while ((entry = readdir(dir)) != NULL) {
        /* 跳过目录和隐藏文件 */
        if (entry->d_name[0] == '.') {
            continue;
        }

        size_t len = strlen(entry->d_name);

        /* 优先查找.bin文件 */
        if (len > 4 && strcmp(entry->d_name + len - 4, ".bin") == 0) {
            snprintf(bin_file, sizeof(bin_file), "%s/%s", version_dir, entry->d_name);
            LOG_INFO("Found bin file: %s", entry->d_name);
            break;
        }

        /* 查找可执行文件（无后缀，排除.txt等文件） */
        if (strchr(entry->d_name, '.') == NULL) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", version_dir, entry->d_name);

            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR)) {
                /* 找到可执行文件 */
                snprintf(executable_file, sizeof(executable_file), "%s", full_path);
                LOG_INFO("Found executable file: %s", entry->d_name);
            }
        }
    }
    closedir(dir);

    /* 优先使用.bin文件，如果没有则使用可执行文件 */
    if (bin_file[0] != '\0') {
        /* 使用.bin文件 */
        snprintf(executable_file, sizeof(executable_file), "%s", bin_file);
    } else if (executable_file[0] == '\0') {
        LOG_ERROR("No .bin or executable file found in version directory");
        return DOWNLOAD_RESULT_EXTRACT;
    }

    /* 读取预期校验和 */
    char expected_checksum[65];
    if (read_checksum_from_txt(version_dir, expected_checksum) != SUCCESS) {
        LOG_ERROR("Failed to read expected checksum from txt file");
        return DOWNLOAD_RESULT_CHECKSUM;
    }

    /* 计算可执行文件的实际校验和 */
    char actual_checksum[65];
    if (calculate_sha256(executable_file, actual_checksum) != SUCCESS) {
        LOG_ERROR("Failed to calculate executable file checksum");
        return DOWNLOAD_RESULT_CHECKSUM;
    }

    LOG_INFO("Executable file: %s", executable_file);
    LOG_INFO("Expected checksum: %s", expected_checksum);
    LOG_INFO("Actual checksum:   %s", actual_checksum);

    /* 严格比对（大小写不敏感） */
    if (strcasecmp(actual_checksum, expected_checksum) != 0) {
        LOG_ERROR("SECURITY ALERT: Executable file checksum mismatch!");
        LOG_ERROR("File may be corrupted or tampered!");
        return DOWNLOAD_RESULT_CHECKSUM;
    }

    LOG_INFO("Executable file checksum verification PASSED");
    return DOWNLOAD_RESULT_SUCCESS;
}


/* 查找版本目录（支持大小写不敏感匹配）
 * 返回实际的版本目录路径，如果找不到返回 NULL
 */
static char* find_version_directory(const char *version, char *actual_path, size_t path_size)
{
    char version_dir[512];
    struct stat st;

    /* 首先尝试精确匹配 */
    snprintf(version_dir, sizeof(version_dir), "%s/%s", VERSION_BASE_DIR, version);
    if (stat(version_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        strncpy(actual_path, version_dir, path_size - 1);
        actual_path[path_size - 1] = '\0';
        return actual_path;
    }

    /* 精确匹配失败，尝试大小写不敏感匹配 */
    DIR *dir = opendir(VERSION_BASE_DIR);
    if (!dir) {
        LOG_ERROR("Failed to open version base directory: %s", VERSION_BASE_DIR);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* 跳过 . 和 .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* 大小写不敏感比较 */
        if (strcasecmp(entry->d_name, version) == 0) {
            snprintf(version_dir, sizeof(version_dir), "%s/%s", VERSION_BASE_DIR, entry->d_name);

            /* 验证是否为目录 */
            if (stat(version_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                strncpy(actual_path, version_dir, path_size - 1);
                actual_path[path_size - 1] = '\0';
                closedir(dir);
                LOG_INFO("Found version directory (case-insensitive): %s -> %s", version, entry->d_name);
                return actual_path;
            }
        }
    }

    closedir(dir);
    return NULL;
}

/* 保存版本元数据 */
static int save_version_metadata(const version_metadata_t *metadata)
{
    char meta_path[512];
    snprintf(meta_path, sizeof(meta_path), "%s/%s/version.json",
            VERSION_BASE_DIR, metadata->version);

    FILE *fp = fopen(meta_path, "w");
    if (!fp) {
        LOG_ERROR("Failed to create metadata file: %s", meta_path);
        return ERROR_GENERAL;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"version\": \"%s\",\n", metadata->version);
    fprintf(fp, "  \"build_time\": \"%s\",\n", metadata->build_time);
    fprintf(fp, "  \"checksum\": \"%s\",\n", metadata->checksum);
    fprintf(fp, "  \"file_size\": %u,\n", metadata->file_size);
    fprintf(fp, "  \"ver_type\": %u,\n", metadata->ver_type);
    fprintf(fp, "  \"install_path\": \"%s\"\n", metadata->install_path);
    fprintf(fp, "}\n");

    fclose(fp);
    LOG_INFO("Version metadata saved: %s", meta_path);
    return SUCCESS;
}

/* 加载版本元数据 */
static int load_version_metadata(const char *version, version_metadata_t *metadata)
{
    char meta_path[512];

    /* 优先尝试读取metadata.txt（新格式） */
    snprintf(meta_path, sizeof(meta_path), "%s/%s/metadata.txt",
            VERSION_BASE_DIR, version);

    FILE *fp = fopen(meta_path, "r");
    if (!fp) {
        /* 如果metadata.txt不存在，尝试version.json（旧格式） */
        snprintf(meta_path, sizeof(meta_path), "%s/%s/version.json",
                VERSION_BASE_DIR, version);
        fp = fopen(meta_path, "r");
        if (!fp) {
            LOG_ERROR("Failed to open metadata file (tried metadata.txt and version.json)");
            return ERROR_GENERAL;
        }
    }

    char line[512];
    memset(metadata, 0, sizeof(version_metadata_t));

    /* 检测文件格式 */
    bool is_txt_format = (strstr(meta_path, "metadata.txt") != NULL);

    while (fgets(line, sizeof(line), fp)) {
        /* 移除换行符 */
        line[strcspn(line, "\r\n")] = 0;

        if (is_txt_format) {
            /* metadata.txt格式: key=value */
            /* 跳过注释和空行 */
            if (line[0] == '#' || line[0] == '\0') {
                continue;
            }

            /* 解析键值对 */
            char *eq = strchr(line, '=');
            if (!eq) continue;

            *eq = '\0';  /* 分割键和值 */
            char *key = line;
            char *value = eq + 1;

            /* 去除键前后的空格 */
            while (*key == ' ' || *key == '\t') key++;
            char *key_end = key + strlen(key) - 1;
            while (key_end > key && (*key_end == ' ' || *key_end == '\t')) {
                *key_end = '\0';
                key_end--;
            }

            /* 去除值前后的空格 */
            while (*value == ' ' || *value == '\t') value++;
            char *value_end = value + strlen(value) - 1;
            while (value_end > value && (*value_end == ' ' || *value_end == '\t')) {
                *value_end = '\0';
                value_end--;
            }

            /* 解析各个字段 */
            if (strcmp(key, "version") == 0) {
                strncpy(metadata->version, value, sizeof(metadata->version) - 1);
            } else if (strcmp(key, "sha256") == 0) {
                strncpy(metadata->checksum, value, sizeof(metadata->checksum) - 1);
            } else if (strcmp(key, "file_type") == 0) {
                /* 可选字段，暂不使用 */
            } else if (strcmp(key, "file_sub_type") == 0) {
                /* 可选字段，暂不使用 */
            } else if (strcmp(key, "bin_file") == 0) {
                /* 可选字段，暂不使用 */
            }
        } else {
            /* version.json格式: JSON */
            if (strstr(line, "\"version\"")) {
                sscanf(line, "  \"version\": \"%39[^\"]\"", metadata->version);
            } else if (strstr(line, "\"build_time\"")) {
                sscanf(line, "  \"build_time\": \"%19[^\"]\"", metadata->build_time);
            } else if (strstr(line, "\"checksum\"")) {
                sscanf(line, "  \"checksum\": \"%64[^\"]\"", metadata->checksum);
            } else if (strstr(line, "\"file_size\"")) {
                sscanf(line, "  \"file_size\": %u", &metadata->file_size);
            } else if (strstr(line, "\"ver_type\"")) {
                sscanf(line, "  \"ver_type\": %hhu", &metadata->ver_type);
            } else if (strstr(line, "\"install_path\"")) {
                sscanf(line, "  \"install_path\": \"%255[^\"]\"", metadata->install_path);
            }
        }
    }

    fclose(fp);

    /* 验证必要字段 */
    if (metadata->version[0] == '\0') {
        LOG_WARN("Version field not found in metadata, using directory name: %s", version);
        strncpy(metadata->version, version, sizeof(metadata->version) - 1);
    }

    LOG_INFO("Loaded metadata: version=%s, checksum=%s",
             metadata->version, metadata->checksum);

    return SUCCESS;
}

/* 使用curl执行FTP下载（支持断点续传） */
static int ftp_download_file_with_curl(const char *ftp_server,
                                        uint16_t ftp_port,
                                        const char *remote_path,
                                        const char *remote_file,
                                        const char *local_file,
                                        int retry_count)
{
    char cmd[1024];
    char full_url[512];
    int attempt = 0;

    /* 构造FTP URL */
    if (remote_path && remote_path[0] != '\0') {
        if (remote_path[0] == '/') {
            snprintf(full_url, sizeof(full_url), "ftp://%s:%u%s/%s",
                    ftp_server, ftp_port, remote_path, remote_file);
        } else {
            snprintf(full_url, sizeof(full_url), "ftp://%s:%u/%s/%s",
                    ftp_server, ftp_port, remote_path, remote_file);
        }
    } else {
        snprintf(full_url, sizeof(full_url), "ftp://%s:%u/%s",
                ftp_server, ftp_port, remote_file);
    }

    /* 重试下载 */
    for (attempt = 0; attempt < retry_count; attempt++) {
        if (attempt > 0) {
            LOG_WARN("Retry download attempt %d/%d", attempt + 1, retry_count);
            sleep(5);  /* 重试间隔5秒 */
        }

        /* 构造curl命令 - 简化参数以提高兼容性 */
        snprintf(cmd, sizeof(cmd),
                "curl -s -S --connect-timeout 30 --max-time 600 "
                "-o \"%s\" \"%s\" 2>&1",
                local_file, full_url);

        LOG_INFO("Executing FTP download (attempt %d): %s", attempt + 1, cmd);

        FILE *fp = popen(cmd, "r");
        if (!fp) {
            LOG_ERROR("Failed to execute curl command");
            continue;
        }

        char output[1024] = {0};
        size_t bytes_read = fread(output, 1, sizeof(output) - 1, fp);
        int status = pclose(fp);

        if (bytes_read > 0) {
            output[bytes_read] = '\0';
            if (strlen(output) > 0) {
                LOG_WARN("curl output: %s", output);
            }
        }

        if (status == 0) {
            LOG_INFO("FTP download completed successfully");
            return SUCCESS;
        }

        LOG_ERROR("curl command failed with status: %d", status);
    }

    LOG_ERROR("FTP download failed after %d attempts", retry_count);
    return ERROR_GENERAL;
}

/* 发送版本下载应答 (MsgID: 22) */
static int send_version_download_ack(const cpri_msg_header_t *req_header,
                                     uint8_t ver_type,
                                     uint32_t result)
{
    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    response.header.msg_id = MSG_VERSION_DOWNLOAD_ACK;
    response.header.paau_id = req_header->paau_id;
    response.header.bbu_id = req_header->bbu_id;
    response.header.port_num = req_header->port_num;
    response.header.serial_num = req_header->serial_num;

    ie_version_download_ack_t ie_ack;
    ie_ack.header.ie_type = (IE_VERSION_DOWNLOAD_ACK);
    ie_ack.header.ie_length = (sizeof(ie_version_download_ack_t));
    ie_ack.ver_type = ver_type;
    ie_ack.result = result;

    response.payload_len = sizeof(ie_version_download_ack_t);
    response.payload = (uint8_t *)malloc(response.payload_len);
    if (!response.payload) {
        LOG_ERROR("Failed to allocate memory for download ack");
        return ERROR_MEMORY;
    }
    memcpy(response.payload, &ie_ack, sizeof(ie_version_download_ack_t));

    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent version download ack (serial=%u, result=%u)",
                 response.header.serial_num, result);
    }

    cpri_free_message(&response);
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

/* 发送版本下载结果指示 (MsgID: 23) */
static int send_version_download_result(const cpri_msg_header_t *req_header,
                                        uint8_t ver_type,
                                        uint32_t result)
{
    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    response.header.msg_id = MSG_VERSION_DOWNLOAD_RESULT_IND;
    response.header.paau_id = req_header->paau_id;
    response.header.bbu_id = req_header->bbu_id;
    response.header.port_num = req_header->port_num;
    response.header.serial_num = req_header->serial_num;

    ie_version_download_result_t ie_result;
    ie_result.header.ie_type = (IE_VERSION_DOWNLOAD_RESULT);
    ie_result.header.ie_length = (sizeof(ie_version_download_result_t));
    ie_result.ver_type = ver_type;
    ie_result.result = result;

    response.payload_len = sizeof(ie_version_download_result_t);
    response.payload = (uint8_t *)malloc(response.payload_len);
    if (!response.payload) {
        LOG_ERROR("Failed to allocate memory for download result");
        return ERROR_MEMORY;
    }
    memcpy(response.payload, &ie_result, sizeof(ie_version_download_result_t));

    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent version download result (serial=%u, result=%u)",
                 response.header.serial_num, result);
    }

    cpri_free_message(&response);
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

/* 版本下载线程函数 */
static void* version_download_thread_func(void *arg)
{
    version_download_task_t *task = (version_download_task_t *)arg;
    uint32_t result = DOWNLOAD_RESULT_SUCCESS;
    char download_path[512];
    char version_dir[512];
    char checksum[65];

    LOG_INFO("Version download thread started for version: %s", task->version);

    /* 创建下载目录 */
    if (create_directory_recursive(VERSION_DOWNLOAD_DIR) != SUCCESS) {
        LOG_ERROR("Failed to create download directory");
        result = DOWNLOAD_RESULT_OTHER;
        goto send_result;
    }

    /* 下载文件路径 */
    snprintf(download_path, sizeof(download_path), "%s/%s",
            VERSION_DOWNLOAD_DIR, task->file_name);

    /* 执行FTP下载（3次重试） */
    int ret = ftp_download_file_with_curl(task->ftp_server,
                                          task->ftp_port,
                                          task->file_path,
                                          task->file_name,
                                          download_path,
                                          3);
    if (ret != SUCCESS) {
        LOG_ERROR("FTP download failed");
        result = DOWNLOAD_RESULT_TIMEOUT;
        goto send_result;
    }

    /* 检查文件大小 */
    struct stat st;
    if (stat(download_path, &st) != 0) {
        LOG_ERROR("Downloaded file not found: %s", download_path);
        result = DOWNLOAD_RESULT_NOT_EXIST;
        goto send_result;
    }

    if (st.st_size != task->file_len) {
        LOG_WARN("File size mismatch: expected=%u, actual=%ld",
                task->file_len, st.st_size);
    }

    /* 计算SHA-256校验和 */
    LOG_INFO("Calculating checksum for downloaded file...");
    if (calculate_sha256(download_path, checksum) != SUCCESS) {
        LOG_ERROR("Failed to calculate checksum");
        result = DOWNLOAD_RESULT_CHECKSUM;
        goto send_result;
    }
    LOG_INFO("File checksum: %s", checksum);

    /* 创建版本目录 */
    snprintf(version_dir, sizeof(version_dir), "%s/%s",
            VERSION_BASE_DIR, task->version);
    if (create_directory_recursive(version_dir) != SUCCESS) {
        LOG_ERROR("Failed to create version directory: %s", version_dir);
        result = DOWNLOAD_RESULT_OTHER;
        goto send_result;
    }

    /* 解压tar文件 */
    LOG_INFO("Extracting tar file to: %s", version_dir);
    if (extract_tar_file(download_path, version_dir) != SUCCESS) {
        LOG_ERROR("Failed to extract tar file");
        result = DOWNLOAD_RESULT_EXTRACT;
        goto send_result;
    }

    /* 【关键】验证文件的校验和 */
    LOG_INFO("Verifying file checksum (ver_type=%u)...", task->ver_type);
    result = verify_bin_file_checksum(version_dir);
    if (result != DOWNLOAD_RESULT_SUCCESS) {
        LOG_ERROR("File checksum verification FAILED");
        /* 删除已解压的文件（安全措施） */
        char cleanup_cmd[1024];
        snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf \"%s\"", version_dir);
        int cleanup_ret = system(cleanup_cmd);
        if (cleanup_ret != 0) {
            LOG_WARN("Failed to cleanup corrupted directory: %s (ret=%d)", version_dir, cleanup_ret);
        }
        LOG_WARN("Removed corrupted version directory: %s", version_dir);
        goto send_result;
    }

    /* 从 metadata.txt 读取真实版本号（仅固件版本） */
    version_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));

    if (task->ver_type == VERSION_TYPE_FIRMWARE) {
        /* 固件版本：从 metadata.txt 读取版本号 */
        if (load_version_metadata(task->version, &metadata) == SUCCESS && metadata.version[0] != '\0') {
            /* 如果从 metadata.txt 读取的版本号与临时版本号不同，需要重命名目录 */
            if (strcmp(metadata.version, task->version) != 0) {
                char new_version_dir[512];
                snprintf(new_version_dir, sizeof(new_version_dir), "%s/%s",
                        VERSION_BASE_DIR, metadata.version);

                LOG_INFO("Real firmware version from metadata.txt: %s (temp version was: %s)",
                        metadata.version, task->version);

                /* 检查目标目录是否已存在 */
                struct stat st_check;
                if (stat(new_version_dir, &st_check) == 0) {
                    LOG_WARN("Version directory already exists: %s, removing old version", new_version_dir);
                    char rm_cmd[1024];
                    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", new_version_dir);
                    system(rm_cmd);
                }

                /* 重命名目录 */
                if (rename(version_dir, new_version_dir) != 0) {
                    LOG_ERROR("Failed to rename version directory from %s to %s: %s",
                             version_dir, new_version_dir, strerror(errno));
                    result = DOWNLOAD_RESULT_OTHER;
                    goto send_result;
                }

                /* 更新版本目录路径和任务中的版本号 */
                strncpy(version_dir, new_version_dir, sizeof(version_dir) - 1);
                strncpy(task->version, metadata.version, sizeof(task->version) - 1);
                LOG_INFO("Firmware version directory renamed to: %s", version_dir);
            }
        } else {
            LOG_WARN("Failed to load metadata.txt for firmware, using temporary version: %s", task->version);
            strncpy(metadata.version, task->version, sizeof(metadata.version) - 1);
        }
    } else {
        /* 软件版本：使用文件名或 file_ver 作为版本号，不从 metadata.txt 读取 */
        LOG_INFO("Software version: using version from filename: %s", task->version);
        strncpy(metadata.version, task->version, sizeof(metadata.version) - 1);
    }

    /* 保存版本元数据 */
    strncpy(metadata.build_time, task->file_time, sizeof(metadata.build_time) - 1);
    strncpy(metadata.checksum, checksum, sizeof(metadata.checksum) - 1);
    metadata.file_size = st.st_size;
    metadata.ver_type = task->ver_type;
    snprintf(metadata.install_path, sizeof(metadata.install_path), "%s", version_dir);

    if (save_version_metadata(&metadata) != SUCCESS) {
        LOG_ERROR("Failed to save version metadata");
        result = DOWNLOAD_RESULT_OTHER;
        goto send_result;
    }

    /* 删除下载的tar文件 */
    unlink(download_path);

    LOG_INFO("Version %s downloaded and installed successfully", task->version);

send_result:
    /* 发送下载结果指示 */
    send_version_download_result(&task->req_header, task->ver_type, result);

    /* 清除下载中标志 */
    pthread_mutex_lock(&g_download_mutex);
    g_download_task.in_progress = false;
    pthread_mutex_unlock(&g_download_mutex);

    LOG_INFO("Version download thread finished");
    return NULL;
}

/* 发送版本激活应答 (MsgID: 32) */
static int send_version_activate_ack(const cpri_msg_header_t *req_header,
                                     uint8_t ver_type,
                                     uint8_t result)
{
    cpri_message_t response;
    memset(&response, 0, sizeof(response));

    response.header.msg_id = MSG_PAAU_VERSION_ACTIVATE_ACK;
    response.header.paau_id = req_header->paau_id;
    response.header.bbu_id = req_header->bbu_id;
    response.header.port_num = req_header->port_num;
    response.header.serial_num = req_header->serial_num;

    ie_version_activate_ack_t ie_ack;
    ie_ack.header.ie_type = (IE_VERSION_ACTIVATE_ACK);
    ie_ack.header.ie_length = (sizeof(ie_version_activate_ack_t));
    ie_ack.ver_type = ver_type;
    ie_ack.result = result;

    response.payload_len = sizeof(ie_version_activate_ack_t);
    response.payload = (uint8_t *)malloc(response.payload_len);
    if (!response.payload) {
        LOG_ERROR("Failed to allocate memory for activate ack");
        return ERROR_MEMORY;
    }
    memcpy(response.payload, &ie_ack, sizeof(ie_version_activate_ack_t));

    uint8_t buffer[2048];
    int len = cpri_encode_message(&response, buffer, sizeof(buffer));
    if (len > 0) {
        tcp_client_send(&g_tcp_client, buffer, len);
        LOG_INFO("Sent version activate ack (serial=%u, result=%u)",
                 response.header.serial_num, result);
    }

    cpri_free_message(&response);
    return (len > 0) ? SUCCESS : ERROR_GENERAL;
}

/* 切换到指定版本（原子操作） */
static int switch_to_version(const char *version, const char *reason)
{
    char version_dir[512];
    char rollback_dir[512];
    char timestamp[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    /* 生成时间戳 */
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

    /* 检查目标版本是否存在 */
    snprintf(version_dir, sizeof(version_dir), "%s/%s", VERSION_BASE_DIR, version);
    struct stat st;
    if (stat(version_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        LOG_ERROR("Version directory not found: %s", version_dir);
        return ACTIVATE_RESULT_NOT_EXIST;
    }

    /* 检查版本元数据 */
    version_metadata_t metadata;
    if (load_version_metadata(version, &metadata) != SUCCESS) {
        LOG_ERROR("Failed to load version metadata");
        return ACTIVATE_RESULT_CORRUPTED;
    }

    /* 备份当前版本 */
    char current_link[512];
    snprintf(current_link, sizeof(current_link), "%s", VERSION_CURRENT_LINK);

    if (lstat(current_link, &st) == 0 && S_ISLNK(st.st_mode)) {
        /* 创建回滚目录 */
        snprintf(rollback_dir, sizeof(rollback_dir), "%s/%s",
                VERSION_ROLLBACK_DIR, timestamp);
        if (create_directory_recursive(rollback_dir) != SUCCESS) {
            LOG_ERROR("Failed to create rollback directory");
            return ACTIVATE_RESULT_OTHER;
        }

        /* 读取当前符号链接指向 */
        char current_target[512];
        ssize_t len = readlink(current_link, current_target, sizeof(current_target) - 1);
        if (len > 0) {
            current_target[len] = '\0';

            /* 保存回滚信息 */
            char rollback_info[1024];
            snprintf(rollback_info, sizeof(rollback_info), "%s/rollback.json", rollback_dir);
            FILE *fp = fopen(rollback_info, "w");
            if (fp) {
                fprintf(fp, "{\n");
                fprintf(fp, "  \"timestamp\": \"%s\",\n", timestamp);
                fprintf(fp, "  \"previous_version\": \"%s\",\n", current_target);
                fprintf(fp, "  \"new_version\": \"%s\",\n", version);
                fprintf(fp, "  \"reason\": \"%s\"\n", reason ? reason : "upgrade");
                fprintf(fp, "}\n");
                fclose(fp);
            }

            LOG_INFO("Backed up current version info to: %s", rollback_dir);
        }
    }

    /* 删除旧的符号链接 */
    unlink(current_link);

    /* 创建新的符号链接 */
    if (symlink(version_dir, current_link) != 0) {
        LOG_ERROR("Failed to create symlink: %s -> %s", current_link, version_dir);
        return ACTIVATE_RESULT_OTHER;
    }

    LOG_INFO("Successfully switched to version: %s", version);
    strncpy(g_current_version, version, sizeof(g_current_version) - 1);

    return ACTIVATE_RESULT_SUCCESS;
}

/* 公共接口实现 */

int version_manager_init(void)
{
    /* 创建必要的目录 */
    create_directory_recursive(VERSION_BASE_DIR);
    create_directory_recursive(VERSION_ROLLBACK_DIR);
    create_directory_recursive(VERSION_DOWNLOAD_DIR);

    /* 读取当前版本 */
    char current_link[512];
    snprintf(current_link, sizeof(current_link), "%s", VERSION_CURRENT_LINK);

    struct stat st;
    if (lstat(current_link, &st) == 0 && S_ISLNK(st.st_mode)) {
        char target[512];
        ssize_t len = readlink(current_link, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            /* 提取版本号 */
            char *version_name = strrchr(target, '/');
            if (version_name) {
                strncpy(g_current_version, version_name + 1, sizeof(g_current_version) - 1);
                LOG_INFO("Current version: %s", g_current_version);
            }
        }
    } else {
        LOG_WARN("No current version symlink found");
    }

    g_download_task.in_progress = false;
    LOG_INFO("Version manager initialized");
    return SUCCESS;
}

int version_handle_download_request(const cpri_message_t *msg)
{
    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid version download request message");
        return ERROR_INVALID_PARAM;
    }

    /* 检查是否已有下载任务在进行 */
    pthread_mutex_lock(&g_download_mutex);
    if (g_download_task.in_progress) {
        pthread_mutex_unlock(&g_download_mutex);
        LOG_WARN("Version download already in progress, rejecting request");

        ie_version_download_req_t *ie_req = (ie_version_download_req_t *)msg->payload;
        send_version_download_ack(&msg->header, ie_req->ver_type, DOWNLOAD_ACK_REJECT_BUSY);
        return ERROR_GENERAL;
    }
    g_download_task.in_progress = true;
    pthread_mutex_unlock(&g_download_mutex);

    /* 解析IE 14 */
    ie_version_download_req_t *ie_req = (ie_version_download_req_t *)msg->payload;

    LOG_INFO("Received version download request: version=%s, file=%s",
            ie_req->file_ver, ie_req->file_name);

    /* 准备下载任务 */
    memset(&g_download_task, 0, sizeof(g_download_task));

    /* 根据版本类型确定临时版本号 */
    if (ie_req->ver_type == VERSION_TYPE_SOFTWARE) {
        /* 软件版本：必须使用 BBU 提供的 file_ver 作为版本号 */
        if (ie_req->file_ver[0] != '\0' && strlen(ie_req->file_ver) > 0) {
            strncpy(g_download_task.version, ie_req->file_ver, sizeof(g_download_task.version) - 1);
            LOG_INFO("Software version from BBU: %s", g_download_task.version);
        } else {
            /* 如果 BBU 没有提供版本号，从文件名提取（兼容处理） */
            const char *dot_pos = strstr(ie_req->file_name, ".tar.gz");
            if (dot_pos) {
                size_t len = dot_pos - ie_req->file_name;
                if (len < sizeof(g_download_task.version)) {
                    strncpy(g_download_task.version, ie_req->file_name, len);
                    g_download_task.version[len] = '\0';
                } else {
                    strncpy(g_download_task.version, ie_req->file_name, sizeof(g_download_task.version) - 1);
                }
            } else {
                strncpy(g_download_task.version, ie_req->file_name, sizeof(g_download_task.version) - 1);
            }
            LOG_WARN("Software version not provided by BBU, extracted from filename: %s", g_download_task.version);
        }
    } else {
        /* 固件版本：使用临时版本号，真实版本号将从 metadata.txt 读取 */
        if (ie_req->file_ver[0] != '\0' && strlen(ie_req->file_ver) > 0) {
            strncpy(g_download_task.version, ie_req->file_ver, sizeof(g_download_task.version) - 1);
            LOG_INFO("Firmware temporary version: %s", g_download_task.version);
        } else {
            /* 从文件名提取临时版本号 */
            const char *dot_pos = strstr(ie_req->file_name, ".tar.gz");
            if (dot_pos) {
                size_t len = dot_pos - ie_req->file_name;
                if (len < sizeof(g_download_task.version)) {
                    strncpy(g_download_task.version, ie_req->file_name, len);
                    g_download_task.version[len] = '\0';
                } else {
                    strncpy(g_download_task.version, ie_req->file_name, sizeof(g_download_task.version) - 1);
                }
            } else {
                strncpy(g_download_task.version, ie_req->file_name, sizeof(g_download_task.version) - 1);
            }
            LOG_INFO("Firmware temporary version from filename: %s (will read real version from metadata.txt)",
                     g_download_task.version);
        }
    }

    strncpy(g_download_task.file_path, ie_req->file_path, sizeof(g_download_task.file_path) - 1);
    strncpy(g_download_task.file_name, ie_req->file_name, sizeof(g_download_task.file_name) - 1);
    strncpy(g_download_task.file_time, ie_req->file_time, sizeof(g_download_task.file_time) - 1);
    g_download_task.file_len = ie_req->file_len;
    g_download_task.ver_type = ie_req->ver_type;
    g_download_task.in_progress = true;

    /* 设置FTP服务器信息 */
    const char *ftp_server = config_get_string("FTP_SERVER", g_config.bbu_ip);
    strncpy(g_download_task.ftp_server, ftp_server, sizeof(g_download_task.ftp_server) - 1);
    g_download_task.ftp_port = config_get_int("FTP_PORT", 21);

    /* 保存请求消息头 */
    memcpy(&g_download_task.req_header, &msg->header, sizeof(cpri_msg_header_t));

    /* 发送应答 - 接受请求 */
    int ret = send_version_download_ack(&msg->header, ie_req->ver_type, DOWNLOAD_ACK_ACCEPT);
    if (ret != SUCCESS) {
        LOG_ERROR("Failed to send version download ack");
        pthread_mutex_lock(&g_download_mutex);
        g_download_task.in_progress = false;
        pthread_mutex_unlock(&g_download_mutex);
        return ret;
    }

    /* 创建下载线程 */
    ret = pthread_create(&g_download_thread, NULL, version_download_thread_func, &g_download_task);
    if (ret != 0) {
        LOG_ERROR("Failed to create version download thread: %d", ret);
        pthread_mutex_lock(&g_download_mutex);
        g_download_task.in_progress = false;
        pthread_mutex_unlock(&g_download_mutex);

        send_version_download_result(&msg->header, ie_req->ver_type, DOWNLOAD_RESULT_OTHER);
        return ERROR_GENERAL;
    }

    pthread_detach(g_download_thread);
    LOG_INFO("Version download task started");
    return SUCCESS;
}

int version_handle_activate_request(const cpri_message_t *msg)
{
    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid version activate request message");
        return ERROR_INVALID_PARAM;
    }

    /* 解析IE 311 */
    ie_version_activate_ind_t *ie_ind = (ie_version_activate_ind_t *)msg->payload;

    LOG_INFO("Received version activate request: version=%s, type=%u",
            ie_ind->version_num, ie_ind->ver_type);

    /* 处理版本号：去掉.tar.gz后缀（如果有） */
    char version[MAX_VERSION_LEN];
    strncpy(version, ie_ind->version_num, sizeof(version) - 1);
    version[sizeof(version) - 1] = '\0';

    /* 去掉.tar.gz后缀 */
    char *tar_gz_pos = strstr(version, ".tar.gz");
    if (tar_gz_pos) {
        *tar_gz_pos = '\0';
        LOG_INFO("Removed .tar.gz suffix, actual version: %s", version);
    }

    uint8_t result = ACTIVATE_RESULT_SUCCESS;

    /* 根据版本类型执行不同的激活逻辑 */
    if (ie_ind->ver_type == VERSION_TYPE_FIRMWARE) {
        /* 固件版本：只验证版本是否存在，不修改软链接 */
        LOG_INFO("Activating firmware version: %s (will not change current symlink)", version);

        char version_dir[512];
        /* 使用大小写不敏感查找 */
        if (find_version_directory(version, version_dir, sizeof(version_dir)) == NULL) {
            LOG_ERROR("Firmware version directory not found: %s", version);
            result = ACTIVATE_RESULT_NOT_EXIST;
        } else {
            /* 从实际目录路径中提取版本号 */
            char *actual_version = strrchr(version_dir, '/');
            if (actual_version) {
                actual_version++;  /* 跳过 '/' */
            } else {
                actual_version = version;
            }

            /* 验证版本元数据 */
            version_metadata_t metadata;
            if (load_version_metadata(actual_version, &metadata) != SUCCESS) {
                LOG_ERROR("Failed to load firmware version metadata");
                result = ACTIVATE_RESULT_CORRUPTED;
            } else {
                LOG_INFO("Firmware version validated: %s (actual: %s)", version, actual_version);
                /* 更新 version 为实际版本号，用于后续的固件上注 */
                strncpy(version, actual_version, sizeof(version) - 1);
                result = ACTIVATE_RESULT_SUCCESS;
            }
        }
    } else {
        /* 软件版本：执行版本切换（修改软链接） */
        LOG_INFO("Activating software version: %s (will update current symlink)", version);
        result = switch_to_version(version, "BBU activate request");
    }

    /* 发送激活应答 */
    send_version_activate_ack(&msg->header, ie_ind->ver_type, result);

    if (result == ACTIVATE_RESULT_SUCCESS) {
        LOG_INFO("Version activated successfully");

        /* 如果是固件类型,触发FPGA固件上注 */
        if (ie_ind->ver_type == VERSION_TYPE_FIRMWARE) {
            LOG_INFO("触发FPGA固件上注: %s", version);

            char version_dir[512];
            snprintf(version_dir, sizeof(version_dir), "%s/%s",
                    VERSION_BASE_DIR, version);

            /* 启动后台上注任务 */
            int ret = fpga_firmware_injection_start(version_dir, version);
            if (ret != SUCCESS) {
                LOG_ERROR("启动FPGA固件上注失败");
                /* 已发送激活成功应答,记录错误但继续 */
            }
        } else {
            /* 软件版本激活 */
            LOG_INFO("Software version activated: %s", version);
            LOG_INFO("System will restart to apply new software version...");

            /* 创建重启标记文件，供启动脚本检测 */
            FILE *fp = fopen("/tmp/antenna_mgmt_restart_pending", "w");
            if (fp) {
                fprintf(fp, "version=%s\n", ie_ind->version_num);
                fprintf(fp, "timestamp=%ld\n", time(NULL));
                fclose(fp);
            }

            /* 延迟3秒后重启服务（给BBU时间接收应答） */
            LOG_INFO("Service will restart in 3 seconds...");
            sleep(3);

            /* 方式1: 通过systemd重启服务（推荐） */
            system("systemctl restart antenna-mgmt.service &");

            /* 方式2: 如果需要重启整个系统（谨慎使用） */
            /* system("shutdown -r +1 'Antenna management software upgrade' &"); */
        }
    }

    return SUCCESS;
}

int version_get_current(char *version, size_t len)
{
    if (!version || len == 0) {
        return ERROR_INVALID_PARAM;
    }

    strncpy(version, g_current_version, len - 1);
    version[len - 1] = '\0';
    return SUCCESS;
}

int version_list_installed(char versions[][MAX_VERSION_LEN], int max_count)
{
    DIR *dir = opendir(VERSION_BASE_DIR);
    if (!dir) {
        LOG_ERROR("Failed to open version directory");
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_count) {
        if (entry->d_name[0] == '.') {
            continue;  /* 跳过隐藏目录 */
        }

        char version_path[512];
        snprintf(version_path, sizeof(version_path), "%s/%s",
                VERSION_BASE_DIR, entry->d_name);

        struct stat st;
        if (stat(version_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(versions[count], entry->d_name, MAX_VERSION_LEN - 1);
            versions[count][MAX_VERSION_LEN - 1] = '\0';
            count++;
        }
    }

    closedir(dir);
    return count;
}

int version_rollback(const char *version, const char *reason)
{
    if (!version) {
        LOG_ERROR("Invalid version parameter");
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("Rolling back to version: %s, reason: %s", version, reason ? reason : "manual");

    uint8_t result = switch_to_version(version, reason);
    if (result == ACTIVATE_RESULT_SUCCESS) {
        LOG_INFO("Rollback successful");
        return SUCCESS;
    } else {
        LOG_ERROR("Rollback failed with result: %u", result);
        return ERROR_GENERAL;
    }
}

void version_manager_destroy(void)
{
    pthread_mutex_lock(&g_download_mutex);
    if (g_download_task.in_progress) {
        LOG_WARN("Version download in progress during destroy");
    }
    pthread_mutex_unlock(&g_download_mutex);

    LOG_INFO("Version manager destroyed");
}



