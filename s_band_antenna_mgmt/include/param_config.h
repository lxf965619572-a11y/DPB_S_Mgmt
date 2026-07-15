#ifndef PARAM_CONFIG_H
#define PARAM_CONFIG_H

#include <stdint.h>
#include "cpri_protocol.h"

/* IE 11: 系统时间配置 */
typedef struct __attribute__((packed)) {
    uint16_t ie_type;   /* IE标志 = 11 */
    uint16_t ie_length; /* IE长度 = 11字节 */
    uint8_t  second;    /* 0-59 */
    uint8_t  minute;    /* 0-59 */
    uint8_t  hour;      /* 0-23 */
    uint8_t  day;       /* 1-31 */
    uint8_t  month;     /* 1-12 */
    uint16_t year;      /* 如2024 */
} system_time_config_ie_t;

/* IE 502: CPU占用率统计周期配置 */
typedef struct __attribute__((packed)) {
    uint16_t ie_type;   /* IE标志 = 502 */
    uint16_t ie_length; /* IE长度 = 8字节 */
    uint32_t period;    /* 统计周期(秒) */
} cpu_period_config_ie_t;

/* IE 504: CPRI口工作模式配置 */
typedef struct __attribute__((packed)) {
    uint16_t ie_type;   /* IE标志 = 504 */
    uint16_t ie_length; /* IE长度 = 8字节 */
    uint32_t work_mode; /* 1:普通 2:级联 3:主备 4:负荷分担 */
} cpri_work_mode_config_ie_t;

/* IE 551: 系统时间配置响应 */
typedef struct __attribute__((packed)) {
    uint16_t ie_type;   /* IE标志 = 551 */
    uint16_t ie_length; /* IE长度 = 8字节 */
    uint32_t result;    /* 0:成功 1:失败 */
} system_time_config_resp_ie_t;

/* IE 553: CPU占用率统计周期配置响应 */
typedef struct __attribute__((packed)) {
    uint16_t ie_type;   /* IE标志 = 553 */
    uint16_t ie_length; /* IE长度 = 8字节 */
    uint32_t result;    /* 0:成功 1:失败 */
} cpu_period_config_resp_ie_t;

/* IE 555: CPRI口工作模式配置响应 */
typedef struct __attribute__((packed)) {
    uint16_t ie_type;   /* IE标志 = 555 */
    uint16_t ie_length; /* IE长度 = 10字节 */
    uint8_t  main_fiber_port;  /* 主光纤端口号 */
    uint8_t  sub_fiber_port;   /* 辅光纤端口号 */
    uint32_t result;           /* 0:成功 1:失败 */
} cpri_work_mode_config_resp_ie_t;

/* IE ID 定义 */
#define IE_SYSTEM_TIME_CONFIG           11
#define IE_CPU_PERIOD_CONFIG            502
#define IE_CPRI_WORK_MODE_CONFIG        504
#define IE_SYSTEM_TIME_CONFIG_RESP      551
#define IE_CPU_PERIOD_CONFIG_RESP       553
#define IE_CPRI_WORK_MODE_CONFIG_RESP   555

/* 消息ID定义 */
#define MSG_ID_PARAM_CONFIG             61
#define MSG_ID_PARAM_CONFIG_RESP        62

/* CPRI工作模式定义 */
#define CPRI_MODE_NORMAL                0  /* 普通模式 */
#define CPRI_MODE_CASCADE               1  /* 级联模式 */
#define CPRI_MODE_MASTER_SLAVE          2  /* 主备模式 */
#define CPRI_MODE_LOAD_SHARING          3  /* 负荷分担模式 */

/* 配置结果定义 */
#define CONFIG_RESULT_SUCCESS           0
#define CONFIG_RESULT_FAILURE           1

/* 无效光纤端口号 */
#define INVALID_FIBER_PORT              0xFF

/**
 * @brief 初始化参数配置模块
 * @return 0成功, 非0失败
 */
int param_config_init(void);

/**
 * @brief 处理参数配置请求
 * @param request 配置请求消息
 * @param response 配置响应消息(输出)
 * @return 0成功, 非0失败
 */
int param_config_handle_request(const cpri_message_t *request, cpri_message_t *response);

/**
 * @brief 配置系统时间
 * @param config_ie 系统时间配置IE
 * @return 0成功, 非0失败
 */
int param_config_set_system_time(const system_time_config_ie_t *config_ie);

/**
 * @brief 配置CPU占用率统计周期
 * @param config_ie CPU周期配置IE
 * @return 0成功, 非0失败
 */
int param_config_set_cpu_period(const cpu_period_config_ie_t *config_ie);

/**
 * @brief 配置CPRI工作模式
 * @param config_ie CPRI工作模式配置IE
 * @return 0成功, 非0失败
 */
int param_config_set_cpri_mode(const cpri_work_mode_config_ie_t *config_ie);

/**
 * @brief 获取当前CPU统计周期
 * @return 当前周期(秒)
 */
uint32_t param_config_get_cpu_period(void);

#endif /* PARAM_CONFIG_H */
