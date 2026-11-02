#include "version_manager.h"
#include "logger.h"
#include "tcp_client.h"
#include "config.h"
#include "fpga_firmware_injector.h"
#include "firmware_package.h"
#include "alarm_manager.h"
#include "shell_util.h"
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

/* 进程退出标志。与 log_upload 的 g_upload_stopping 同一用途：下载线程是 detached
 * 的、且 curl 的 --max-time 是 600 秒，destroy 里 join 会把退出流程卡住十分钟，
 * 所以只能靠这个标志让线程在发 TCP 报文前收手 —— 否则它可能在 tcp_client 拆卸、
 * send_mutex 被销毁之后仍去锁它（见 version_manager_destroy）。 */
static volatile bool g_shutting_down = false;

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

/* 成员数上限。正常版本包只有个位数成员；设上限顺便保证 tar -t 列表不会
 * 撑爆下面的 64KB 缓冲（截断会被判为异常而拒收）。 */
#define TAR_MAX_MEMBERS  4096

static int verify_tar_members(const char *tar_path);
static int check_tar_member_types(const char *tar_path);

/* 解压前检查压缩包的全部成员名。
 *
 * 这是"解压前置校验"的落点：verify_bin_file_checksum 的期望值是从**解压出来的**
 * 目录里读的（见 read_checksum_from_txt），比对的是"包里的文件"与"包里自带的期望值"，
 * 只能发现传输损坏，且必须在解压之后才能做 —— 指望它拦住恶意压缩包是错的。
 * 压缩包的内容在解压前没有可信参照，但**成员名**可以先看：成员名里的 '..' 是唯一
 * 能在写入发生前判定的穿越特征。
 * 用 -t 只列目录、不解压，读到的是一行一个成员名（tar 默认已加 --quoting-style=escape
 * 转义特殊字符，这里再拒一次控制字符以防实现差异）。
 *
 * 边界（实测 GNU tar 的行为，勿误判防护范围）：
 *   - 含 '..' 的成员，GNU tar 自己就会拒绝并整体失败（1.29 与 1.35 均如此，
 *     不写任何文件）；开头的 '/' 和 '../' 也会被 tar 剥掉。所以这一层在 GNU tar 上
 *     主要是冗余兜底，真正起作用的是换了 tar 实现（busybox/BSD）或旧版本时。
 *   - 【本函数的已知盲区】`tar -t` 只打印成员**名字**，链接成员的目标（linkname）
 *     根本不出现。于是这样的成员名全部合法、能穿过本函数：
 *         lib            （实为 lib -> /etc 的符号链接）
 *         data           （实为硬链接 etc/passwd）
 *     而 GNU tar 确实会把这个指向外部的符号链接创建出来（实测 exit=0）。
 *     真正挡住"后续成员经该链接向外写"的是 tar 默认不跟随目录符号链接，
 *     不是本函数；本函数对链接成员不提供任何保证。
 *   - 链接成员盲区由紧接的第二遍扫描（check_tar_member_types）负责关掉：
 *     它只放行普通文件与目录。 */
static int verify_tar_members(const char *tar_path)
{
    /* 成员列表缓冲。容量要能装下正常版本包的全部成员名；超过即视为异常
     * （见下方截断判断），不做静默截断处理 —— 否则被截断的那个成员名
     * 恰好可能藏住 '..'。 */
    const size_t list_cap = 64 * 1024;
    char *argv[] = { (char *)"tar", (char *)"-tzf", (char *)tar_path, NULL };
    char *out = (char *)malloc(list_cap);
    if (!out) {
        LOG_ERROR("Failed to alloc buffer for tar member list");
        return ERROR_GENERAL;
    }

    int status = shell_run(argv, out, list_cap);
    if (status < 0) {
        LOG_ERROR("Failed to list tar members: %s", tar_path);
        free(out);
        return ERROR_GENERAL;
    }
    if (status != 0) {
        LOG_ERROR("tar -t failed (status=%d) for %s", status, tar_path);
        free(out);
        return ERROR_GENERAL;
    }
    /* shell_run 在缓冲满时继续排空管道但丢弃多余输出，不告知是否截断。
     * 缓冲被填满说明列表可能不完整，无法保证每个成员名都校验过 → 拒绝。 */
    if (strlen(out) >= list_cap - 1) {
        LOG_ERROR("tar member listing too large (>= %zu bytes), rejecting: %s",
                  list_cap, tar_path);
        free(out);
        return ERROR_GENERAL;
    }

    int ret = SUCCESS;
    uint32_t scanned = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(out, "\n", &saveptr); line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        scanned++;
        /* 条目数上限：正常版本包只有个位数成员（可执行文件 + metadata.txt
         * + 少量依赖库）。设上限是为了保证成员列表一定装得进上面的缓冲 ——
         * 否则一个条目极多的大包会被 64KB 截断判断拒掉，那本是合法包。 */
        if (scanned > TAR_MAX_MEMBERS) {
            LOG_ERROR("Archive has more than %u members, rejecting: %s",
                      (unsigned)TAR_MAX_MEMBERS, tar_path);
            ret = ERROR_GENERAL;
            break;
        }
        if (!shell_safe_tar_member(line)) {
            /* 这里把成员名原样打进日志：它正是需要被审计下来的东西。
             * 但先按可见字符替换控制字符，避免它伪造日志行。 */
            for (char *c = line; *c; c++) {
                unsigned char u = (unsigned char)*c;
                if (u < 0x20 || u == 0x7f) {
                    *c = '?';
                }
            }
            LOG_ERROR("SECURITY ALERT: unsafe tar member rejected: '%s' (archive=%s)",
                      line, tar_path);
            ret = ERROR_GENERAL;
            break;
        }
    }
    free(out);

    /* 一个成员都没有说明这不是个有效压缩包（tar -t 会返回 0） */
    if (ret == SUCCESS && scanned == 0) {
        LOG_ERROR("Archive contains no members, rejecting: %s", tar_path);
        ret = ERROR_GENERAL;
    }

    if (ret == SUCCESS) {
        LOG_INFO("Tar member check passed (%u entries): %s", scanned, tar_path);
        ret = check_tar_member_types(tar_path);
    }
    return ret;
}

/* 第二遍扫描：只放行"普通文件"与"目录"，其余成员类型一律拒绝。
 *
 * 这一层专治上面 verify_tar_members 的盲区。`tar -t` 只打印成员**名字**，
 * 链接成员的目标不出现，于是 lib -> /etc 这类符号链接可以用完全正常的名字
 * 穿过名字校验，而 GNU tar 确实会把它创建出来（实测 exit=0）。
 * 正式版本包只有"可执行文件 + metadata.txt + 少量依赖库 + 目录"（见
 * docs/VERSION_MANAGEMENT.md），不含任何链接，所以这里只放行文件与目录是安全的。
 *
 * 实现上刻意**不解析**成员名与链接目标：tar -tv 的 -rw-r--r-- ... 段里名字之后
 * 还有 " -> 目标" / " link to 目标" 等后缀，去解析它容易引入新的解析漏洞。
 * 这里只看每行**第一个字符**给出的成员类型（tar 的类型字符取自
 * "-bcdhlps" 这组，其中 'l' 符号链接、'h' 硬链接恰好都被 tar 用 !isalpha 挡掉、
 * 不可能出现在名字首字符 —— 所以按行首判类型不会与名字混淆）：
 *   '-' 普通文件、'd' 目录 = 放行；
 *   其余（'l' 符号链接、'h' 硬链接、'b'/'c' 设备、'p' 管道、's'/'?' 等）= 拒绝。 */
