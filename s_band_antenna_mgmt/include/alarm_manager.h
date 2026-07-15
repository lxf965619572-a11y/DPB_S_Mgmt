/**
 * @file alarm_manager.h
 * @brief 告警管理模块 - 完整实现告警检测、存储和上报功能
 */

#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include "common.h"
#include "cpri_protocol.h"
#include "fpga_protocol.h"

/* IE类型定义 */
#define IE_TYPE_ALARM_REPORT    0x3E9   /* 1001: 告警上报请求 */

/* 告警码定义 (根据表C.1) */
#define ALARM_CODE_LO_UNLOCK           50001    /* 本振失锁告警 */
#define ALARM_CODE_OPTICAL_SYNC_LOSS   1097     /* 光链路同步码流丢失 */
#define ALARM_CODE_OVER_TEMPERATURE    1156     /* 过温告警 */
#define ALARM_CODE_VERSION_ACTIVATE_FAIL 1158   /* 版本激活失败 */
#define ALARM_CODE_CHANNEL_FAULT_EXCEED  1159   /* 通道故障过多 */
#define ALARM_CODE_TCP_DISCONNECT      1160     /* TCP连接断开告警(自定义) */

/* 告警有效性 */
#define ALARM_VALIDITY_VALID    0    /* 告警有效 */
#define ALARM_VALIDITY_INVALID  1    /* 告警不存在 */

/* 告警清除标志 */
#define ALARM_CLEAR_FLAG_ACTIVE  0   /* 告警产生/未清除 */
#define ALARM_CLEAR_FLAG_CLEARED 1   /* 告警已清除 */

/* 告警属性 */
typedef enum {
    ALARM_ATTR_FAULT,    /* 故障类 - 需要清除 */
    ALARM_ATTR_EVENT,    /* 事件类 - 发生即上报,无清除 */
    ALARM_ATTR_STATE     /* 状态类 - 状态翻转时上报 */
} alarm_attribute_t;

/* 告警上报IE结构 (IE 1001, 134字节payload) */
typedef struct __attribute__((packed)) {
    uint16_t validity;          /* 告警有效性 (0:有效, 1:告警不存在) */
    uint32_t alarm_code;        /* 告警码 */
    uint32_t sub_code;          /* 告警子码 */
    uint32_t clear_flag;        /* 告警清除标志 (0:产生, 1:清除) */
    char     timestamp[20];     /* 告警发生时间 (yyyy-mm-dd hh:mm:ss) */
    char     additional_info[100]; /* 附加信息 */
} alarm_report_ie_t;

/* 告警记录结构 */
typedef struct {
    bool     active;            /* 告警是否激活 */
    uint32_t alarm_code;        /* 告警码 */
    uint32_t sub_code;          /* 告警子码 */
    time_t   start_time;        /* 告警产生时间 */
    time_t   clear_time;        /* 告警清除时间 */
    bool     reported;          /* 是否已上报 */
    bool     clear_reported;    /* 清除是否已上报 */
    alarm_attribute_t attribute; /* 告警属性 */
    char     additional_info[100]; /* 附加信息 */
} alarm_record_t;

/* 告警状态跟踪 (用于检测状态变化) */
typedef struct {
    bool lo_unlock_active;              /* 本振失锁状态 */
    bool optical_sync_loss[8];          /* 光口0-7同步丢失状态 */
    bool over_temperature[10];          /* 温度探测点0-9过温状态 */
    bool channel_fault_exceed_active;   /* 通道故障过多状态 */
    bool tcp_disconnect_active;         /* TCP断开状态 */
} alarm_state_tracker_t;

/* 告警管理器 */
#define MAX_ALARM_RECORDS 64

typedef struct {
    alarm_record_t records[MAX_ALARM_RECORDS];
    alarm_state_tracker_t state_tracker;
    pthread_mutex_t mutex;
    uint32_t alarm_count;       /* 当前激活的告警数量 */
    uint32_t total_count;       /* 历史总告警数量 */
    bool tcp_connected;         /* TCP连接状态 */
    bool pending_tcp_alarm;     /* 是否有待发送的TCP断开告警 */
} alarm_manager_t;

