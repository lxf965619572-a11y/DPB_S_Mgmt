/**
 * @file param_query.h
 * @brief 参数查询模块 - 处理BBU的参数查询请求并返回响应
 * @details 支持以下参数查询:
 *          - 系统时间查询
 *          - CPU占用率查询
 *          - CPU占用率统计周期查询
 *          - 相控阵温度查询
 *          - 过温门限查询
 *          - 输出功率查询
 *          - Toffset/T2a/Ta3延时查询
 */

#ifndef PARAM_QUERY_H
#define PARAM_QUERY_H

#include "common.h"
#include "cpri_protocol.h"
#include "fpga_protocol.h"

/* 查询请求IE类型定义 (MsgID: 51) */
#define IE_TYPE_SYSTEM_TIME_QUERY       401     /* 系统时间查询 */
#define IE_TYPE_CPU_USAGE_QUERY         402     /* CPU占用率查询 */
#define IE_TYPE_CPU_PERIOD_QUERY        403     /* CPU占用率统计周期查询 */
#define IE_TYPE_TEMP_QUERY              404     /* 相控阵温度查询 */
#define IE_TYPE_TEMP_THRESHOLD_QUERY    407     /* 过温门限查询 */
#define IE_TYPE_OUTPUT_POWER_QUERY      408     /* 输出功率查询 */
#define IE_TYPE_TOFFSET_QUERY           409     /* Toffset查询 */
#define IE_TYPE_T2A_QUERY               410     /* T2a查询 */
#define IE_TYPE_TA3_QUERY               411     /* Ta3查询 */

/* 查询响应IE类型定义 (MsgID: 52) */
#define IE_TYPE_SYSTEM_TIME_RESP        11     /* 系统时间响应 */
#define IE_TYPE_CPU_USAGE_RESP          451     /* CPU占用率响应 */
#define IE_TYPE_CPU_PERIOD_RESP         452     /* CPU占用率统计周期响应 */
#define IE_TYPE_TEMP_RESP               453     /* 相控阵温度响应 */
#define IE_TYPE_TEMP_THRESHOLD_RESP     456     /* 过温门限响应 */
#define IE_TYPE_OUTPUT_POWER_RESP       457     /* 输出功率响应 */
#define IE_TYPE_TOFFSET_RESP            458     /* Toffset响应 */
#define IE_TYPE_T2A_RESP                459     /* T2a响应 */
#define IE_TYPE_TA3_RESP                460     /* Ta3响应 */

/* 查询结果 */
#define QUERY_RESULT_SUCCESS    0       /* 查询成功 */
#define QUERY_RESULT_FAILURE    1       /* 查询失败 */

/* 查询请求IE结构 */

/* IE 404: 相控阵温度查询 */
typedef struct __attribute__((packed)) {
    uint8_t temp_point;         /* 测温点索引 (0~N) */
} temp_query_ie_t;

/* IE 407: 过温门限查询 */
typedef struct __attribute__((packed)) {
    uint8_t temp_point;         /* 测温点索引 (0~N) */
} temp_threshold_query_ie_t;

/* IE 408: 输出功率查询 */
typedef struct __attribute__((packed)) {
    uint16_t rf_channel;        /* 射频通道号 */
} output_power_query_ie_t;

/* 查询响应IE结构 */

/* IE 551: 系统时间响应 */
typedef struct __attribute__((packed)) {
    uint8_t second;                 /* 秒: 0-59 */
    uint8_t minute;                 /* 分: 0-59 */
    uint8_t hour;                   /* 时: 0-23 */
    uint8_t day;                    /* 日: 1-31 */
    uint8_t month;                  /* 月: 1-12 */
    uint16_t year;                  /* 年: 1970-2099 */
} system_time_resp_ie_t;

/* IE 451: CPU占用率响应 */
typedef struct __attribute__((packed)) {
    uint32_t cpu_usage;         /* CPU占用率 (0-100) */
} cpu_usage_resp_ie_t;

/* IE 452: CPU占用率统计周期响应 */
typedef struct __attribute__((packed)) {
    uint32_t period;            /* 统计周期 (秒) */
} cpu_period_resp_ie_t;

/* IE 453: 相控阵温度响应 */
typedef struct __attribute__((packed)) {
    uint8_t temp_point;         /* 测温点索引 */
    int8_t temperature;         /* 温度值 (摄氏度, 有符号) */
} temp_resp_ie_t;

