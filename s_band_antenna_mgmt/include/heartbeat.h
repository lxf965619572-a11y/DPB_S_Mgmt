#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "common.h"
#include "cpri_protocol.h"

/* 心跳参数 */
#define HEARTBEAT_INTERVAL_SEC      3       /* 发送间隔：3秒 */
#define HEARTBEAT_TIMEOUT_COUNT     20      /* 超时阈值：20次 */

/* 心跳管理器 */
typedef struct {
    uint32_t paau_heartbeat_count;      /* PAAU发送心跳计数 */
    uint32_t bbu_heartbeat_miss_count;  /* BBU心跳丢失计数 */
    time_t last_bbu_heartbeat_time;     /* 上次收到BBU心跳的时间 */
    bool heartbeat_enabled;             /* 心跳是否启用 */
    pthread_mutex_t mutex;              /* 互斥锁 */
} heartbeat_manager_t;

/* 初始化心跳管理器 */
int heartbeat_init(void);

/* 启动心跳（通道建立完成后调用） */
int heartbeat_start(void);

/* 停止心跳 */
int heartbeat_stop(void);

/* 发送PAAU心跳消息 */
int heartbeat_send_paau(void);

/* 处理收到的BBU心跳消息 */
int heartbeat_handle_bbu(const cpri_message_t *msg);

/* 检查BBU心跳超时 */
bool heartbeat_check_timeout(void);

/* 销毁心跳管理器 */
void heartbeat_destroy(void);

#endif /* HEARTBEAT_H */