static int check_tar_member_types(const char *tar_path)
{
    const size_t list_cap = 64 * 1024;
    char *argv[] = { (char *)"tar", (char *)"-tvzf", (char *)tar_path, NULL };
    char *out = (char *)malloc(list_cap);
    if (!out) {
        LOG_ERROR("Failed to alloc buffer for tar type list");
        return ERROR_GENERAL;
    }

    int status = shell_run(argv, out, list_cap);
    if (status < 0 || status != 0) {
        LOG_ERROR("tar -tv failed (status=%d) for %s", status, tar_path);
        free(out);
        return ERROR_GENERAL;
    }
    if (strlen(out) >= list_cap - 1) {
        LOG_ERROR("tar -tv output too large (>= %zu bytes), rejecting: %s",
                  list_cap, tar_path);
        free(out);
        return ERROR_GENERAL;
    }

    int ret = SUCCESS;
    uint32_t scanned = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(out, "\n", &saveptr); line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        /* tar 会往输出里混入告警（如 "Removing leading `/'..."），它们以 "tar:"
         * 开头。用"第 2 列是否为权限位 rwx-"来识别真正的条目行：权限串的第 2 列
         * 必然是属主读位（'r' 或 '-'），而告警行的第 2 列是 'a'。 */
        if (line[1] != 'r' && line[1] != 'w' && line[1] != 'x' && line[1] != '-') {
            continue;
        }

        scanned++;
        /* 只放行普通文件 '-' 与目录 'd'。其余一律拒绝 —— 包括 'l'/'h' 链接、
         * 'b'/'c' 设备、'p' 管道，以及任何未预期的类型字符（fail-closed）。 */
        if (line[0] != '-' && line[0] != 'd') {
            const char *tname = "unknown/unsupported";
            switch (line[0]) {
                case 'l': tname = "symbolic link"; break;
                case 'h': tname = "hard link"; break;
                case 'b': tname = "block device"; break;
                case 'c': tname = "char device"; break;
                case 'p': tname = "fifo"; break;
                case 's': tname = "socket"; break;
            }
            LOG_ERROR("SECURITY ALERT: rejected tar member of type '%c' (%s) "
                      "in %s - only regular files and directories are allowed",
                      line[0], tname, tar_path);
            ret = ERROR_GENERAL;
            break;
        }
    }
    free(out);

    if (ret == SUCCESS && scanned == 0) {
        LOG_ERROR("tar -tv produced no parseable entries, rejecting: %s", tar_path);
        ret = ERROR_GENERAL;
    }
    if (ret == SUCCESS) {
        LOG_INFO("Tar member type check passed (%u entries, files/dirs only): %s",
                 scanned, tar_path);
    }
    return ret;
}