/* IE 456: 过温门限响应 */
typedef struct __attribute__((packed)) {
    uint8_t temp_point;         /* 测温点索引 */
    int8_t up_threshold;        /* 温度上门限 (摄氏度, 有符号) */
    int8_t low_threshold;       /* 温度下门限 (摄氏度, 有符号) */
} temp_threshold_resp_ie_t;

/* IE 457: 输出功率响应 */
typedef struct __attribute__((packed)) {
    uint8_t reserved;           /* 保留 */
    uint16_t power;             /* 输出功率 (1/256 dBm) */
} output_power_resp_ie_t;

/* IE 458: Toffset响应 */
typedef struct __attribute__((packed)) {
    uint32_t delay;             /* CPRI Toffset值 */
} toffset_resp_ie_t;

/* IE 459: T2a响应 */
typedef struct __attribute__((packed)) {
    uint32_t delay;             /* CPRI T2a值 */
} t2a_resp_ie_t;

/* IE 460: Ta3响应 */
typedef struct __attribute__((packed)) {
    uint32_t delay;             /* CPRI Ta3值 */
} ta3_resp_ie_t;

/* CPU统计信息 */
typedef struct {
    uint32_t cpu_usage;         /* CPU占用率 (0-100) */
    uint32_t stat_period;       /* 统计周期 (秒) */
    time_t last_update;         /* 最后更新时间 */
} cpu_stats_t;

/**
 * @brief 初始化参数查询模块
 * @return 0成功, -1失败
 */
int param_query_init(void);

/**
 * @brief 清理参数查询模块
 */
void param_query_cleanup(void);

/**
 * @brief 处理参数查询请求
 * @param request 查询请求消息
 * @param response 查询响应消息(输出)
 * @return 0成功, -1失败
 */
int param_query_handle_request(const cpri_message_t *request, cpri_message_t *response);

/**
 * @brief 获取CPU占用率
 * @return CPU占用率 (0-100)
 */
uint32_t param_query_get_cpu_usage(void);

/**
 * @brief 获取CPU统计周期
 * @return 统计周期 (秒)
 */
uint32_t param_query_get_cpu_period(void);

/**
 * @brief 更新CPU统计信息
 * @param usage CPU占用率 (0-100)
 */
void param_query_update_cpu_stats(uint32_t usage);

/* 各个参数查询处理函数 */

/**
 * @brief 处理系统时间查询
 * @param response 响应消息
 * @return 0成功, -1失败
 */
int param_query_handle_system_time(cpri_message_t *response);

/**
 * @brief 处理CPU占用率查询
 * @param response 响应消息
 * @return 0成功, -1失败
 */
int param_query_handle_cpu_usage(cpri_message_t *response);

/**
 * @brief 处理CPU统计周期查询
 * @param response 响应消息
 * @return 0成功, -1失败
 */
int param_query_handle_cpu_period(cpri_message_t *response);

/**
 * @brief 处理相控阵温度查询
 * @param temp_point 测温点索引
 * @param response 响应消息
 * @return 0成功, -1失败
 */
int param_query_handle_temperature(uint8_t temp_point, cpri_message_t *response);

/**
 * @brief 处理过温门限查询
 * @param temp_point 测温点索引
 * @param response 响应消息
 * @return 0成功, -1失败
 */
int param_query_handle_temp_threshold(uint8_t temp_point, cpri_message_t *response);

/**
 * @brief 处理输出功率查询
 * @param rf_channel 射频通道号
 * @param response 响应消息
 * @return 0成功, -1失败
 */
int param_query_handle_output_power(uint16_t rf_channel, cpri_message_t *response);

/**
 * @brief 处理Toffset查询
 * @param response 响应消息
 * @return 0成功, -1失败
 */
int param_query_handle_toffset(cpri_message_t *response);

/**
 * @brief 处理T2a查询
 * @param response 响应消息
 * @return 0成功, -1失败
 */
int param_query_handle_t2a(cpri_message_t *response);

/**
 * @brief 处理Ta3查询
 * @param response 响应消息
 * @return 0成功, -1失败
 */
int param_query_handle_ta3(cpri_message_t *response);

#endif /* PARAM_QUERY_H */
