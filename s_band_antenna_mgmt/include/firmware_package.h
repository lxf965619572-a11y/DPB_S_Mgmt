/**
 * @file firmware_package.h
 * @brief 固件包解析模块 - 解析固件元数据并验证完整性
 */

#ifndef FIRMWARE_PACKAGE_H
#define FIRMWARE_PACKAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "common.h"

/* 固件元数据结构 */
typedef struct {
    uint8_t file_type;          /* 文件类型: 0xF0-0xFF */
    uint8_t file_sub_type;      /* 文件子类型: 0x00-0xFF */
    char sha256[65];            /* SHA256哈希值(十六进制字符串) */
    uint32_t file_size;         /* 文件大小(字节) */
    char version[40];           /* 版本号 */
    char bin_file_path[256];    /* .bin文件路径 */
} firmware_metadata_t;

/**
 * @brief 初始化固件包解析模块
 * @return 成功返回SUCCESS,失败返回错误码
 */
int firmware_package_init(void);

/**
 * @brief 清理固件包解析模块
 */
void firmware_package_cleanup(void);

/**
 * @brief 解析固件包元数据
 * @param version_dir 版本目录路径
 * @param metadata 输出:元数据结构
 * @return 成功返回SUCCESS,失败返回错误码
 */
int firmware_parse_metadata(const char *version_dir, firmware_metadata_t *metadata);

/**
 * @brief 验证固件文件完整性(SHA256)
 * @param bin_file_path .bin文件路径
 * @param expected_sha256 期望的SHA256值(十六进制字符串)
 * @return 成功返回SUCCESS,失败返回错误码
 */
int firmware_verify_integrity(const char *bin_file_path, const char *expected_sha256);

/**
 * @brief 获取固件文件大小
 * @param bin_file_path .bin文件路径
 * @param file_size 输出:文件大小
 * @return 成功返回SUCCESS,失败返回错误码
 */
int firmware_get_file_size(const char *bin_file_path, uint32_t *file_size);

#endif /* FIRMWARE_PACKAGE_H */