/* 解压tar文件 */
static int extract_tar_file(const char *tar_path, const char *dest_dir)
{
    /* 先做成员名校验，任何一条不通过就不解压 */
    if (verify_tar_members(tar_path) != SUCCESS) {
        return ERROR_GENERAL;
    }

    /* 用 argv 数组传参，不经过 shell：
     * tar_path / dest_dir 均由北向报文字段拼成，走 shell 会被 $() 与反引号注入。
     * --no-same-owner：不让包里的 uid/gid 覆盖解压文件的属主（本服务以 root 运行，
     * 否则包内成员可声明属主，影响后续以 root 身份执行的版本切换与上注）。 */
    char *argv[] = { (char *)"tar", (char *)"-xzf", (char *)tar_path,
                     (char *)"-C", (char *)dest_dir,
                     (char *)"--no-same-owner", NULL };

    LOG_INFO("Extracting tar file: %s -> %s", tar_path, dest_dir);

    char output[1024] = {0};
    int status = shell_run(argv, output, sizeof(output));
    if (status < 0) {
        LOG_ERROR("Failed to execute tar command");
        return ERROR_GENERAL;
    }

    if (strlen(output) > 0) {
        LOG_WARN("tar output: %s", output);
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


/* 保存版本元数据（写到 dir/version.json；软件与固件的目录不同，由调用方给出） */
static int save_version_metadata(const char *dir, const version_metadata_t *metadata)
{
    char meta_path[512 + 32];   /* dir 最长 512，再拼文件名 */
    snprintf(meta_path, sizeof(meta_path), "%s/version.json", dir);

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

/* 加载版本元数据（读 dir/metadata.txt，没有则读 dir/version.json）。
 * 目录名的最后一段作为缺省版本号。 */
static int load_version_metadata(const char *dir, version_metadata_t *metadata)
{
    char meta_path[512 + 32];   /* dir 最长 512，再拼文件名 */
    const char *version = strrchr(dir, '/');
    version = version ? version + 1 : dir;

    /* 优先尝试读取metadata.txt（新格式） */
    snprintf(meta_path, sizeof(meta_path), "%s/metadata.txt", dir);

    FILE *fp = fopen(meta_path, "r");
    if (!fp) {
        /* 如果metadata.txt不存在，尝试version.json（旧格式） */
        snprintf(meta_path, sizeof(meta_path), "%s/version.json", dir);
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

/* 删除目录树（argv 传参，不经过 shell；路径中可能含北向/包内可控的版本号） */
static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;     /* 不存在，无需删除 */
    }
    char *rm_argv[] = { (char *)"rm", (char *)"-rf", (char *)path, NULL };
    char rm_out[256] = {0};
    int rm_ret = shell_run(rm_argv, rm_out, sizeof(rm_out));
    if (rm_ret != 0) {
        LOG_WARN("Failed to remove %s (ret=%d, out=%s)", path, rm_ret, rm_out);
    }
}

/* 在 FIRMWARE_BASE_DIR/<种类>/ 下按版本号查找固件目录（先精确、后大小写不敏感）。
 *
 * IE 311（激活指示）只带版本号、不带 FPGA 种类，所以要在三个种类目录里都找：
 *   - 只命中一个：就是它；
 *   - 命中多个（不同 FPGA 的固件用了同一个版本号）：取最近入库的那个（目录
 *     mtime 最新），即 BBU 刚下载的那个，并打 WARN 列出全部候选。
 *     建议不同 FPGA 的固件版本号不要重复，从根本上消除歧义。
 *
 * 注意：刻意不回退到 VERSION_BASE_DIR（旧布局/软件目录）。软件包的 metadata.txt
 * 同样带 file_sub_type（例如 0x01），若回退去那里找，同名的软件版本可能被当成
 * V7A 固件写进 FPGA。旧布局下的固件需要重新下载一次。
 *
 * 成功返回 SUCCESS，并输出目录路径与 FPGA 种类。 */
static int find_firmware_directory(const char *version, char *out_dir, size_t out_size,
                                   fpga_kind_t *out_kind)
{
    char best_dir[512] = {0};
    fpga_kind_t best_kind = FPGA_KIND_UNKNOWN;
    time_t best_mtime = 0;
    int matches = 0;

    for (int k = FPGA_KIND_UNKNOWN + 1; k < FPGA_KIND_COUNT; k++) {
        const char *kind_dir = firmware_fpga_kind_dir((fpga_kind_t)k);
        char base[256];
        snprintf(base, sizeof(base), "%s/%s", FIRMWARE_BASE_DIR, kind_dir);

        char found[512] = {0};
        struct stat st;

        /* 精确匹配 */
        char candidate[512];
        snprintf(candidate, sizeof(candidate), "%s/%s", base, version);
        if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(found, sizeof(found), "%s", candidate);
        } else {
            /* 大小写不敏感匹配（兼容 BBU 下发的版本号大小写与包内不一致） */
            DIR *dir = opendir(base);
            if (dir) {
                struct dirent *entry;
                while ((entry = readdir(dir)) != NULL) {
                    if (entry->d_name[0] == '.') {
                        continue;
                    }
                    if (strcasecmp(entry->d_name, version) == 0) {
                        snprintf(candidate, sizeof(candidate), "%s/%s", base, entry->d_name);
                        if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                            snprintf(found, sizeof(found), "%s", candidate);
                            break;
                        }
                    }
                }
                closedir(dir);
            }
        }

        if (found[0] == '\0') {
            continue;
        }

        matches++;
        LOG_INFO("Firmware candidate for version %s: %s (%s)",
                 version, found, firmware_fpga_kind_name((fpga_kind_t)k));
        if (matches == 1 || st.st_mtime > best_mtime) {
            snprintf(best_dir, sizeof(best_dir), "%s", found);
            best_kind = (fpga_kind_t)k;
            best_mtime = st.st_mtime;
        }
    }

    if (matches == 0) {
        return ERROR_GENERAL;
    }
    if (matches > 1) {
        LOG_WARN("Firmware version %s exists for %d FPGA kinds, picking the most recently "
                 "installed one: %s (%s). Use distinct version numbers per FPGA to avoid this.",
                 version, matches, best_dir, firmware_fpga_kind_name(best_kind));
    }

    snprintf(out_dir, out_size, "%s", best_dir);
    if (out_kind) {
        *out_kind = best_kind;
    }
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

        /* argv 数组传参，不经过 shell：
         * full_url / local_file 都含北向可控字段（file_path / file_name）。 */
        char *curl_argv[] = { (char *)"curl", (char *)"-s", (char *)"-S",
                              (char *)"--connect-timeout", (char *)"30",
                              (char *)"--max-time", (char *)"600",
                              (char *)"-o", (char *)local_file, full_url, NULL };

        LOG_INFO("Executing FTP download (attempt %d): %s -> %s",
                 attempt + 1, full_url, local_file);

        char output[1024] = {0};
        int status = shell_run(curl_argv, output, sizeof(output));
        if (status < 0) {
            LOG_ERROR("Failed to execute curl command");
            continue;
        }

        if (strlen(output) > 0) {
            LOG_WARN("curl output: %s", output);
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

    const bool is_firmware = (task->ver_type == VERSION_TYPE_FIRMWARE);

    /* 创建下载目录：软件与固件的压缩包分开暂存，同名文件互不覆盖 */
    const char *download_dir = is_firmware ? VERSION_DOWNLOAD_FW_DIR : VERSION_DOWNLOAD_SW_DIR;
    if (create_directory_recursive(download_dir) != SUCCESS) {
        LOG_ERROR("Failed to create download directory: %s", download_dir);
        result = DOWNLOAD_RESULT_OTHER;
        goto send_result;
    }

    /* 下载文件路径 */
    snprintf(download_path, sizeof(download_path), "%s/%s",
            download_dir, task->file_name);

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

    version_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));

    if (!is_firmware) {
        /* ================= 软件版本：VERSION_BASE_DIR/<版本>（布局不变） ================= */
        snprintf(version_dir, sizeof(version_dir), "%s/%s",
                VERSION_BASE_DIR, task->version);
        if (create_directory_recursive(version_dir) != SUCCESS) {
            LOG_ERROR("Failed to create version directory: %s", version_dir);
            result = DOWNLOAD_RESULT_OTHER;
            goto send_result;
        }

        LOG_INFO("Extracting software package to: %s", version_dir);
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
            remove_tree(version_dir);     /* 删除已解压的文件（安全措施） */
            LOG_WARN("Removed corrupted version directory: %s", version_dir);
            goto send_result;
        }

        /* 软件版本：使用文件名或 file_ver 作为版本号，不从 metadata.txt 读取 */
        LOG_INFO("Software version: using version from filename: %s", task->version);
        strncpy(metadata.version, task->version, sizeof(metadata.version) - 1);
    } else {
        /* ======== 固件版本：FIRMWARE_BASE_DIR/<v7a|v7b|subarray>/<版本> ========
         * FPGA 种类要解开包、读 metadata.txt 的 file_sub_type 才知道，所以先解压到
         * 暂存区，校验通过并判定种类后，再整体 rename 到对应种类的目录。
         * 暂存区与目标目录都在 FIRMWARE_BASE_DIR 下，rename 不跨文件系统。 */
        char staging_dir[512];
        snprintf(staging_dir, sizeof(staging_dir), "%s/%s",
                 FIRMWARE_STAGING_DIR, task->version);
        remove_tree(staging_dir);     /* 清掉上次异常中断留下的残留 */
        if (create_directory_recursive(staging_dir) != SUCCESS) {
            LOG_ERROR("Failed to create firmware staging directory: %s", staging_dir);
            result = DOWNLOAD_RESULT_OTHER;
            goto send_result;
        }

        LOG_INFO("Extracting firmware package to staging: %s", staging_dir);
        if (extract_tar_file(download_path, staging_dir) != SUCCESS) {
            LOG_ERROR("Failed to extract tar file");
            remove_tree(staging_dir);
            result = DOWNLOAD_RESULT_EXTRACT;
            goto send_result;
        }

        /* 【关键】验证文件的校验和 */
        LOG_INFO("Verifying file checksum (ver_type=%u)...", task->ver_type);
        result = verify_bin_file_checksum(staging_dir);
        if (result != DOWNLOAD_RESULT_SUCCESS) {
            LOG_ERROR("File checksum verification FAILED");
            remove_tree(staging_dir);
            LOG_WARN("Removed corrupted firmware staging directory: %s", staging_dir);
            goto send_result;
        }

        /* 读 metadata.txt：file_sub_type 决定 FPGA 种类，version 是真实版本号。
         * 与上注时用同一个解析函数，这里解析不过的包，激活后也一定上注失败，
         * 所以在入库前就拒掉。 */
        firmware_metadata_t fw_meta;
        if (firmware_parse_metadata(staging_dir, &fw_meta) != SUCCESS) {
            LOG_ERROR("Invalid firmware package: failed to parse metadata.txt");
            remove_tree(staging_dir);
            result = DOWNLOAD_RESULT_EXTRACT;
            goto send_result;
        }

        fpga_kind_t kind = firmware_fpga_kind(fw_meta.file_sub_type);
        const char *kind_dir = firmware_fpga_kind_dir(kind);
        if (kind_dir == NULL) {
            LOG_ERROR("Unknown FPGA firmware sub type 0x%02X (expected 0x01/0x02=V7A, "
                      "0x13/0x14=V7B, 0x11/0x12=subarray), package rejected",
                      fw_meta.file_sub_type);
            remove_tree(staging_dir);
            result = DOWNLOAD_RESULT_OTHER;
            goto send_result;
        }

        /* 【关键】fw_meta.version 来自压缩包内的 metadata.txt，随后被拼进目标路径并
         * 交给 rm -rf / rename —— 二阶注入面（压缩包来自北向指定的下载路径）。
         * 与入站处同样过 shell_safe_name，不通过就沿用受校验的临时版本号。 */
        char real_version[MAX_VERSION_LEN];
        if (fw_meta.version[0] != '\0' && shell_safe_name(fw_meta.version)) {
            strncpy(real_version, fw_meta.version, sizeof(real_version) - 1);
        } else {
            if (fw_meta.version[0] != '\0') {
                LOG_ERROR("SECURITY ALERT: unsafe version in package metadata, "
                          "rejected: '%s' (keeping temp version '%s')",
                          fw_meta.version, task->version);
            }
            strncpy(real_version, task->version, sizeof(real_version) - 1);
        }
        real_version[sizeof(real_version) - 1] = '\0';

        char kind_base[256];
        snprintf(kind_base, sizeof(kind_base), "%s/%s", FIRMWARE_BASE_DIR, kind_dir);
        if (create_directory_recursive(kind_base) != SUCCESS) {
            LOG_ERROR("Failed to create firmware directory: %s", kind_base);
            remove_tree(staging_dir);
            result = DOWNLOAD_RESULT_OTHER;
            goto send_result;
        }

        snprintf(version_dir, sizeof(version_dir), "%s/%s", kind_base, real_version);

        /* 只会替换【同一种 FPGA】的同版本旧包；其他 FPGA 与软件版本不受影响 */
        struct stat st_check;
        if (stat(version_dir, &st_check) == 0) {
            LOG_WARN("%s firmware %s already exists, replacing it",
                     firmware_fpga_kind_name(kind), real_version);
            remove_tree(version_dir);
        }

        if (rename(staging_dir, version_dir) != 0) {
            LOG_ERROR("Failed to move firmware from %s to %s: %s",
                      staging_dir, version_dir, strerror(errno));
            remove_tree(staging_dir);
            result = DOWNLOAD_RESULT_OTHER;
            goto send_result;
        }

        LOG_INFO("Firmware stored: FPGA=%s (sub_type=0x%02X), version=%s, dir=%s",
                 firmware_fpga_kind_name(kind), fw_meta.file_sub_type,
                 real_version, version_dir);

        strncpy(task->version, real_version, sizeof(task->version) - 1);
        task->version[sizeof(task->version) - 1] = '\0';
        strncpy(metadata.version, real_version, sizeof(metadata.version) - 1);
    }

    /* 保存版本元数据 */
    strncpy(metadata.build_time, task->file_time, sizeof(metadata.build_time) - 1);
    strncpy(metadata.checksum, checksum, sizeof(metadata.checksum) - 1);
    metadata.file_size = st.st_size;
    metadata.ver_type = task->ver_type;
    snprintf(metadata.install_path, sizeof(metadata.install_path), "%s", version_dir);

    if (save_version_metadata(version_dir, &metadata) != SUCCESS) {
        LOG_ERROR("Failed to save version metadata");
        result = DOWNLOAD_RESULT_OTHER;
        goto send_result;
    }

    /* 删除下载的tar文件 */
    unlink(download_path);

    LOG_INFO("Version %s downloaded and installed successfully", task->version);