/* 告警配置 */
typedef struct {
    uint8_t channel_fault_threshold;    /* 通道故障门限 */
    int8_t over_temp_threshold_high;    /* 过温门限(°C) */
    int8_t over_temp_threshold_low;     /* 低温门限(°C) */
} alarm_config_t;

/**
 * @brief 初始化告警管理器
 * @param config 告警配置
 * @return 0成功, -1失败
 */
int alarm_manager_init(const alarm_config_t *config);

/**
 * @brief 销毁告警管理器
 */
void alarm_manager_destroy(void);

/**
 * @brief 设置TCP连接状态
 * @param connected true:已连接, false:断开
 */
void alarm_set_tcp_status(bool connected);

/**
 * @brief 触发告警
 * @param alarm_code 告警码
 * @param sub_code 告警子码
 * @param additional_info 附加信息
 * @return 0成功, -1失败
 */
int alarm_trigger(uint32_t alarm_code, uint32_t sub_code, const char *additional_info);

/**
 * @brief 清除告警
 * @param alarm_code 告警码
 * @param sub_code 告警子码
 * @return 0成功, -1失败
 */
int alarm_clear(uint32_t alarm_code, uint32_t sub_code);

/**
 * @brief 检查告警是否激活
 * @param alarm_code 告警码
 * @param sub_code 告警子码
 * @return true激活, false未激活
 */
bool alarm_is_active(uint32_t alarm_code, uint32_t sub_code);

/**
 * @brief 上报告警到BBU
 * @param alarm_code 告警码
 * @param sub_code 告警子码
 * @param clear_flag 清除标志
 * @param additional_info 附加信息
 * @return 0成功, -1失败
 */
int alarm_report_to_bbu(uint32_t alarm_code, uint32_t sub_code,
                        uint32_t clear_flag, const char *additional_info);

/**
 * @brief 上报所有未上报的告警(TCP连接建立后调用)
 * @return 上报成功的数量
 */
int alarm_report_all_pending(void);

/* 特定告警检测函数 */

/**
 * @brief 检测本振失锁告警
 * @param lo_lock_status 本振锁定状态(从FPGA获取, 0:失锁, 非0:锁定)
 */
void alarm_check_lo_unlock(uint32_t lo_lock_status);

/**
 * @brief 检测光链路同步丢失告警
 * @param optical_port 光口号(0-7)
 * @param sync_status 同步状态(0:丢失, 1:正常)
 */
void alarm_check_optical_sync_loss(uint8_t optical_port, bool sync_status);

/**
 * @brief 检测过温告警
 * @param probe_id 温度探测点ID(0-9)
 * @param temperature 当前温度(°C)
 */
void alarm_check_over_temperature(uint8_t probe_id, int8_t temperature);

/**
 * @brief 检测通道故障过多告警
 * @param fault_count 当前故障通道数
 */
void alarm_check_channel_fault_exceed(uint8_t fault_count);

/**
 * @brief 触发版本激活失败告警
 * @param version_info 版本信息
 */
void alarm_trigger_version_activate_fail(const char *version_info);

/**
 * @brief 触发TCP断开告警
 * @param reason 断开原因
 */
void alarm_trigger_tcp_disconnect(const char *reason);

/**
 * @brief 清除TCP断开告警
 */
void alarm_clear_tcp_disconnect(void);

/**
 * @brief 周期性告警检测任务(从FPGA遥测数据检测)
 * @param fpga_status FPGA状态数据
 */
void alarm_periodic_check(const fpga_status_frame_t *fpga_status);

/**
 * @brief 获取告警名称
 * @param alarm_code 告警码
 * @return 告警名称字符串
 */
const char* alarm_get_name(uint32_t alarm_code);

/**
 * @brief 获取告警属性
 * @param alarm_code 告警码
 * @return 告警属性
 */
alarm_attribute_t alarm_get_attribute(uint32_t alarm_code);

/**
 * @brief 获取所有活跃告警列表
 * @param records 输出告警记录数组
 * @param max_count 数组最大容量
 * @param actual_count 实际返回的告警数量
 * @return 0成功, -1失败
 */
int alarm_get_active_alarms(alarm_record_t *records, int max_count, int *actual_count);

#endif /* ALARM_MANAGER_H */
