/**
 * @file fpga_firmware_injector.h
 * @brief FPGA固件注入器 - 状态机驱动的固件上注流程
 */

#ifndef FPGA_FIRMWARE_INJECTOR_H
#define FPGA_FIRMWARE_INJECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include "common.h"
#include "firmware_package.h"
#include "uart_rs422_client.h"

/* 注入状态定义 */
typedef enum {
    FPGA_INJ_STATE_IDLE = 0,                /* 空闲状态 */
    FPGA_INJ_STATE_PARSING_METADATA,        /* 解析元数据 */
    FPGA_INJ_STATE_TRANSFER_START,          /* 传输开始 */
    FPGA_INJ_STATE_TRANSFER_DATA,           /* 传输数据 */
    FPGA_INJ_STATE_TRANSFER_END,            /* 传输结束 */
    FPGA_INJ_STATE_RECONFIG_START,          /* 重构开始 */
    FPGA_INJ_STATE_RECONFIG_POLLING,        /* 重构轮询 */
    FPGA_INJ_STATE_COMPLETED,               /* 完成 */
    FPGA_INJ_STATE_FAILED                   /* 失败 */
} fpga_injection_state_t;

/* 两层结构定义 */
#define FPGA_SEGMENT_SIZE           (1024 * 1024)   /* 第一层：1MB段大小 */
#define FPGA_PACKET_SIZE            1000            /* 第二层：1000字节包大小 */

/* 超时定义 */
#define FPGA_TRANSFER_START_TIMEOUT_SEC     300     /* 传输开始超时: 5分钟 */
#define FPGA_RECONFIG_TIMEOUT_SEC           600     /* 重构超时: 10分钟 */
#define FPGA_RECONFIG_POLL_INTERVAL_SEC     5       /* 重构轮询间隔: 5秒 */

/* 重试配置 */
#define FPGA_MAX_PACKET_RETRIES     3       /* 每包最大重试次数 */

/* 注入任务上下文 */
typedef struct {
    fpga_injection_state_t state;           /* 当前状态 */
    firmware_metadata_t metadata;           /* 固件元数据 */
    char version_dir[256];                  /* 版本目录路径 */
    char version_num[40];                   /* 版本号 */

    /* 传输状态 - 两层结构 */
    uint32_t total_segments;                /* 总段数（1MB段） */
    uint32_t current_segment;               /* 当前段号（1MB段编号） */
    uint32_t total_packets_in_segment;      /* 当前段内的总包数 */
    uint32_t current_packet_in_segment;     /* 当前段内的包编号 */
    uint32_t segment_offset;                /* 当前段在文件中的偏移量 */
    uint16_t file_checksum;                 /* 文件CRC16-CCITT-FALSE */
    FILE *bin_fp;                           /* .bin文件指针 */

    /* 时间和重试 */
    time_t start_time;                      /* 开始时间 */
    time_t last_activity;                   /* 最后活动时间 */
    uint32_t retry_count;                   /* 当前重试次数 */
    uint32_t max_retries;                   /* 最大重试次数 */

    /* 进度回调 */
    void (*progress_callback)(uint32_t current, uint32_t total);

    /* 线程管理 */
    pthread_t injection_thread;             /* 注入线程 */
    pthread_mutex_t state_mutex;            /* 状态互斥锁 */
    bool in_progress;                       /* 是否正在进行 */
    bool abort_requested;                   /* 是否请求中止 */

    /* UART客户端引用 */
    uart_rs422_client_t *uart_client;       /* UART客户端 */
} fpga_injection_task_t;

/**
 * @brief 初始化FPGA固件注入器
 * @param uart_client UART RS-422客户端
 * @return 成功返回SUCCESS,失败返回错误码
 */
int fpga_firmware_injection_init(uart_rs422_client_t *uart_client);

/**
 * @brief 启动固件注入任务(后台线程)
 * @param version_dir 版本目录路径
 * @param version_num 版本号
 * @return 成功返回SUCCESS,失败返回错误码
 */
int fpga_firmware_injection_start(const char *version_dir, const char *version_num);

/**
 * @brief 获取注入进度
 * @param current 输出:当前进度
 * @param total 输出:总进度
 * @param state 输出:当前状态
 * @return 成功返回SUCCESS,失败返回错误码
 */
int fpga_firmware_injection_get_progress(uint32_t *current, uint32_t *total,
                                          fpga_injection_state_t *state);

/**
 * @brief 中止注入任务
 * @return 成功返回SUCCESS,失败返回错误码
 */
int fpga_firmware_injection_abort(void);

/**
 * @brief 等待注入完成
 * @param timeout_sec 超时时间(秒), 0表示无限等待
 * @return 成功返回SUCCESS,超时返回ERROR_TIMEOUT,失败返回错误码
 */
int fpga_firmware_injection_wait(uint32_t timeout_sec);

/**
 * @brief 清理FPGA固件注入器
 */
void fpga_firmware_injection_cleanup(void);

/**
 * @brief 获取状态名称
 * @param state 状态
 * @return 状态名称字符串
 */
const char* fpga_injection_get_state_name(fpga_injection_state_t state);

#endif /* FPGA_FIRMWARE_INJECTOR_H */