send_result:
    /* 发送下载结果指示。进程正在退出时不再发：此刻 tcp_client 可能已销毁，
     * 发出去也只会锁到已销毁的 send_mutex。结果指示发不出去无关紧要 —— 进程都要退了。 */
    if (!g_shutting_down) {
        send_version_download_result(&task->req_header, task->ver_type, result);
    } else {
        LOG_WARN("Shutting down, skip version download result indication (result=%u)", result);
    }

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

/* ==================== 试运行记录读写 ====================
 * 记录文件 VERSION_TRIAL_FILE，key=value 文本，同时被 scripts/upgrade-guard.sh 读写：
 *   new=<新版本目录>        与 readlink(VERSION_CURRENT_LINK) 的原始结果逐字比较
 *   previous=<上一个版本目录>
 *   attempts=<已启动次数>   由 upgrade-guard.sh 每次启动前 +1，程序稳定运行后清零
 *   time=<写入时间>
 * 读写都在 g_trial_mutex 下进行（通道建立回调与主循环两个线程会调用）。 */
typedef struct {
    char new_dir[512];
    char prev_dir[512];
    uint32_t attempts;
    char time[32];
} version_trial_t;

static pthread_mutex_t g_trial_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_trial_confirmed = false;      /* 本进程已确认过，不再重复处理 */
static bool g_trial_stable_done = false;    /* 本进程已做过稳定运行清零 */
static uint32_t g_trial_stable_sec = 120;   /* UPGRADE_TRIAL_STABLE_SEC */

/* 读取 current 软链接的原始指向（不做 realpath，与 shell 侧 readlink 一致） */
static int read_current_link(char *buf, size_t size)
{
    ssize_t len = readlink(VERSION_CURRENT_LINK, buf, size - 1);
    if (len <= 0) {
        buf[0] = '\0';
        return ERROR_GENERAL;
    }
    buf[len] = '\0';
    return SUCCESS;
}

static int trial_read(version_trial_t *t)
{
    memset(t, 0, sizeof(*t));
    FILE *fp = fopen(VERSION_TRIAL_FILE, "r");
    if (!fp) {
        return ERROR_GENERAL;
    }
    char line[640];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "new") == 0) {
            snprintf(t->new_dir, sizeof(t->new_dir), "%s", val);
        } else if (strcmp(key, "previous") == 0) {
            snprintf(t->prev_dir, sizeof(t->prev_dir), "%s", val);
        } else if (strcmp(key, "attempts") == 0) {
            t->attempts = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "time") == 0) {
            snprintf(t->time, sizeof(t->time), "%s", val);
        }
    }
    fclose(fp);
    return (t->new_dir[0] != '\0') ? SUCCESS : ERROR_GENERAL;
}

