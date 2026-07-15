#include "firmware_package.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <ctype.h>

int firmware_package_init(void)
{
    LOG_INFO("固件包解析模块已初始化");
    return SUCCESS;
}

void firmware_package_cleanup(void)
{
    LOG_INFO("固件包解析模块已清理");
}

/* 辅助函数: 去除字符串首尾空白 */
static char* trim_whitespace(char *str)
{
    char *end;

    /* 去除前导空白 */
    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) return str;

    /* 去除尾部空白 */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    end[1] = '\0';
    return str;
}

int firmware_parse_metadata(const char *version_dir, firmware_metadata_t *metadata)
{
    if (!version_dir || !metadata) {
        return ERROR_INVALID_PARAM;
    }

    memset(metadata, 0, sizeof(firmware_metadata_t));

    /* 构造metadata.txt路径 */
    char metadata_path[512];
    snprintf(metadata_path, sizeof(metadata_path), "%s/metadata.txt", version_dir);

    /* 读取文本文件 */
    FILE *fp = fopen(metadata_path, "r");
    if (!fp) {
        LOG_ERROR("无法打开元数据文件: %s", metadata_path);
        return ERROR_GENERAL;
    }

    char line[512];
    char bin_filename[256] = {0};
    int fields_found = 0;

    /* 逐行解析 key=value 格式 */
    while (fgets(line, sizeof(line), fp)) {
        /* 跳过注释和空行 */
        char *trimmed = trim_whitespace(line);
        if (trimmed[0] == '#' || trimmed[0] == '\0') {
            continue;
        }

        /* 查找等号 */
        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim_whitespace(trimmed);
        char *value = trim_whitespace(eq + 1);

        /* 解析各字段 */
        if (strcmp(key, "file_type") == 0) {
            metadata->file_type = (uint8_t)strtoul(value, NULL, 0);
            fields_found++;
        } else if (strcmp(key, "file_sub_type") == 0) {
            metadata->file_sub_type = (uint8_t)strtoul(value, NULL, 0);
            fields_found++;
        } else if (strcmp(key, "sha256") == 0) {
            strncpy(metadata->sha256, value, sizeof(metadata->sha256) - 1);
            fields_found++;
        } else if (strcmp(key, "version") == 0) {
            strncpy(metadata->version, value, sizeof(metadata->version) - 1);
            fields_found++;
        } else if (strcmp(key, "bin_file") == 0) {
            strncpy(bin_filename, value, sizeof(bin_filename) - 1);
            fields_found++;
        }
    }

    fclose(fp);

    /* 检查必需字段 */
    if (fields_found < 5) {
        LOG_ERROR("元数据缺少必需字段 (找到 %d/5 个字段)", fields_found);
        return ERROR_GENERAL;
    }

    /* 构造.bin文件完整路径 */
    snprintf(metadata->bin_file_path, sizeof(metadata->bin_file_path),
             "%s/%s", version_dir, bin_filename);

    /* 获取文件大小 */
    int ret = firmware_get_file_size(metadata->bin_file_path, &metadata->file_size);
    if (ret != SUCCESS) {
        LOG_ERROR("无法获取固件文件大小: %s", metadata->bin_file_path);
        return ret;
    }

    LOG_INFO("固件元数据解析成功:");
    LOG_INFO("  文件类型: 0x%02X", metadata->file_type);
    LOG_INFO("  文件子类型: 0x%02X", metadata->file_sub_type);
    LOG_INFO("  版本: %s", metadata->version);
    LOG_INFO("  文件大小: %u 字节", metadata->file_size);
    LOG_INFO("  SHA256: %s", metadata->sha256);
    LOG_INFO("  .bin文件: %s", metadata->bin_file_path);

    return SUCCESS;
}

int firmware_verify_integrity(const char *bin_file_path, const char *expected_sha256)
{
    if (!bin_file_path || !expected_sha256) {
        return ERROR_INVALID_PARAM;
    }

    FILE *fp = fopen(bin_file_path, "rb");
    if (!fp) {
        LOG_ERROR("无法打开固件文件: %s", bin_file_path);
        return ERROR_GENERAL;
    }

    /* 计算SHA256 */
    SHA256_CTX sha256_ctx;
    SHA256_Init(&sha256_ctx);

    uint8_t buffer[8192];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        SHA256_Update(&sha256_ctx, buffer, bytes_read);
    }

    fclose(fp);

    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256_ctx);

    /* 转换为十六进制字符串 */
    char calculated_sha256[65];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(calculated_sha256 + (i * 2), "%02x", hash[i]);
    }
    calculated_sha256[64] = '\0';

    /* 比较SHA256 */
    if (strcasecmp(calculated_sha256, expected_sha256) != 0) {
        LOG_ERROR("SHA256校验失败!");
        LOG_ERROR("  期望: %s", expected_sha256);
        LOG_ERROR("  实际: %s", calculated_sha256);
        return ERROR_GENERAL;
    }

    LOG_INFO("SHA256校验通过: %s", calculated_sha256);
    return SUCCESS;
}

int firmware_get_file_size(const char *bin_file_path, uint32_t *file_size)
{
    if (!bin_file_path || !file_size) {
        return ERROR_INVALID_PARAM;
    }

    struct stat st;
    if (stat(bin_file_path, &st) != 0) {
        LOG_ERROR("无法获取文件状态: %s", bin_file_path);
        return ERROR_GENERAL;
    }

    *file_size = (uint32_t)st.st_size;
    return SUCCESS;
}
