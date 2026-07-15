#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

/* 返回值定义 */
#define SUCCESS             0
#define ERROR_GENERAL      -1
#define ERROR_INVALID_PARAM -2
#define ERROR_MEMORY       -3
#define ERROR_NETWORK      -4
#define ERROR_TIMEOUT      -5
#define ERROR_NOT_INITIALIZED -6

/* 日志级别 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

/* 全局配置结构 */
typedef struct {
    char bbu_ip[32];
    uint16_t bbu_port;
    char paau_ip[32];
    uint32_t reconnect_interval_ms;
    uint32_t heartbeat_interval_ms;
    log_level_t log_level;

    /* 日志轮转配置 */
    size_t log_max_size;           // 日志文件最大大小（字节）
    int log_max_backups;           // 保留的备份文件数量
    bool log_rotate_on_startup;    // 启动时是否轮转
} global_config_t;

extern global_config_t g_config;

#endif /* COMMON_H */