/* 先写临时文件、fsync，再 rename 覆盖：任何时刻断电，记录要么是旧的要么是新的，
 * 不会是写了一半的。 */
static int trial_write(const version_trial_t *t)
{
    char tmp[sizeof(VERSION_TRIAL_FILE) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", VERSION_TRIAL_FILE);
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        LOG_ERROR("Failed to write trial record %s: %s", tmp, strerror(errno));
        return ERROR_GENERAL;
    }
    fprintf(fp, "new=%s\nprevious=%s\nattempts=%u\ntime=%s\n",
            t->new_dir, t->prev_dir, t->attempts, t->time);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    if (rename(tmp, VERSION_TRIAL_FILE) != 0) {
        LOG_ERROR("Failed to commit trial record: %s", strerror(errno));
        unlink(tmp);
        return ERROR_GENERAL;
    }
    return SUCCESS;
}

static void trial_remove(void)
{
    if (unlink(VERSION_TRIAL_FILE) == 0) {
        sync();
    }
}

/* 原子地把 current 指向 target：先建临时软链接，再 rename 覆盖。
 * 原实现是 unlink 后 symlink，两步之间断电会让 current 消失，服务再也起不来。 */
static int replace_current_link(const char *target)
{
    char tmp_link[sizeof(VERSION_CURRENT_LINK) + 8];
    snprintf(tmp_link, sizeof(tmp_link), "%s.tmp", VERSION_CURRENT_LINK);
    unlink(tmp_link);
    if (symlink(target, tmp_link) != 0) {
        LOG_ERROR("Failed to create symlink %s -> %s: %s", tmp_link, target, strerror(errno));
        return ERROR_GENERAL;
    }
    if (rename(tmp_link, VERSION_CURRENT_LINK) != 0) {
        LOG_ERROR("Failed to replace %s: %s", VERSION_CURRENT_LINK, strerror(errno));
        unlink(tmp_link);
        return ERROR_GENERAL;
    }
    sync();
    return SUCCESS;
}

/* 切换到指定版本（原子操作）
 * trial=true：BBU 激活新版本，进入试运行（失败可被 upgrade-guard.sh 自动回退）；
 * trial=false：人工回退等，直接切换，并结束任何进行中的试运行。 */
static int switch_to_version(const char *version, const char *reason, bool trial)
{
    char version_dir[512];
    char rollback_dir[512];
    char timestamp[32];
    time_t now = time(NULL);
    struct tm tm_storage;
    struct tm *tm_info = localtime_r(&now, &tm_storage);
    if (!tm_info) {
        LOG_ERROR("localtime_r failed, cannot build version timestamp");
        return ACTIVATE_RESULT_OTHER;
    }

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
    if (load_version_metadata(version_dir, &metadata) != SUCCESS) {
        LOG_ERROR("Failed to load version metadata");
        return ACTIVATE_RESULT_CORRUPTED;
    }

    /* 记录当前版本（回退的目标） */
    char current_target[512] = {0};
    read_current_link(current_target, sizeof(current_target));

    if (current_target[0] != '\0') {
        snprintf(rollback_dir, sizeof(rollback_dir), "%s/%s",
                VERSION_ROLLBACK_DIR, timestamp);
        if (create_directory_recursive(rollback_dir) != SUCCESS) {
            LOG_ERROR("Failed to create rollback directory");
            return ACTIVATE_RESULT_OTHER;
        }

        /* 保存切换记录（审计用） */
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

    /* 试运行记录必须【先于】切换软链接写入：
     *   - 若写完记录、切换前断电：记录的 new 与 current 不符，upgrade-guard.sh
     *     会当作过期记录删掉，系统仍跑旧版本；
     *   - 反过来先切换后写记录，两步之间断电，新版本就在没有保护的情况下运行。 */
    struct stat prev_st;
    bool same_version = (current_target[0] != '\0' && strcmp(current_target, version_dir) == 0);
    pthread_mutex_lock(&g_trial_mutex);
    if (trial && !same_version && current_target[0] != '\0' &&
        stat(current_target, &prev_st) == 0 && S_ISDIR(prev_st.st_mode)) {
        version_trial_t t;
        memset(&t, 0, sizeof(t));
        snprintf(t.new_dir, sizeof(t.new_dir), "%s", version_dir);
        snprintf(t.prev_dir, sizeof(t.prev_dir), "%s", current_target);
        t.attempts = 0;
        snprintf(t.time, sizeof(t.time), "%s", timestamp);
        if (trial_write(&t) == SUCCESS) {
            LOG_INFO("Upgrade trial armed: %s (fallback: %s)", version_dir, current_target);
        } else {
            LOG_WARN("Failed to arm upgrade trial, new version will run without auto rollback");
        }
    } else if (!same_version) {
        /* 人工回退、首次安装（无上一个版本可回）：不试运行，清掉残留记录。
         * 重复激活当前版本（same_version）则保持现状，不打断进行中的试运行。 */
        if (trial) {
            LOG_WARN("No previous version to fall back to, %s will run without auto rollback",
                     version_dir);
        }
        trial_remove();
    }
    pthread_mutex_unlock(&g_trial_mutex);

    /* 原子切换软链接 */
    if (replace_current_link(version_dir) != SUCCESS) {
        pthread_mutex_lock(&g_trial_mutex);
        trial_remove();     /* 没切成功，试运行记录作废 */
        pthread_mutex_unlock(&g_trial_mutex);
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
    create_directory_recursive(VERSION_DOWNLOAD_SW_DIR);
    create_directory_recursive(VERSION_DOWNLOAD_FW_DIR);

    /* 固件按 FPGA 种类分目录存放 */
    create_directory_recursive(FIRMWARE_STAGING_DIR);
    for (int k = FPGA_KIND_UNKNOWN + 1; k < FPGA_KIND_COUNT; k++) {
        char kind_base[256];
        snprintf(kind_base, sizeof(kind_base), "%s/%s",
                 FIRMWARE_BASE_DIR, firmware_fpga_kind_dir((fpga_kind_t)k));
        create_directory_recursive(kind_base);
    }

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

    /* ---- 试运行 / 自动回退状态 ---- */
    int stable_sec = config_get_int("UPGRADE_TRIAL_STABLE_SEC", 120);
    g_trial_stable_sec = (stable_sec > 0) ? (uint32_t)stable_sec : 120;

    version_trial_t t;
    if (trial_read(&t) == SUCCESS) {
        char cur[512];
        read_current_link(cur, sizeof(cur));
        if (strcmp(cur, t.new_dir) != 0) {
            /* 记录的不是当前运行的版本（如切换前断电、人工改过软链接），作废 */
            LOG_WARN("Stale upgrade trial record (trial=%s, current=%s), removing",
                     t.new_dir, cur);
            trial_remove();
        } else {
            LOG_WARN("Running new version in TRIAL mode: start %u, fallback=%s. "
                     "Will be confirmed when channel to BBU is established.",
                     t.attempts, t.prev_dir);
        }
    }

    /* 本次启动前 upgrade-guard.sh 做过自动回退：告警通知 BBU（TCP 连上后补报）。
     * 通知文件保留到与 BBU 通道建立后再删，保证告警有机会送达。 */
    FILE *nfp = fopen(VERSION_AUTO_ROLLBACK_NOTICE, "r");
    if (nfp) {
        char from[640] = {0}, to[640] = {0}, line[640];
        while (fgets(line, sizeof(line), nfp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strncmp(line, "from=", 5) == 0) {
                snprintf(from, sizeof(from), "%s", line + 5);
            } else if (strncmp(line, "to=", 3) == 0) {
                snprintf(to, sizeof(to), "%s", line + 3);
            }
        }
        fclose(nfp);
        const char *from_v = strrchr(from, '/');
        const char *to_v = strrchr(to, '/');
        char info[80];
        snprintf(info, sizeof(info), "auto rollback %s -> %s",
                 from_v ? from_v + 1 : from, to_v ? to_v + 1 : to);
        LOG_ERROR("Previous upgrade failed to start, system auto rolled back: %s -> %s", from, to);
        alarm_trigger_version_activate_fail(info);
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

    /* 长度门：payload 不足一个完整 IE 14 时直接丢弃。
     * payload 来自固定 8192 字节的内存池槽位，不校验长度就强转结构体，读到的
     * 是池里上一条报文的残留（旧的文件名/路径/版本号），可能据此发起一次
     * 并非本次请求的下载。与 msg_handler.c 中其他 IE 的长度门保持一致。
     * 注意必须放在"busy"分支之前：那里也会读 ie_req->ver_type。 */
    if (msg->payload_len < sizeof(ie_version_download_req_t)) {
        LOG_ERROR("Version download request payload too short: %u bytes (need %zu), dropped",
                  msg->payload_len, sizeof(ie_version_download_req_t));
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
    /* 清空任务结构 + 置闸门必须在【同一个持锁区间】内完成。
     * 原代码是「持锁置 in_progress=true → 解锁 → memset」：memset 把刚置上的标志
     * 抹回 0，闸门形同虚设，第二个下载请求能通过上面的检查，
     * 两个线程操作同一个全局任务与同一个版本目录（并发 tar/rm/rename）。 */
    memset(&g_download_task, 0, sizeof(g_download_task));
    g_download_task.in_progress = true;
    pthread_mutex_unlock(&g_download_mutex);

    /* 解析IE 14 */
    ie_version_download_req_t *ie_req = (ie_version_download_req_t *)msg->payload;

    /* IE 14 里的字符串都是定长字段，协议不保证以 NUL 结尾（例如 file_name[16]
     * 正好填满 16 字节）。直接对它们用 %s / strlen / strstr 会越过字段一路读到
     * 后续字段乃至结构体之外。先拷成本地的、保证结尾 NUL 的副本，后面只用副本。 */
    char req_file_ver[sizeof(ie_req->file_ver) + 1];
    char req_file_name[sizeof(ie_req->file_name) + 1];
    char req_file_path[sizeof(ie_req->file_path) + 1];
    char req_file_time[sizeof(ie_req->file_time) + 1];
    memcpy(req_file_ver,  ie_req->file_ver,  sizeof(ie_req->file_ver));
    memcpy(req_file_name, ie_req->file_name, sizeof(ie_req->file_name));
    memcpy(req_file_path, ie_req->file_path, sizeof(ie_req->file_path));
    memcpy(req_file_time, ie_req->file_time, sizeof(ie_req->file_time));
    req_file_ver[sizeof(ie_req->file_ver)]   = '\0';
    req_file_name[sizeof(ie_req->file_name)] = '\0';
    req_file_path[sizeof(ie_req->file_path)] = '\0';
    req_file_time[sizeof(ie_req->file_time)] = '\0';

    LOG_INFO("Received version download request: version=%s, file=%s",
            req_file_ver, req_file_name);

    /* 根据版本类型确定临时版本号 */
    if (ie_req->ver_type == VERSION_TYPE_SOFTWARE) {
        /* 软件版本：必须使用 BBU 提供的 file_ver 作为版本号 */
        if (req_file_ver[0] != '\0' && strlen(req_file_ver) > 0) {
            strncpy(g_download_task.version, req_file_ver, sizeof(g_download_task.version) - 1);
            LOG_INFO("Software version from BBU: %s", g_download_task.version);
        } else {
            /* 如果 BBU 没有提供版本号，从文件名提取（兼容处理） */
            const char *dot_pos = strstr(req_file_name, ".tar.gz");
            if (dot_pos) {
                size_t len = dot_pos - req_file_name;
                if (len < sizeof(g_download_task.version)) {
                    strncpy(g_download_task.version, req_file_name, len);
                    g_download_task.version[len] = '\0';
                } else {
                    strncpy(g_download_task.version, req_file_name, sizeof(g_download_task.version) - 1);
                }
            } else {
                strncpy(g_download_task.version, req_file_name, sizeof(g_download_task.version) - 1);
            }
            LOG_WARN("Software version not provided by BBU, extracted from filename: %s", g_download_task.version);
        }
    } else {
        /* 固件版本：使用临时版本号，真实版本号将从 metadata.txt 读取 */
        if (req_file_ver[0] != '\0' && strlen(req_file_ver) > 0) {
            strncpy(g_download_task.version, req_file_ver, sizeof(g_download_task.version) - 1);
            LOG_INFO("Firmware temporary version: %s", g_download_task.version);
        } else {
            /* 从文件名提取临时版本号 */
            const char *dot_pos = strstr(req_file_name, ".tar.gz");
            if (dot_pos) {
                size_t len = dot_pos - req_file_name;
                if (len < sizeof(g_download_task.version)) {
                    strncpy(g_download_task.version, req_file_name, len);
                    g_download_task.version[len] = '\0';
                } else {
                    strncpy(g_download_task.version, req_file_name, sizeof(g_download_task.version) - 1);
                }
            } else {
                strncpy(g_download_task.version, req_file_name, sizeof(g_download_task.version) - 1);
            }
            LOG_INFO("Firmware temporary version from filename: %s (will read real version from metadata.txt)",
                     g_download_task.version);
        }
    }

    strncpy(g_download_task.file_path, req_file_path, sizeof(g_download_task.file_path) - 1);
    strncpy(g_download_task.file_name, req_file_name, sizeof(g_download_task.file_name) - 1);
    strncpy(g_download_task.file_time, req_file_time, sizeof(g_download_task.file_time) - 1);
    g_download_task.file_len = ie_req->file_len;
    g_download_task.ver_type = ie_req->ver_type;

    /* 校验北向可控的字段。它们会被拼进 FTP URL、本地路径并交给 tar/rm：
     *   - version/file_name 含 '/' 或 ".." 可穿越出 /opt/vendor/versions/ 之外
     *     （如 rm -rf ../../..）；为空会让 version_dir 退化成 VERSION_BASE_DIR 本身，
     *     进而把整个版本目录删掉。
     *   - file_path 直接拼进 FTP URL，同样必须挡住 '..'（可让 curl 写到别处）
     *     与控制字符（换行可截断 URL/伪造日志行）。file_path 允许为空
     *     （表示 FTP 根目录），这也是原代码支持的用法。 */
    if (!shell_safe_name(g_download_task.version) ||
        !shell_safe_name(g_download_task.file_name) ||
        (g_download_task.file_path[0] != '\0' &&
         !shell_safe_relpath(g_download_task.file_path))) {
        LOG_ERROR("Rejected version download: unsafe field "
                  "(version='%s', file_name='%s', file_path='%s')",
                  g_download_task.version, g_download_task.file_name,
                  g_download_task.file_path);
        send_version_download_result(&msg->header, ie_req->ver_type, DOWNLOAD_RESULT_OTHER);
        /* 闸门在此处已置位（见函数开头的持锁区间），提前返回必须放闸，
         * 否则后续所有下载请求都会被"already in progress"永久拒绝。 */
        pthread_mutex_lock(&g_download_mutex);
        g_download_task.in_progress = false;
        pthread_mutex_unlock(&g_download_mutex);
        return ERROR_INVALID_PARAM;
    }

    /* 下载线程把 file_len 与落地文件大小做一致性比对，这里是唯一的来源，
     * 只做量级合理性检查，避免明显离谱的值（如 32 位回绕）被当成正常值记录。 */
    if (g_download_task.file_len > MAX_IMAGE_BYTES) {
        LOG_ERROR("Rejected version download: file_len too large (%u > %u)",
                  g_download_task.file_len, (unsigned)MAX_IMAGE_BYTES);
        send_version_download_result(&msg->header, ie_req->ver_type, DOWNLOAD_RESULT_TOO_LARGE);
        pthread_mutex_lock(&g_download_mutex);
        g_download_task.in_progress = false;
        pthread_mutex_unlock(&g_download_mutex);
        return ERROR_INVALID_PARAM;
    }

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

/* 异步执行服务重启：fork 一个脱离会话的子进程，先睡 3 秒（给 BBU 接收应答留时间），
 * 再 execvp 执行。
 *
 * 改造前这里是 sleep(3) + system("systemctl restart antenna-mgmt.service &")，
 * 两者都跑在 TCP 接收线程上：3 秒不读 socket 会把 BBU 心跳堵在内核缓冲里
 * （本项目反复踩过这个坑，cell_config 与 reset 都为此改成了异步），而 system()
 * 还会拉起一个 shell —— 全项目其余调用点都已改成 shell_run 的 argv 形式，
 * 只剩这里还是 shell 字符串。本函数与 reset.c 的 reset_schedule_action 同构：
 * 不经 shell、不在接收线程里 sleep。fork/exec 失败只记日志，不影响已发出的应答。 */
static void version_schedule_service_restart(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("Failed to fork for service restart: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        setsid();           /* 脱离会话，父进程退出/重启都不影响它 */
        sleep(3);
        char *argv[] = { (char *)"systemctl", (char *)"restart",
                         (char *)"antenna-mgmt.service", NULL };
        execvp(argv[0], argv);
        _exit(127);         /* exec 失败 */
    }

    LOG_INFO("Scheduled service restart in 3 seconds (helper pid=%d)", (int)pid);
}

int version_handle_activate_request(const cpri_message_t *msg)
{
    if (!msg || !msg->payload) {
        LOG_ERROR("Invalid version activate request message");
        return ERROR_INVALID_PARAM;
    }

    /* 长度门：同下载请求，不足一个完整 IE 311 时丢弃，避免读到池残留数据 */
    if (msg->payload_len < sizeof(ie_version_activate_ind_t)) {
        LOG_ERROR("Version activate request payload too short: %u bytes (need %zu), dropped",
                  msg->payload_len, sizeof(ie_version_activate_ind_t));
        return ERROR_INVALID_PARAM;
    }

    /* 解析IE 311 */
    ie_version_activate_ind_t *ie_ind = (ie_version_activate_ind_t *)msg->payload;

    /* version_num 是定长字段，不保证 NUL 结尾，用 %.*s 限定读取长度 */
    LOG_INFO("Received version activate request: version=%.*s, type=%u",
            (int)sizeof(ie_ind->version_num), ie_ind->version_num, ie_ind->ver_type);

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

    /* 【关键】version 来自 IE 311 的 version_num，是北向直接可控的字段。
     * 它随后被拼进 version_dir（"%s/%s"）并用于 stat/lstat/readlink 与目录切换，
     * 属与下载路径同一类的二阶注入面：含 '/' 或 '..' 可指向 VERSION_BASE_DIR
     * 之外（如 ../../etc），为空则退化成 VERSION_BASE_DIR 本身。
     * 与下载入口（version_handle_download_request）用同一个 shell_safe_name 过滤。 */
    if (!shell_safe_name(version)) {
        LOG_ERROR("SECURITY ALERT: unsafe version in activate request, rejected: '%s'",
                  version);
        send_version_activate_ack(&msg->header, ie_ind->ver_type, ACTIVATE_RESULT_OTHER);
        return ERROR_INVALID_PARAM;
    }

    uint8_t result = ACTIVATE_RESULT_SUCCESS;
    char fw_dir[512] = {0};     /* 固件激活时找到的实际目录，供后面上注使用 */

    /* 根据版本类型执行不同的激活逻辑 */
    if (ie_ind->ver_type == VERSION_TYPE_FIRMWARE) {
        /* 固件版本：只验证版本是否存在，不修改软链接。
         * 固件按 FPGA 种类分目录存放（FIRMWARE_BASE_DIR/<种类>/<版本>），
         * IE 311 不带种类，由 find_firmware_directory 在各种类目录中查找。 */
        LOG_INFO("Activating firmware version: %s (will not change current symlink)", version);

        fpga_kind_t kind = FPGA_KIND_UNKNOWN;
        if (find_firmware_directory(version, fw_dir, sizeof(fw_dir), &kind) != SUCCESS) {
            LOG_ERROR("Firmware version not found under %s/{v7a,v7b,subarray}: %s",
                      FIRMWARE_BASE_DIR, version);
            result = ACTIVATE_RESULT_NOT_EXIST;
        } else {
            /* 从实际目录路径中提取版本号（大小写可能与请求不同） */
            const char *actual_version = strrchr(fw_dir, '/');
            actual_version = actual_version ? actual_version + 1 : version;

            /* 用上注时同一个解析函数校验，并核对包内子类型与所在目录一致，
             * 防止目录被人为挪动/拷错后把一种 FPGA 的固件写进另一种 FPGA。 */
            firmware_metadata_t fw_meta;
            if (firmware_parse_metadata(fw_dir, &fw_meta) != SUCCESS) {
                LOG_ERROR("Failed to parse firmware metadata in %s", fw_dir);
                result = ACTIVATE_RESULT_CORRUPTED;
            } else if (firmware_fpga_kind(fw_meta.file_sub_type) != kind) {
                LOG_ERROR("Firmware sub_type 0x%02X (%s) does not match its directory %s (%s)",
                          fw_meta.file_sub_type,
                          firmware_fpga_kind_name(firmware_fpga_kind(fw_meta.file_sub_type)),
                          fw_dir, firmware_fpga_kind_name(kind));
                result = ACTIVATE_RESULT_MISMATCH;
            } else {
                LOG_INFO("Firmware version validated: %s (actual: %s, FPGA=%s, dir=%s)",
                         version, actual_version, firmware_fpga_kind_name(kind), fw_dir);
                /* 更新 version 为实际版本号，用于后续的固件上注 */
                strncpy(version, actual_version, sizeof(version) - 1);
                version[sizeof(version) - 1] = '\0';
                result = ACTIVATE_RESULT_SUCCESS;
            }
        }
    } else {
        /* 软件版本：执行版本切换（修改软链接） */
        LOG_INFO("Activating software version: %s (will update current symlink)", version);
        result = switch_to_version(version, "BBU activate request", true);
    }

    /* 发送激活应答 */
    send_version_activate_ack(&msg->header, ie_ind->ver_type, result);

    if (result == ACTIVATE_RESULT_SUCCESS) {
        LOG_INFO("Version activated successfully");

        /* 如果是固件类型,触发FPGA固件上注 */
        if (ie_ind->ver_type == VERSION_TYPE_FIRMWARE) {
            LOG_INFO("触发FPGA固件上注: %s (%s)", version, fw_dir);

            /* 启动后台上注任务（目录为激活时查到的 FIRMWARE_BASE_DIR/<种类>/<版本>） */
            int ret = fpga_firmware_injection_start(fw_dir, version);
            if (ret != SUCCESS) {
                LOG_ERROR("启动FPGA固件上注失败");
                /* 已发送激活成功应答,记录错误但继续 */
            }
        } else {
            /* 软件版本激活 */
            LOG_INFO("Software version activated: %s", version);
            LOG_INFO("System will restart to apply new software version...");

            /* 原先这里会写 /tmp/antenna_mgmt_restart_pending，但没有任何程序读取它，
             * 且服务配置了 PrivateTmp=true，写入的是服务私有 /tmp。已移除；
             * 新版本能否起来由试运行机制（VERSION_TRIAL_FILE + upgrade-guard.sh）负责。 */

            /* 延迟 3 秒后重启服务（给 BBU 时间接收应答）。
             * 异步执行：不在此线程 sleep，也不经 shell（见 version_schedule_service_restart）。 */
            version_schedule_service_restart();
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

void version_trial_on_channel_established(void)
{
    pthread_mutex_lock(&g_trial_mutex);
    if (g_trial_confirmed) {
        pthread_mutex_unlock(&g_trial_mutex);
        return;
    }
    g_trial_confirmed = true;

    version_trial_t t;
    if (trial_read(&t) == SUCCESS) {
        char cur[512];
        read_current_link(cur, sizeof(cur));
        if (strcmp(cur, t.new_dir) == 0) {
            LOG_INFO("Upgrade confirmed healthy (channel to BBU established): %s", t.new_dir);
        } else {
            LOG_WARN("Removing stale upgrade trial record (trial=%s, current=%s)", t.new_dir, cur);
        }
        trial_remove();
    }

    /* 自动回退告警已在 TCP 建链时补报，通道也建好了，通知文件完成使命 */
    if (unlink(VERSION_AUTO_ROLLBACK_NOTICE) == 0) {
        LOG_INFO("Auto rollback notice delivered, removed %s", VERSION_AUTO_ROLLBACK_NOTICE);
    }
    pthread_mutex_unlock(&g_trial_mutex);
}

void version_trial_on_uptime(uint32_t uptime_sec)
{
    if (uptime_sec < g_trial_stable_sec) {
        return;
    }

    pthread_mutex_lock(&g_trial_mutex);
    if (g_trial_stable_done || g_trial_confirmed) {
        pthread_mutex_unlock(&g_trial_mutex);
        return;
    }
    g_trial_stable_done = true;

    version_trial_t t;
    if (trial_read(&t) == SUCCESS && t.attempts != 0) {
        char cur[512];
        read_current_link(cur, sizeof(cur));
        if (strcmp(cur, t.new_dir) == 0) {
            /* 能稳定运行这么久，说明不是"一启动就崩"。清零后，之后的断电/正常
             * 重启不会被累计成启动失败；记录保留，等与 BBU 通道建立后再确认。 */
            t.attempts = 0;
            if (trial_write(&t) == SUCCESS) {
                LOG_INFO("New version ran stable for %us, trial start counter reset "
                         "(still waiting for BBU channel to confirm)", uptime_sec);
            }
        }
    }
    pthread_mutex_unlock(&g_trial_mutex);
}

int version_rollback(const char *version, const char *reason)
{
    if (!version) {
        LOG_ERROR("Invalid version parameter");
        return ERROR_INVALID_PARAM;
    }

    LOG_INFO("Rolling back to version: %s, reason: %s", version, reason ? reason : "manual");

    uint8_t result = switch_to_version(version, reason, false);
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
    /* 先置退出标志：下载线程在发 TCP 报文前会检查它，置位后不再发送。
     *
     * 【刻意不 join 下载线程】它是 detached 的，且 curl 的 --max-time 是 600 秒，
     * join 可能把退出流程卡住十分钟。因此这里只缩小竞态窗口、并不消除它：
     * 线程若已越过那个检查、正处在 tcp_client_send 内部，仍可能撞上已被销毁的
     * send_mutex。与 log_upload 是同一种取舍。
     * 必须由调用方（main.c）把本函数排在 tcp_client_stop 之前，置位才会发生在
     * TCP 拆卸之前 —— 原先它排在 tcp_client_destroy 之后，标志加在这里也来不及。 */
    g_shutting_down = true;

    pthread_mutex_lock(&g_download_mutex);
    if (g_download_task.in_progress) {
        LOG_WARN("Version download in progress during destroy, not waiting for it "
                 "(shutdown flag set, its TCP result will be skipped)");
    }
    pthread_mutex_unlock(&g_download_mutex);

    LOG_INFO("Version manager destroyed");
}



